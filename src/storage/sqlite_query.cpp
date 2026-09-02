#include "duckdb.hpp"

#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "sqlite_scanner.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_table_entry.hpp"
#include "storage/sqlite_transaction.hpp"
#include "sqlite_db.hpp"
#include "sqlite_stmt.hpp"
#include "sqlite_utils.hpp"

namespace duckdb {

static unique_ptr<FunctionData> SQLiteQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<SqliteBindData>();

	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("Parameters to sqlite_query cannot be NULL");
	}

	// look up the database to query
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
	auto &sqlite_catalog = catalog.Cast<SQLiteCatalog>();
	auto &transaction = SQLiteTransaction::Get(context, catalog);
	auto sql = input.inputs[1].GetValue<string>();
	// strip any trailing semicolons
	StringUtil::RTrim(sql);
	while (!sql.empty() && sql.back() == ';') {
		sql = sql.substr(0, sql.size() - 1);
		StringUtil::RTrim(sql);
	}

	vector<Value> params;
	auto params_it = input.named_parameters.find("params");
	if (params_it != input.named_parameters.end()) {
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
	}

	auto &con = transaction.GetDB();
	auto stmt = con.Prepare(sql);
	if (!stmt.stmt) {
		throw BinderException("Failed to prepare query \"%s\"", sql);
	}
	if (stmt.GetColumnCount() > 0) {
		for (idx_t c = 0; c < stmt.GetColumnCount(); c++) {
			return_types.emplace_back(LogicalType::VARCHAR);
			names.emplace_back(Identifier(stmt.GetName(c)));
		}
	} else {
		return_types.emplace_back(LogicalType::BIGINT);
		names.emplace_back("rowcount");
		result->command_only = true;
	}
	result->rows_per_group = optional_idx();
	result->sql = std::move(sql);
	result->params = std::move(params);
	result->all_varchar = true;
	result->file_name = sqlite_catalog.GetDBPath();
	result->catalog = &sqlite_catalog;
	return std::move(result);
}

SQLiteQueryFunction::SQLiteQueryFunction()
    : TableFunction("sqlite_query", {LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, SQLiteQueryBind) {
	SqliteScanFunction scan_function;
	init_global = scan_function.init_global;
	init_local = scan_function.init_local;
	function = scan_function.function;
	global_initialization = TableFunctionInitialization::INITIALIZE_ON_SCHEDULE;
	named_parameters["params"] = LogicalType::ANY;
}
} // namespace duckdb
