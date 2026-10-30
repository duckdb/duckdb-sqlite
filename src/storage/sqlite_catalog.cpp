#include "storage/sqlite_catalog.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/storage/database_size.hpp"
#include "sqlite_db.hpp"
#include "sqlite_duckdb_vfs_cache.hpp"
#include "duckdb/common/file_system.hpp"
#include "storage/sqlite_schema_entry.hpp"
#include "storage/sqlite_transaction.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "sqlite_sql_writer.hpp"
#include "sqlite_stmt.hpp"

namespace duckdb {

SQLiteCatalog::SQLiteCatalog(AttachedDatabase &db_p, const string &path, SQLiteOpenOptions options_p)
    : Catalog(db_p), path(path), options(std::move(options_p)), in_memory(path == ":memory:"), active_in_memory(false),
      capabilities(options.access_mode == AccessMode::READ_ONLY, db_p.GetName()) {
	if (InMemory()) {
		in_memory_db = SQLiteDB::Open(path, options, true);
	} else if (options.keep_database_open && !FileSystem::IsRemoteFile(path) &&
	           !SQLiteDuckDBCacheVFS::CanHandlePath(path)) {
		// opt-in (SET sqlite_keep_database_open): keep one handle open so per-transaction handles are never
		// the last connection (see anchor_db). Remote / VFS-backed databases are read-only and never checkpoint.
		anchor_db = SQLiteDB::Open(path, options, true);
		// SQLite only counts a connection as a WAL participant (holder of the -shm "DMS" lock) once
		// it has read from the database; a handle that was merely opened does not stop the others
		// from checkpointing and unlinking the WAL on close. Touch the schema once so this one does.
		anchor_db.Execute("SELECT 1 FROM sqlite_master LIMIT 1");
	}
}

SQLiteCatalog::~SQLiteCatalog() {
}

void SQLiteCatalog::Initialize(bool load_builtin) {
	CreateSchemaInfo info;
	main_schema = make_uniq<SQLiteSchemaEntry>(*this, info);
}

optional_ptr<CatalogEntry> SQLiteCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	if (info.SchemaName() == Identifier::DefaultSchema()) {
		// CREATE SCHEMA is called when doing COPY DATABASE from SQLite to SQLite
		return main_schema.get();
	}
	throw BinderException("SQLite databases do not support creating new schemas");
}

void SQLiteCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	callback(*main_schema);
}

optional_ptr<SchemaCatalogEntry> SQLiteCatalog::LookupSchema(CatalogTransaction transaction,
                                                             const EntryLookupInfo &schema_lookup,
                                                             OnEntryNotFound if_not_found) {
	auto &schema_name = schema_lookup.GetEntryName();
	if (schema_name == DEFAULT_SCHEMA || schema_name == INVALID_SCHEMA) {
		return main_schema.get();
	}
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return nullptr;
	}
	throw BinderException("SQLite databases only have a single schema - \"%s\"", std::string(DEFAULT_SCHEMA));
}

bool SQLiteCatalog::InMemory() {
	return in_memory;
}

string SQLiteCatalog::GetDBPath() {
	return path;
}

SQLiteDB *SQLiteCatalog::GetInMemoryDatabase() {
	if (!InMemory()) {
		throw InternalException("GetInMemoryDatabase() called on a non-in-memory database");
	}
	lock_guard<mutex> l(in_memory_lock);
	if (active_in_memory) {
		throw TransactionException("Only a single transaction can be active on an "
		                           "in-memory SQLite database at a time");
	}
	active_in_memory = true;
	return &in_memory_db;
}

void SQLiteCatalog::ReleaseInMemoryDatabase() {
	if (!InMemory()) {
		return;
	}
	lock_guard<mutex> l(in_memory_lock);
	if (!active_in_memory) {
		throw InternalException("ReleaseInMemoryDatabase called but there is no "
		                        "active transaction on an in-memory database");
	}
	active_in_memory = false;
}

void SQLiteCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	throw BinderException("SQLite databases do not support dropping schemas");
}

DatabaseSize SQLiteCatalog::GetDatabaseSize(ClientContext &context) {
	DatabaseSize result;

	auto &transaction = SQLiteTransaction::Get(context, *this);
	auto &db = transaction.GetDB();
	result.total_blocks = db.RunPragma("page_count");
	result.block_size = db.RunPragma("page_size");
	result.free_blocks = db.RunPragma("freelist_count");
	result.used_blocks = result.total_blocks - result.free_blocks;
	result.bytes = result.total_blocks * result.block_size;
	result.wal_size = idx_t(-1);
	return result;
}

dbconnector::attached::AttachedCatalog SQLiteCatalog::Lookup(ClientContext &ctx, const Identifier &name) {
	using namespace dbconnector::attached;

	AttachedCatalog attached_catalog = AttachedCatalog::Lookup(ctx, "sqlite", name);
	if (!attached_catalog) {
		throw InvalidInputException("Attached SQLite database not found in the specified client session, name: %s",
		                            name);
	}
	return attached_catalog;
}

//===--------------------------------------------------------------------===//
// Remote pushdown
//===--------------------------------------------------------------------===//
template <class T>
static bool SQLiteCheckPushdown(bool enabled, SQLiteCapabilities &capabilities, const T &construct,
                                const char *construct_name) {
	if (!enabled) {
		return false;
	}
	auto result = capabilities.SupportsPushdown(construct);
	if (!result && SQLiteDB::DebugPrintQueries()) {
		Printer::Print("Remote pushdown: " + string(construct_name) + " not supported: " + construct.ToString());
	}
	return result;
}

bool SQLiteCatalog::SupportsPushdown(const ParsedExpression &expression) {
	return SQLiteCheckPushdown(options.remote_pushdown, capabilities, expression, "expression");
}

bool SQLiteCatalog::SupportsPushdown(const TableRef &ref) {
	return SQLiteCheckPushdown(options.remote_pushdown, capabilities, ref, "table reference");
}

bool SQLiteCatalog::SupportsPushdown(const QueryNode &node) {
	return SQLiteCheckPushdown(options.remote_pushdown, capabilities, node, "query");
}

bool SQLiteCatalog::SupportsPushdown(const SQLStatement &statement) {
	return SQLiteCheckPushdown(options.remote_pushdown, capabilities, statement, "statement");
}

static bool SQLiteIsSimpleFilter(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::CONJUNCTION:
		for (auto &child : expr.Cast<ConjunctionExpression>().GetChildren()) {
			if (!SQLiteIsSimpleFilter(*child)) {
				return false;
			}
		}
		return true;
	case ExpressionClass::COMPARISON: {
		auto &comparison = expr.Cast<ComparisonExpression>();
		auto left_class = comparison.Left().GetExpressionClass();
		auto right_class = comparison.Right().GetExpressionClass();
		return (left_class == ExpressionClass::COLUMN_REF && right_class == ExpressionClass::CONSTANT) ||
		       (left_class == ExpressionClass::CONSTANT && right_class == ExpressionClass::COLUMN_REF);
	}
	case ExpressionClass::OPERATOR: {
		auto &children = expr.Cast<OperatorExpression>().GetChildren();
		switch (expr.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			return children[0]->GetExpressionClass() == ExpressionClass::COLUMN_REF;
		case ExpressionType::COMPARE_IN:
			if (children[0]->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
				return false;
			}
			for (idx_t i = 1; i < children.size(); i++) {
				if (children[i]->GetExpressionClass() != ExpressionClass::CONSTANT) {
					return false;
				}
			}
			return true;
		default:
			return false;
		}
	}
	default:
		return false;
	}
}

//! Whether a query is a plain scan of a single table - those are executed through a regular table scan, which reads
//! the table in parallel (by rowid ranges) and pushes down projections and simple filters as well
static bool SQLiteIsTrivialQuery(const QueryNode &node, const identifier_set_t &cte_names) {
	if (node.type != QueryNodeType::SELECT_NODE || !node.cte_map.map.empty()) {
		return false;
	}
	for (auto &modifier : node.modifiers) {
		// a LIMIT is applied by the table scan as well
		if (modifier->type != ResultModifierType::LIMIT_MODIFIER || modifier->Cast<LimitModifier>().offset) {
			return false;
		}
	}
	auto &select = node.Cast<SelectNode>();
	if (!select.from_table || select.from_table->type != TableReferenceType::BASE_TABLE ||
	    !select.groups.grouping_sets.empty() || select.having || select.qualify || select.sample ||
	    select.aggregate_handling != AggregateHandling::STANDARD_HANDLING) {
		return false;
	}
	auto &base = select.from_table->Cast<BaseTableRef>();
	if (base.sample || cte_names.count(base.Table()) > 0) {
		return false;
	}
	for (auto &expr : select.select_list) {
		auto expr_class = expr->GetExpressionClass();
		if (expr_class != ExpressionClass::COLUMN_REF && expr_class != ExpressionClass::STAR) {
			return false;
		}
	}
	return !select.where_clause || SQLiteIsSimpleFilter(*select.where_clause);
}

static bool SQLiteIsDMLWithoutReturning(const QueryNode &node) {
	switch (node.type) {
	case QueryNodeType::UPDATE_QUERY_NODE:
		return node.Cast<UpdateQueryNode>().returning_list.empty();
	case QueryNodeType::DELETE_QUERY_NODE:
		return node.Cast<DeleteQueryNode>().returning_list.empty();
	case QueryNodeType::INSERT_QUERY_NODE:
		return node.Cast<InsertQueryNode>().returning_list.empty();
	default:
		return false;
	}
}

static unique_ptr<TableRef> SQLiteExecuteLocally(unique_ptr<QueryNode> node) {
	auto select = make_uniq<SelectStatement>();
	select->node = std::move(node);
	return make_uniq<SubqueryRef>(std::move(select));
}

static void SQLitePrintPushdownFallback(const string &reason) {
	if (SQLiteDB::DebugPrintQueries()) {
		Printer::Print("Remote pushdown: executing locally, " + reason);
	}
}

bool SQLiteCatalog::HasUTF8Encoding(ClientContext &context) {
	lock_guard<mutex> guard(encoding_lock);
	if (!utf8_encoding.IsValid()) {
		auto &db = SQLiteTransaction::Get(context, *this).GetDB();
		auto stmt = db.Prepare("PRAGMA encoding");
		utf8_encoding = stmt.Step() && stmt.GetValue<string>(0) == "UTF-8" ? 1 : 0;
	}
	return utf8_encoding.GetIndex() == 1;
}

static unique_ptr<ParsedExpression> SQLiteNamedParameter(const char *name, Value value) {
	return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>(name),
	                                       ConstantExpression::FromValue(std::move(value)));
}

unique_ptr<TableRef> SQLiteCatalog::RemoteExecute(ClientContext &context, unique_ptr<QueryNode> node) {
	// bind the query locally to figure out the result types and names DuckDB would produce
	identifier_set_t cte_names;
	SQLiteSQLWriter::CollectCTENames(*node, cte_names);
	auto local_node = node->Copy();
	SQLiteSQLWriter::QualifyTables(*local_node, GetName(), identifier_set_t());
	if (SQLiteIsTrivialQuery(*node, cte_names)) {
		return SQLiteExecuteLocally(std::move(local_node));
	}
	if (!HasUTF8Encoding(context)) {
		// strings are not ordered like DuckDB orders them
		SQLitePrintPushdownFallback("the database does not use the UTF-8 encoding");
		return SQLiteExecuteLocally(std::move(local_node));
	}
	if (!Settings::Get<DefaultCollationSetting>(context).empty()) {
		// DuckDB compares the strings with the collation
		SQLitePrintPushdownFallback("default_collation is set");
		return SQLiteExecuteLocally(std::move(local_node));
	}
	BoundStatement bound;
	try {
		auto bind_node = local_node->Copy();
		auto binder = Binder::CreateBinder(context);
		bound = binder->Bind(*bind_node);
	} catch (std::exception &ex) {
		// the query cannot be bound - execute it locally so that DuckDB reports the error
		SQLitePrintPushdownFallback("the query cannot be bound: " + string(ex.what()));
		return SQLiteExecuteLocally(std::move(local_node));
	}
	for (auto &type : bound.types) {
		if (!TypeExpression::CanRepresent(type)) {
			SQLitePrintPushdownFallback("unsupported result type " + type.ToString());
			return SQLiteExecuteLocally(std::move(local_node));
		}
	}

	SQLiteWriterResult query;
	try {
		query = SQLiteSQLWriter::WriteQuery(context, *this, *node);
	} catch (SQLitePushdownNotSupported &ex) {
		// the query contains a construct that cannot be translated for the types it operates on
		SQLitePrintPushdownFallback(ex.what());
		return SQLiteExecuteLocally(std::move(local_node));
	}
	{
		// verify that SQLite accepts the query (e.g. no LATERAL references, which SQLite does not support)
		// a double-quoted identifier that does not resolve would be read as a string literal by SQLite
		auto &db = SQLiteTransaction::Get(context, *this).GetDB();
		string error;
		if (!db.TryPrepareStrict(query.sql, error)) {
			SQLitePrintPushdownFallback("SQLite cannot prepare the query: " + error);
			return SQLiteExecuteLocally(std::move(local_node));
		}
	}
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(ConstantExpression::FromValue(Value(GetName().GetIdentifierName())));
	args.push_back(ConstantExpression::FromValue(Value(query.sql)));
	bool changed_rows = SQLiteIsDMLWithoutReturning(*node);
	bool is_dml = node->type == QueryNodeType::UPDATE_QUERY_NODE || node->type == QueryNodeType::DELETE_QUERY_NODE ||
	              node->type == QueryNodeType::INSERT_QUERY_NODE;
	if (is_dml && !changed_rows) {
		// a DML statement with RETURNING - which sqlite_query cannot tell from its result
		args.push_back(SQLiteNamedParameter("modifies_database", Value::BOOLEAN(true)));
	}
	if (changed_rows) {
		// the statement yields the number of changed rows
		args.push_back(SQLiteNamedParameter("return_changed_rows", Value::BOOLEAN(true)));
	} else {
		vector<Value> type_values;
		for (auto &type : bound.types) {
			type_values.emplace_back(type.ToString());
		}
		args.push_back(SQLiteNamedParameter("column_types", Value::LIST(LogicalType::VARCHAR, std::move(type_values))));
	}
	auto func_ref = make_uniq<TableFunctionRef>();
	func_ref->function = make_uniq<FunctionExpression>("sqlite_query", std::move(args));
	func_ref->alias = Identifier("sqlite_query");

	// name the result columns the way DuckDB would name them
	auto select_node = make_uniq<SelectNode>();
	for (idx_t i = 0; i < bound.types.size(); i++) {
		auto column_name = Identifier("c" + to_string(i));
		func_ref->column_name_alias.push_back(column_name);
		unique_ptr<ParsedExpression> expr = make_uniq<ColumnRefExpression>(column_name, Identifier("sqlite_query"));
		expr->SetAlias(bound.names[i]);
		select_node->select_list.push_back(std::move(expr));
	}
	select_node->from_table = std::move(func_ref);
	auto select = make_uniq<SelectStatement>();
	select->node = std::move(select_node);
	return make_uniq<SubqueryRef>(std::move(select));
}

unique_ptr<TableRef> SQLiteCatalog::RemoteExecute(ClientContext &context, unique_ptr<SQLStatement> statement) {
	SQLiteWriterResult query;
	try {
		query = SQLiteSQLWriter::WriteStatement(context, *this, *statement);
	} catch (SQLitePushdownNotSupported &ex) {
		// statements cannot be executed locally instead
		throw NotImplementedException("The statement cannot be executed in SQLite (%s) - use \"SET "
		                              "sqlite_enable_remote_pushdown = false\" before attaching the database to "
		                              "execute it without remote pushdown",
		                              ex.what());
	}
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(ConstantExpression::FromValue(Value(GetName().GetIdentifierName())));
	args.push_back(ConstantExpression::FromValue(Value(query.sql)));
	if (!query.preliminary_queries.empty()) {
		vector<Value> preliminary_values;
		for (auto &preliminary_query : query.preliminary_queries) {
			preliminary_values.emplace_back(preliminary_query);
		}
		args.push_back(SQLiteNamedParameter("preliminary_queries",
		                                    Value::LIST(LogicalType::VARCHAR, std::move(preliminary_values))));
	}
	if (query.clear_catalog_cache) {
		args.push_back(SQLiteNamedParameter("clear_catalog_cache", Value::BOOLEAN(true)));
	}
	if (query.changed_rows) {
		args.push_back(SQLiteNamedParameter("return_changed_rows", Value::BOOLEAN(true)));
	} else {
		args.push_back(SQLiteNamedParameter("suppress_output", Value::BOOLEAN(true)));
	}
	auto func_ref = make_uniq<TableFunctionRef>();
	func_ref->function = make_uniq<FunctionExpression>("sqlite_query", std::move(args));
	return std::move(func_ref);
}

} // namespace duckdb
