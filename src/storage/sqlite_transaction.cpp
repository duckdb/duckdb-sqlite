#include "storage/sqlite_transaction.hpp"
#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_index_entry.hpp"
#include "storage/sqlite_schema_entry.hpp"
#include "storage/sqlite_table_entry.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/common/file_system.hpp"

namespace duckdb {

class SQLiteCatalogMap {
public:
	optional_ptr<CatalogEntry> InsertEntry(const string &entry_name, unique_ptr<CatalogEntry> entry);
	optional_ptr<CatalogEntry> GetEntry(const string &entry_name);
	void EraseEntry(const string &entry_name);

private:
	mutex lock;
	case_insensitive_map_t<unique_ptr<CatalogEntry>> catalog_entries;
};

optional_ptr<CatalogEntry> SQLiteCatalogMap::InsertEntry(const string &entry_name,
                                                         unique_ptr<CatalogEntry> catalog_entry) {
	lock_guard<mutex> guard(lock);
	auto entry = catalog_entries.find(entry_name);
	if (entry != catalog_entries.end()) {
		return entry->second.get();
	}
	auto &result = *catalog_entry;
	catalog_entries[entry_name] = std::move(catalog_entry);
	return result;
}

optional_ptr<CatalogEntry> SQLiteCatalogMap::GetEntry(const string &entry_name) {
	lock_guard<mutex> guard(lock);
	auto entry = catalog_entries.find(entry_name);
	if (entry != catalog_entries.end()) {
		return entry->second.get();
	}
	return nullptr;
}

void SQLiteCatalogMap::EraseEntry(const string &entry_name) {
	lock_guard<mutex> guard(lock);
	catalog_entries.erase(entry_name);
}

SQLiteTransaction::SQLiteTransaction(SQLiteCatalog &sqlite_catalog, TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context), sqlite_catalog(sqlite_catalog), db(nullptr) {
	if (sqlite_catalog.InMemory()) {
		db = sqlite_catalog.GetInMemoryDatabase();
		initialized.store(true, std::memory_order_release);
	} else if (!FileSystem::IsRemoteFile(sqlite_catalog.path)) {
		// local on-disk database - open eagerly (cheap; surfaces open errors at attach time)
		owned_db = SQLiteDB::Open(sqlite_catalog.path, sqlite_catalog.options, context, true);
		db = &owned_db;
		initialized.store(true, std::memory_order_release);
	}
	// remote database - defer the connection open and BEGIN to the first GetDB(). A remote open
	// issues uncached network probes (the -wal/-journal sidecar HEADs); deferring runs that I/O on
	// first use rather than eagerly in StartTransaction, and skips it for a database that is attached
	// but never accessed.
	catalog_map = make_uniq<SQLiteCatalogMap>();
}

SQLiteTransaction::~SQLiteTransaction() {
	sqlite_catalog.ReleaseInMemoryDatabase();
}

void SQLiteTransaction::Start() {
	// Local/in-memory transactions begin here; remote transactions are deferred (db is not yet
	// open) so this is a no-op for them and BEGIN runs in GetDB() on first use.
	if (initialized.load(std::memory_order_acquire) && !started) {
		db->Execute("BEGIN TRANSACTION");
		started = true;
	}
}
void SQLiteTransaction::Commit() {
	// Skip when the transaction never began (e.g. a remote DB attached but never queried).
	if (started) {
		db->Execute("COMMIT");
	}
}
void SQLiteTransaction::Rollback() {
	if (started) {
		db->Execute("ROLLBACK");
	}
}

SQLiteDB &SQLiteTransaction::GetDB() {
	// Fast path: local and in-memory databases are initialized in the constructor.
	if (initialized.load(std::memory_order_acquire)) {
		return *db;
	}
	// Slow path (remote only): open the connection and begin the transaction on first use, outside
	// the MetaTransaction lock. A per-transaction lock guards the init (parallel scans may race it).
	lock_guard<mutex> guard(init_lock);
	if (!initialized.load(std::memory_order_relaxed)) {
		auto client_context = context.lock();
		if (!client_context) {
			throw TransactionException("ClientContext expired before the remote SQLite connection could be opened");
		}
		auto new_db = SQLiteDB::Open(sqlite_catalog.path, sqlite_catalog.options, *client_context, true);
		new_db.Execute("BEGIN TRANSACTION");
		owned_db = std::move(new_db);
		db = &owned_db;
		// Set started before publishing initialized: the release-store on initialized also makes
		// this plain write visible to any thread that later observes initialized via acquire.
		started = true;
		initialized.store(true, std::memory_order_release);
	}
	return *db;
}

SQLiteTransaction &SQLiteTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<SQLiteTransaction>();
}

string ExtractSelectStatement(const string &create_view) {
	Parser parser;
	parser.ParseQuery(create_view);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::CREATE_STATEMENT) {
		throw BinderException(
		    "Failed to create view from SQL string - \"%s\" - statement did not contain a single CREATE VIEW statement",
		    create_view);
	}
	auto &create_statement = parser.statements[0]->Cast<CreateStatement>();
	if (create_statement.info->type != CatalogType::VIEW_ENTRY) {
		throw BinderException(
		    "Failed to create view from SQL string - \"%s\" - view did not contain a CREATE VIEW statement",
		    create_view);
	}
	auto &view_info = create_statement.info->Cast<CreateViewInfo>();
	return view_info.query->ToString();
}

void ExtractColumnIds(const ParsedExpression &expr, TableCatalogEntry &table, CreateIndexInfo &info) {
	if (expr.GetExpressionType() == ExpressionType::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		auto &colname = colref.GetColumnName();
		auto &column_def = table.GetColumn(Identifier(colname));
		auto index = column_def.Oid();
		if (std::find(info.column_ids.begin(), info.column_ids.end(), index) == info.column_ids.end()) {
			info.column_ids.push_back(index);
		}
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { ExtractColumnIds(child, table, info); });
}

unique_ptr<CreateIndexInfo> FromCreateIndex(ClientContext &context, TableCatalogEntry &table, string sql) {
	// parse the SQL statement
	Parser parser;
	parser.ParseQuery(sql);

	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::CREATE_STATEMENT) {
		throw BinderException("Failed to create index from SQL string - \"%s\" - statement did not contain a single "
		                      "CREATE INDEX statement",
		                      sql);
	}
	auto &create_statement = parser.statements[0]->Cast<CreateStatement>();
	if (create_statement.info->type != CatalogType::INDEX_ENTRY) {
		throw BinderException(
		    "Failed to create view from SQL string - \"%s\" - view did not contain a CREATE INDEX statement", sql);
	}
	auto info = unique_ptr_cast<CreateInfo, CreateIndexInfo>(std::move(create_statement.info));
	info->sql = std::move(sql);
	for (auto &expr : info->expressions) {
		ExtractColumnIds(*expr, table, *info);
	}
	return info;
}

optional_ptr<CatalogEntry> SQLiteTransaction::GetCatalogEntry(const string &entry_name) {
	auto entry = catalog_map->GetEntry(entry_name);
	if (entry) {
		return entry;
	}
	// catalog entry not found - look up table in main SQLite database
	auto type = GetDB().GetEntryType(entry_name);
	if (type == CatalogType::INVALID) {
		// no table or view found
		return nullptr;
	}
	unique_ptr<CatalogEntry> result;
	switch (type) {
	case CatalogType::TABLE_ENTRY: {
		CreateTableInfo info(sqlite_catalog.GetMainSchema(), Identifier(entry_name));
		bool all_varchar = false;
		Value sqlite_all_varchar;
		if (context.lock()->TryGetCurrentSetting("sqlite_all_varchar", sqlite_all_varchar)) {
			all_varchar = BooleanValue::Get(sqlite_all_varchar);
		}
		GetDB().GetTableInfo(entry_name, info.columns, info.constraints, all_varchar);
		D_ASSERT(!info.columns.empty());

		result = make_uniq<SQLiteTableEntry>(sqlite_catalog, sqlite_catalog.GetMainSchema(), info, all_varchar);
		break;
	}
	case CatalogType::VIEW_ENTRY: {
		string sql;
		GetDB().GetViewInfo(entry_name, sql);

		unique_ptr<CreateViewInfo> view_info;
		try {
			view_info = CreateViewInfo::FromCreateView(*context.lock(), sqlite_catalog.GetMainSchema(), sql);
		} catch (std::exception &ex) {
			auto view_sql = ExtractSelectStatement(sql);
			auto catalog_name = StringUtil::Replace(sqlite_catalog.GetName().GetIdentifierName(), "\"", "\"\"");
			auto escaped_view_sql = StringUtil::Replace(view_sql, "'", "''");
			auto view_def = StringUtil::Format("CREATE VIEW %s AS FROM sqlite_query(\"%s\", '%s')", entry_name,
			                                   catalog_name, escaped_view_sql);
			view_info = CreateViewInfo::FromCreateView(*context.lock(), sqlite_catalog.GetMainSchema(), view_def);
		}
		view_info->internal = false;
		result = make_uniq<ViewCatalogEntry>(sqlite_catalog, sqlite_catalog.GetMainSchema(), *view_info);
		break;
	}
	case CatalogType::INDEX_ENTRY: {
		string table_name;
		string sql;
		GetDB().GetIndexInfo(entry_name, sql, table_name);
		if (sql.empty()) {
			throw InternalException("SQL is empty");
		}
		auto &table = GetCatalogEntry(table_name)->Cast<TableCatalogEntry>();
		auto index_info = FromCreateIndex(*context.lock(), table, std::move(sql));
		index_info->SetQualifiedName(QualifiedName(sqlite_catalog.GetName(), index_info->GetQualifiedName().Schema(),
		                                           index_info->GetQualifiedName().Name()));

		auto index_entry = make_uniq<SQLiteIndexEntry>(sqlite_catalog, sqlite_catalog.GetMainSchema(), *index_info,
		                                               std::move(table_name));
		result = std::move(index_entry);
		break;
	}
	default:
		throw InternalException("Unrecognized catalog entry type");
	}
	return catalog_map->InsertEntry(entry_name, std::move(result));
}

void SQLiteTransaction::ClearTableEntry(const string &table_name) {
	catalog_map->EraseEntry(table_name);
}

string GetDropSQL(CatalogType type, const string &table_name, bool cascade) {
	string result;
	result = "DROP ";
	switch (type) {
	case CatalogType::TABLE_ENTRY:
		result += "TABLE ";
		break;
	case CatalogType::VIEW_ENTRY:
		result += "VIEW ";
		break;
	case CatalogType::INDEX_ENTRY:
		result += "INDEX ";
		break;
	default:
		throw InternalException("Unsupported type for drop");
	}
	result += KeywordHelper::WriteOptionallyQuoted(table_name);
	return result;
}

void SQLiteTransaction::DropEntry(CatalogType type, const string &table_name, bool cascade) {
	catalog_map->EraseEntry(table_name);
	GetDB().Execute(GetDropSQL(type, table_name, cascade));
}

} // namespace duckdb
