#include "duckdb.hpp"

#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "sqlite_scanner.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/planner/binder.hpp"
#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_table_entry.hpp"
#include "storage/sqlite_transaction.hpp"
#include "sqlite_db.hpp"
#include "sqlite_stmt.hpp"
#include "sqlite_utils.hpp"

namespace duckdb {

static bool SQLiteQueryFlag(TableFunctionBindInput &input, const char *name) {
	auto entry = input.named_parameters.find(name);
	return entry != input.named_parameters.end() && !entry->second.IsNull() && BooleanValue::Get(entry->second);
}

//! The attached SQLite database the query is executed in
static SQLiteCatalog &ExtractCatalog(ClientContext &context, TableFunctionBindInput &input) {
	auto db_name = input.inputs[0].GetValue<string>();
	auto &db_manager = DatabaseManager::Get(context);
	auto db = db_manager.GetDatabase(context, Identifier(db_name));
	if (!db) {
		throw BinderException("Failed to find attached database \"%s\" referenced in sqlite_query", db_name);
	}
	auto &catalog = db->GetCatalog();
	if (catalog.GetCatalogType() != "sqlite") {
		throw BinderException("Attached database \"%s\" does not refer to a SQLite database", db_name);
	}
	return catalog.Cast<SQLiteCatalog>();
}

//! The query, without trailing semicolons
static string ExtractSQL(TableFunctionBindInput &input) {
	auto sql = input.inputs[1].GetValue<string>();
	StringUtil::RTrim(sql);
	while (!sql.empty() && sql.back() == ';') {
		sql = sql.substr(0, sql.size() - 1);
		StringUtil::RTrim(sql);
	}
	return sql;
}

//! The values of the ? parameters of the query (params)
static vector<Value> ExtractParams(TableFunctionBindInput &input) {
	vector<Value> params;
	auto params_it = input.named_parameters.find("params");
	if (params_it == input.named_parameters.end()) {
		return params;
	}
	Value &struct_val = params_it->second;
	if (struct_val.IsNull()) {
		throw BinderException("Parameters to sqlite_query cannot be NULL");
	}
	if (struct_val.type().id() != LogicalTypeId::STRUCT && struct_val.type().id() != LogicalTypeId::TUPLE) {
		throw BinderException("Query parameters must be specified in a STRUCT");
	}
	params = StructValue::GetChildren(struct_val);
	for (idx_t i = 0; i < params.size(); i++) {
		const Value &param = params[i];
		switch (param.type().id()) {
		case LogicalTypeId::BIGINT:
		case LogicalTypeId::DOUBLE:
		case LogicalTypeId::BLOB:
		case LogicalTypeId::VARCHAR:
			break;
		default:
			if (!param.IsNull()) {
				throw BinderException("Unsupported parameter type \"%s\", index: %zu", param.type().ToString(), i);
			}
		}
	}
	return params;
}

//! The strings of a named LIST(VARCHAR) parameter (empty if it is not given)
static vector<string> ExtractStringList(TableFunctionBindInput &input, const char *name) {
	vector<string> result;
	auto entry = input.named_parameters.find(name);
	if (entry == input.named_parameters.end() || entry->second.IsNull()) {
		return result;
	}
	for (auto &value : ListValue::GetChildren(entry->second)) {
		if (value.IsNull()) {
			throw BinderException("The %s of sqlite_query cannot contain NULL", name);
		}
		result.push_back(StringValue::Get(value));
	}
	return result;
}

//! The types of the result columns (column_types)
static vector<LogicalType> ExtractColumnTypes(ClientContext &context, TableFunctionBindInput &input) {
	vector<LogicalType> column_types;
	auto types_it = input.named_parameters.find("column_types");
	if (types_it == input.named_parameters.end() || types_it->second.IsNull()) {
		return column_types;
	}
	for (auto &type_name : ExtractStringList(input, "column_types")) {
		column_types.push_back(TransformStringToLogicalType(type_name, context));
	}
	if (column_types.empty()) {
		throw BinderException("The column_types of sqlite_query cannot be empty");
	}
	return column_types;
}

//! The names of the result columns (column_names) - only together with column_types
static vector<string> ExtractColumnNames(TableFunctionBindInput &input, const vector<LogicalType> &column_types) {
	auto names_it = input.named_parameters.find("column_names");
	if (names_it == input.named_parameters.end() || names_it->second.IsNull()) {
		return vector<string>();
	}
	auto column_names = ExtractStringList(input, "column_names");
	if (column_types.empty()) {
		throw BinderException("column_names can only be specified together with column_types");
	}
	if (column_names.size() != column_types.size()) {
		throw BinderException("%llu column_names were specified, but %llu column_types", column_names.size(),
		                      column_types.size());
	}
	return column_names;
}

static unique_ptr<FunctionData> SQLiteQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<SqliteBindData>();

	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("Parameters to sqlite_query cannot be NULL");
	}

	auto &sqlite_catalog = ExtractCatalog(context, input);
	auto &transaction = SQLiteTransaction::Get(context, sqlite_catalog);
	auto sql = ExtractSQL(input);
	auto params = ExtractParams(input);
	auto column_types = ExtractColumnTypes(context, input);
	auto column_names = ExtractColumnNames(input, column_types);
	result->preliminary_queries = ExtractStringList(input, "preliminary_queries");
	result->clear_catalog_cache = SQLiteQueryFlag(input, "clear_catalog_cache");

	auto &con = transaction.GetDB();
	bool suppress_output = SQLiteQueryFlag(input, "suppress_output");
	// whether the statement writes to the database - statements without a result are assumed to write
	bool modifies_database = SQLiteQueryFlag(input, "modifies_database") || !result->preliminary_queries.empty();
	if (suppress_output) {
		// the statement replaces a DuckDB statement without a result (e.g. DDL) - it is executed as a command
		if (!column_types.empty()) {
			throw BinderException("column_types cannot be combined with suppress_output");
		}
	} else if (!column_types.empty()) {
		// the result is described by the column types - the query is not prepared before it is executed (it can also
		// depend on the preliminary queries, e.g. on a table they create)
		for (idx_t c = 0; c < column_types.size(); c++) {
			names.emplace_back(c < column_names.size() ? column_names[c] : "c" + to_string(c));
		}
	} else if (!result->preliminary_queries.empty()) {
		// the statement can depend on the preliminary queries - it cannot be prepared yet, and without column types
		// it is executed as a command
	} else {
		auto stmt = con.Prepare(sql);
		if (!stmt.stmt) {
			throw BinderException("Failed to prepare query \"%s\"", sql);
		}
		if (!sqlite3_stmt_readonly(stmt.stmt)) {
			modifies_database = true;
		}
		for (idx_t c = 0; c < stmt.GetColumnCount(); c++) {
			names.emplace_back(Identifier(stmt.GetName(c)));
		}
	}
	if (!column_types.empty()) {
		return_types = std::move(column_types);
		result->typed_result = true;
	} else if (!names.empty()) {
		return_types.resize(names.size(), LogicalType::VARCHAR);
		result->all_varchar = true;
	} else {
		return_types.emplace_back(LogicalType::BIGINT);
		names.emplace_back("rowcount");
		result->command_only = true;
		modifies_database = true;
		if (suppress_output) {
			input.table_function.call_return_type = StatementReturnType::NOTHING;
		} else if (SQLiteQueryFlag(input, "return_changed_rows")) {
			// the statement replaces a DuckDB DML statement - report the changed rows like DuckDB does
			input.table_function.call_return_type = StatementReturnType::CHANGED_ROWS;
		}
	}
	if (input.binder) {
		// let DuckDB check the transaction (e.g. that a read-only transaction does not write), and rebind prepared
		// statements when the catalog changed
		auto &properties = input.binder->GetStatementProperties();
		properties.RegisterDBRead(sqlite_catalog, context);
		if (modifies_database) {
			// statements that clear the catalog cache are DDL
			DatabaseModificationType modification = result->clear_catalog_cache ? DatabaseModificationType::ALTER_TABLE
			                                                                    : DatabaseModificationType::UPDATE_DATA;
			properties.RegisterDBModify(sqlite_catalog, context, modification);
		}
	}
	result->types = return_types;
	for (auto &name : names) {
		result->names.push_back(name.GetIdentifierName());
	}
	result->rows_per_group = optional_idx();
	result->sql = std::move(sql);
	result->params = std::move(params);
	result->file_name = sqlite_catalog.GetDBPath();
	result->catalog_name = sqlite_catalog.GetName();
	return std::move(result);
}

static FunctionSignature SQLiteQuerySignature() {
	FunctionSignature signature;
	signature.AddParameter("database", LogicalType::VARCHAR)
	    .AddParameter("sql", LogicalType::VARCHAR)
	    .WithTypedKwargs("options", [&](TypedKwargs &options) {
		    options.Add("params", LogicalType::ANY);
		    options.Add("column_types", LogicalType::LIST(LogicalType::VARCHAR));
		    options.Add("column_names", LogicalType::LIST(LogicalType::VARCHAR));
		    options.Add("preliminary_queries", LogicalType::LIST(LogicalType::VARCHAR));
		    options.Add("return_changed_rows", LogicalType::BOOLEAN);
		    options.Add("suppress_output", LogicalType::BOOLEAN);
		    options.Add("clear_catalog_cache", LogicalType::BOOLEAN);
		    options.Add("modifies_database", LogicalType::BOOLEAN);
	    });
	return signature;
}

static InsertionOrderPreservingMap<string> SQLiteQueryToString(TableFunctionToStringInput &input) {
	D_ASSERT(input.bind_data);
	InsertionOrderPreservingMap<string> result;
	auto &bind_data = input.bind_data->Cast<SqliteBindData>();
	if (!bind_data.preliminary_queries.empty()) {
		result["Preliminary Queries"] = StringUtil::Join(bind_data.preliminary_queries, "\n");
	}
	result["SQL"] = bind_data.sql;
	return result;
}

SQLiteQueryFunction::SQLiteQueryFunction()
    : TableFunction("sqlite_query", SQLiteQuerySignature(), nullptr, SQLiteQueryBind) {
	SqliteScanFunction scan_function;
	init_global = scan_function.init_global;
	init_local = scan_function.init_local;
	function = scan_function.function;
	to_string = SQLiteQueryToString;
	global_initialization = TableFunctionInitialization::INITIALIZE_ON_SCHEDULE;
}
} // namespace duckdb
