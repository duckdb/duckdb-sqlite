#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "sqlite_db.hpp"
#include "sqlite_stmt.hpp"
#include "sqlite_duckdb_vfs_cache.hpp"
#include "sqlite_functions.hpp"
#include "sqlite_utils.hpp"

namespace duckdb {

static bool debug_sqlite_print_queries = false;

SQLiteDB::SQLiteDB() : db(nullptr) {
}

SQLiteDB::SQLiteDB(sqlite3 *db) : db(db) {
}

SQLiteDB::~SQLiteDB() {
	Close();
}

SQLiteDB::SQLiteDB(SQLiteDB &&other) noexcept : db(nullptr) {
	// db must be initialized before the swap: as a constructor, the member is otherwise indeterminate,
	// and swapping garbage into `other` would make other's destructor sqlite3_close() a wild pointer.
	std::swap(db, other.db);
}

SQLiteDB &SQLiteDB::operator=(SQLiteDB &&other) noexcept {
	std::swap(db, other.db);
	return *this;
}

int SQLiteDB::GetOpenFlags(const SQLiteOpenOptions &options, bool is_shared, bool force_read_only) {
	int flags = SQLITE_OPEN_PRIVATECACHE;

	if (force_read_only || options.access_mode == AccessMode::READ_ONLY) {
		// VFS-backed opens (remote/WASM) are always read-only
		flags |= SQLITE_OPEN_READONLY;
	} else {
		flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	}

	if (!is_shared) {
		// FIXME: we should just make sure we are not re-using the same `sqlite3`
		// object across threads
		flags |= SQLITE_OPEN_NOMUTEX;
	}

	flags |= SQLITE_OPEN_EXRESCODE;
	return flags;
}

void SQLiteDB::ApplyBusyTimeout(sqlite3 *db, const SQLiteOpenOptions &options) {
	if (options.busy_timeout > 0) {
		if (options.busy_timeout > NumericLimits<int>::Maximum()) {
			throw std::runtime_error("busy_timeout out of range - must be within "
			                         "valid range for type int");
		}
		auto rc = sqlite3_busy_timeout(db, int(options.busy_timeout));
		if (rc != SQLITE_OK) {
			throw std::runtime_error("Failed to set busy timeout");
		}
	}
}

void SQLiteDB::HandleOpenError(const string &path, int rc, ClientContext *context) {
	string error_msg;
	int primary_error = rc & 0xFF;

	// True when the open went through our VFS: OpenWithVFS passes a context (OpenLocal does not), and
	// it is only reached for paths the VFS handles.
	const bool used_vfs = context && SQLiteDuckDBCacheVFS::CanHandlePath(path);

	switch (primary_error) {
	case SQLITE_CANTOPEN:
		error_msg = "unable to open database file";
		break;
	case SQLITE_PERM:
		error_msg = "access permission denied";
		break;
	case SQLITE_IOERR:
		// Through the VFS, I/O errors are filesystem/network issues, not a local disk fault
		if (used_vfs) {
			error_msg = "unable to open database file";
		} else {
			error_msg = "disk I/O error";
		}
		break;
	case SQLITE_BUSY:
		error_msg = "database is locked";
		break;
	case SQLITE_NOMEM:
		error_msg = "out of memory";
		break;
	case SQLITE_READONLY:
		error_msg = "attempt to write a readonly database";
		break;
	case SQLITE_CORRUPT:
		error_msg = "file is not a database";
		break;
	default:
		error_msg = sqlite3_errstr(rc);
		break;
	}
	// For VFS-backed opens, enrich the terse SQLite-code message with the full filesystem error this
	// attempt recorded (e.g. HTTP status/URL), which SQLite otherwise discards. Cleared at open start,
	// so a non-empty value belongs to this attempt; empty (e.g. the bytes arrived but were not a
	// database) leaves the terse message untouched.
	if (used_vfs) {
		const string detail = SQLiteDuckDBCacheVFS::GetLastErrorForContext(*context);
		if (!detail.empty()) {
			throw ConnectionException("Unable to open database \"%s\": %s (%s)", path, error_msg, detail);
		}
	}
	throw ConnectionException("Unable to open database \"%s\": %s", path, error_msg);
}

SQLiteDB SQLiteDB::OpenLocal(const string &path, const SQLiteOpenOptions &options, bool is_shared) {
	SQLiteDB result;
	int flags = GetOpenFlags(options, is_shared, false);

	auto rc = sqlite3_open_v2(path.c_str(), &result.db, flags, nullptr);
	if (rc != SQLITE_OK) {
		throw std::runtime_error("Unable to open database \"" + path + "\": " + string(sqlite3_errstr(rc)));
	}
	if (options.remote_pushdown) {
		// the functions are only used by the queries that remote pushdown generates
		SQLiteFunctions::Register(result.db);
	}

	ApplyBusyTimeout(result.db, options);

	// Both values were validated against SQLite's keyword lists when the option was parsed, so they
	// can be spliced into the PRAGMA as bare keywords.
	if (!options.journal_mode.empty()) {
		result.Execute("PRAGMA journal_mode=" + options.journal_mode);
	}
	if (!options.synchronous.empty()) {
		// synchronous is a per-connection setting (unlike journal_mode, which is persistent in WAL
		// mode), so it has to be applied to every handle we open
		result.Execute("PRAGMA synchronous=" + options.synchronous);
	}
	return result;
}

// Opens a read-only SQLite database through DuckDB's FileSystem (the caching VFS), for any path
// DuckDB owns rather than a native local file.
SQLiteDB SQLiteDB::OpenWithVFS(const string &path, const SQLiteOpenOptions &options, ClientContext &context,
                               bool is_shared) {
	SQLiteDuckDBCacheVFS::Register(context);
	// Reset any error recorded by a previous open on this context, so a failure below surfaces
	// only the rich httpfs error produced by THIS attempt (see HandleOpenError).
	SQLiteDuckDBCacheVFS::ClearLastErrorForContext(context);

	SQLiteDB result;
	int flags = GetOpenFlags(options, is_shared, true);

	const auto vfs_name = SQLiteDuckDBCacheVFS::GetVFSNameForContext(context);
	auto rc = sqlite3_open_v2(path.c_str(), &result.db, flags, vfs_name.c_str());
	if (rc != SQLITE_OK) {
		HandleOpenError(path, rc, &context);
	}
	if (options.remote_pushdown) {
		// the functions are only used by the queries that remote pushdown generates
		SQLiteFunctions::Register(result.db);
	}

	ApplyBusyTimeout(result.db, options);

	// Keep SQLite-side temp B-trees (sorter/materialization spills) in memory: this read-only VFS
	// rejects the nameless temp-file open its xOpen would receive, and a remote read-only database has
	// no local scratch space to spill to.
	result.Execute("PRAGMA temp_store=MEMORY");

	return result;
}

// Overload for callers without a ClientContext (in-memory / local only). A remote path must go through
// the caching VFS, so assert it is not one here. (":memory:" is not a remote file on any platform, so
// it stays valid, including on WASM.)
SQLiteDB SQLiteDB::Open(const string &path, const SQLiteOpenOptions &options, bool is_shared) {
	D_ASSERT(!FileSystem::IsRemoteFile(path));
	return OpenLocal(path, options, is_shared);
}

// Main entry point for opening SQLite databases.
// Paths DuckDB's FileSystem owns (any remote filesystem, and all paths on WASM) are opened read-only
// through the caching VFS; plain local files use native SQLite (read-write, locking, WAL).
SQLiteDB SQLiteDB::Open(const string &path, const SQLiteOpenOptions &options, ClientContext &context, bool is_shared) {
	if (SQLiteDuckDBCacheVFS::CanHandlePath(path)) {
		return OpenWithVFS(path, options, context, is_shared);
	}
	VerifyCanAccessLocalFile(context, path);
	return OpenLocal(path, options, is_shared);
}

void SQLiteDB::VerifyCanAccessLocalFile(ClientContext &context, const string &path) {
	if (path == ":memory:" || SQLiteDuckDBCacheVFS::CanHandlePath(path)) {
		// in-memory databases do not access files, and the caching VFS goes through DuckDB's FileSystem
		return;
	}
	// native SQLite opens local files itself, so check the file access settings of DuckDB here
	if (!DBConfig::GetConfig(context).CanAccessFile(path, FileType::FILE_TYPE_REGULAR)) {
		throw PermissionException("Cannot access file \"%s\" - file system operations are disabled by configuration",
		                          path);
	}
}

bool SQLiteDB::TryPrepare(const string &query, SQLiteStatement &stmt) {
	stmt.db = db;
	if (debug_sqlite_print_queries) {
		Printer::Print(query + "\n");
	}
	auto rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt.stmt, nullptr);
	if (rc != SQLITE_OK) {
		return false;
	}
	return true;
}

bool SQLiteDB::TryPrepareStrict(const string &query, string &error) {
	int dqs_dml = 0;
	int dqs_ddl = 0;
	sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DML, -1, &dqs_dml);
	sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DDL, -1, &dqs_ddl);
	sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DML, 0, nullptr);
	sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DDL, 0, nullptr);
	SQLiteStatement stmt;
	bool success = TryPrepare(query, stmt);
	if (!success) {
		error = sqlite3_errmsg(db);
	}
	stmt.Close();
	sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DML, dqs_dml, nullptr);
	sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DDL, dqs_ddl, nullptr);
	return success;
}

SQLiteStatement SQLiteDB::Prepare(const string &query) {
	SQLiteStatement stmt;
	if (!TryPrepare(query, stmt)) {
		string error = "Failed to prepare query \"" + query + "\": " + string(sqlite3_errmsg(db));
		throw std::runtime_error(error);
	}
	return stmt;
}

int64_t SQLiteDB::Execute(const string &query) {
	if (debug_sqlite_print_queries) {
		Printer::Print(query + "\n");
	}
	auto rc = sqlite3_exec(db, query.c_str(), nullptr, nullptr, nullptr);
	if (rc != SQLITE_OK) {
		string error = "Failed to execute query \"" + query + "\": " + string(sqlite3_errmsg(db));
		throw std::runtime_error(error);
	}
	return static_cast<int64_t>(sqlite3_changes64(db));
}

bool SQLiteDB::IsOpen() {
	return db;
}

void SQLiteDB::Close() {
	if (!IsOpen()) {
		return;
	}
	auto rc = sqlite3_close_v2(db);
	if (rc == SQLITE_BUSY) {
		throw InternalException("Failed to close database - SQLITE_BUSY");
	}
	db = nullptr;
}

vector<string> SQLiteDB::GetEntries(string entry_type) {
	vector<string> result;
	SQLiteStatement stmt =
	    Prepare("SELECT name FROM sqlite_master WHERE length(sql) > 0 AND type='" + entry_type + "'");
	while (stmt.Step()) {
		auto table_name = stmt.GetValue<string>(0);
		result.push_back(std::move(table_name));
	}
	return result;
}

vector<string> SQLiteDB::GetTables() {
	return GetEntries("table");
}

CatalogType SQLiteDB::GetEntryType(const string &name) {
	SQLiteStatement stmt;
	stmt = Prepare(StringUtil::Format("SELECT type FROM sqlite_master WHERE lower(name)=lower('%s');",
	                                  SQLiteUtils::SanitizeString(name)));
	while (stmt.Step()) {
		auto type = stmt.GetValue<string>(0);
		if (type == "table") {
			return CatalogType::TABLE_ENTRY;
		} else if (type == "view") {
			return CatalogType::VIEW_ENTRY;
		} else if (type == "index") {
			return CatalogType::INDEX_ENTRY;
		} else {
			throw InternalException("Unrecognized SQLite type \"%s\"", name);
		}
	}
	return CatalogType::INVALID;
}

void SQLiteDB::GetIndexInfo(const string &index_name, string &sql, string &table_name) {
	SQLiteStatement stmt;
	stmt = Prepare(StringUtil::Format("SELECT tbl_name, sql FROM sqlite_master WHERE lower(name)=lower('%s');",
	                                  SQLiteUtils::SanitizeString(index_name)));
	while (stmt.Step()) {
		table_name = stmt.GetValue<string>(0);
		sql = stmt.GetValue<string>(1);
		return;
	}
	throw InternalException("GetViewInfo - index \"%s\" not found", index_name);
}

void SQLiteDB::GetViewInfo(const string &view_name, string &sql) {
	SQLiteStatement stmt;
	stmt = Prepare(StringUtil::Format("SELECT sql FROM sqlite_master WHERE lower(name)=lower('%s');",
	                                  SQLiteUtils::SanitizeString(view_name)));
	while (stmt.Step()) {
		sql = stmt.GetValue<string>(0);
		return;
	}
	throw InternalException("GetViewInfo - view \"%s\" not found", view_name);
}

//! SQLite treats an identifier in a DEFAULT clause (e.g. DEFAULT "" or DEFAULT abc) as a string literal
static unique_ptr<ParsedExpression> SQLiteStringDefault(const string &str, const LogicalType &column_type) {
	Value value(str);
	if (!value.DefaultTryCastAs(column_type)) {
		// e.g. DEFAULT "" in an INTEGER column, SQLite stores the string as is - keep the column without a default
		return nullptr;
	}
	return ConstantExpression::FromValue(value);
}

static unique_ptr<ParsedExpression> ParseSQLiteDefault(const string &default_value, const LogicalType &column_type) {
	if (default_value == "\"\"") {
		// the DuckDB parser does not accept zero-length delimited identifiers
		return SQLiteStringDefault(string(), column_type);
	}
	auto expressions = Parser::GetBuiltinParser().ParseExpressionList(default_value);
	if (expressions.empty()) {
		throw InternalException("Expression list is empty");
	}
	auto &expr = expressions[0];
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &column_ref = expr->Cast<ColumnRefExpression>();
		if (!column_ref.IsQualified()) {
			return SQLiteStringDefault(column_ref.GetColumnName().GetIdentifierName(), column_type);
		}
	}
	return std::move(expr);
}

void SQLiteDB::GetTableInfo(const string &table_name, ColumnList &columns, vector<unique_ptr<Constraint>> &constraints,
                            bool all_varchar, vector<bool> &utf8_text_affinity) {
	SQLiteStatement stmt;

	idx_t primary_key_index = idx_t(-1);
	vector<Identifier> primary_keys;

	bool found = false;
	utf8_text_affinity.clear();

	stmt = Prepare("PRAGMA encoding");
	bool utf8 = stmt.Step() && stmt.GetValue<string>(0) == "UTF-8";

	stmt = Prepare(StringUtil::Format("PRAGMA table_info('%s')", SQLiteUtils::SanitizeString(table_name)));
	while (stmt.Step()) {
		auto cid = stmt.GetValue<int>(0);
		auto sqlite_colname = stmt.GetValue<string>(1);
		auto sqlite_type = StringUtil::Lower(stmt.GetValue<string>(2));
		auto not_null = stmt.GetValue<int>(3);
		auto default_value = stmt.GetValue<string>(4);
		auto pk = stmt.GetValue<int>(5);
		StringUtil::Trim(sqlite_type);
		auto column_type = all_varchar ? LogicalType::VARCHAR : SQLiteUtils::TypeToLogicalType(sqlite_type);
		utf8_text_affinity.push_back(utf8 && SQLiteUtils::HasTextAffinity(sqlite_type));

		if (pk) {
			primary_key_index = cid;
			primary_keys.emplace_back(sqlite_colname);
		}
		ColumnDefinition column(Identifier(std::move(sqlite_colname)), std::move(column_type));
		if (!default_value.empty()) {
			auto default_expr = ParseSQLiteDefault(default_value, column.Type());
			if (default_expr) {
				column.SetDefaultValue(std::move(default_expr));
			}
		}
		columns.AddColumn(std::move(column));
		if (not_null) {
			constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(cid)));
		}
		found = true;
	}
	if (!found) {
		throw InternalException("GetTableInfo - table \"%s\" not found", table_name);
	}
	if (!primary_keys.empty()) {
		auto uc = make_uniq<UniqueConstraint>(std::move(primary_keys), true);
		if (primary_keys.size() == 1) {
			uc->SetIndex(LogicalIndex(primary_key_index));
		}
		constraints.emplace_back(std::move(uc));
	}
}

case_insensitive_set_t SQLiteDB::GetIndexedColumns(const string &table_name, bool &non_binary_unique_key) {
	case_insensitive_set_t result;
	non_binary_unique_key = false;
	string rowid_name;
	if (TryGetRowIdName(table_name, rowid_name)) {
		result.insert(rowid_name);
	}
	// an INTEGER PRIMARY KEY is an alias of the row id
	vector<string> pk_columns;
	string pk_type;
	SQLiteStatement stmt;
	stmt = Prepare(StringUtil::Format("PRAGMA table_info(\"%s\")", SQLiteUtils::SanitizeIdentifier(table_name)));
	while (stmt.Step()) {
		if (stmt.GetValue<int>(5) > 0) {
			pk_columns.push_back(stmt.GetValue<string>(1));
			pk_type = stmt.GetValue<string>(2);
		}
	}
	if (pk_columns.size() == 1 && StringUtil::CIEquals(pk_type, "INTEGER")) {
		result.insert(pk_columns[0]);
	}
	vector<pair<string, bool>> indexes;
	stmt = Prepare(StringUtil::Format("PRAGMA index_list(\"%s\")", SQLiteUtils::SanitizeIdentifier(table_name)));
	while (stmt.Step()) {
		indexes.emplace_back(stmt.GetValue<string>(1), stmt.GetValue<int>(2) != 0);
	}
	for (auto &index : indexes) {
		// seqno, cid, name, desc, coll, key
		stmt = Prepare(StringUtil::Format("PRAGMA index_xinfo(\"%s\")", SQLiteUtils::SanitizeIdentifier(index.first)));
		while (stmt.Step()) {
			if (stmt.GetValue<int>(5) == 0) {
				// not a key column of the index (e.g. the row id)
				continue;
			}
			if (stmt.GetValue<int>(0) == 0 && stmt.GetType(2) == SQLITE_TEXT) {
				result.insert(stmt.GetValue<string>(2));
			}
			if (index.second && stmt.GetType(4) == SQLITE_TEXT &&
			    !StringUtil::CIEquals(stmt.GetValue<string>(4), "BINARY")) {
				non_binary_unique_key = true;
			}
		}
	}
	return result;
}

vector<bool> SQLiteDB::GetBinaryCollation(const string &table_name, const ColumnList &columns) {
	vector<bool> result;
	for (auto &column : columns.Logical()) {
		const char *collation = nullptr;
		auto rc =
		    sqlite3_table_column_metadata(db, "main", table_name.c_str(), column.Name().GetIdentifierName().c_str(),
		                                  nullptr, &collation, nullptr, nullptr, nullptr);
		result.push_back(rc == SQLITE_OK && collation && StringUtil::CIEquals(collation, "BINARY"));
	}
	return result;
}

vector<bool> SQLiteDB::GetRealAffinity(const string &table_name, const ColumnList &columns) {
	vector<bool> result;
	for (auto &column : columns.Logical()) {
		const char *declared_type = nullptr;
		auto rc =
		    sqlite3_table_column_metadata(db, "main", table_name.c_str(), column.Name().GetIdentifierName().c_str(),
		                                  &declared_type, nullptr, nullptr, nullptr, nullptr);
		bool real_affinity = false;
		if (rc == SQLITE_OK && declared_type) {
			// https://www.sqlite.org/datatype3.html#determination_of_column_affinity
			auto type = StringUtil::Lower(declared_type);
			real_affinity = !StringUtil::Contains(type, "int") && !StringUtil::Contains(type, "char") &&
			                !StringUtil::Contains(type, "clob") && !StringUtil::Contains(type, "text") &&
			                !StringUtil::Contains(type, "blob") &&
			                (StringUtil::Contains(type, "real") || StringUtil::Contains(type, "floa") ||
			                 StringUtil::Contains(type, "doub"));
		}
		result.push_back(real_affinity);
	}
	return result;
}

bool SQLiteDB::ColumnExists(const string &table_name, const string &column_name) {
	SQLiteStatement stmt;

	stmt = Prepare(StringUtil::Format("PRAGMA table_info(\"%s\")", SQLiteUtils::SanitizeIdentifier(table_name)));
	while (stmt.Step()) {
		auto sqlite_colname = stmt.GetValue<string>(1);
		if (sqlite_colname == column_name) {
			return true;
		}
	}
	return false;
}

bool SQLiteDB::TryGetRowIdName(const string &table_name, string &rowid_name) {
	// table_xinfo also lists generated columns, which hide the row id names as well
	vector<string> column_names;
	SQLiteStatement stmt;
	stmt = Prepare(StringUtil::Format("PRAGMA table_xinfo(\"%s\")", SQLiteUtils::SanitizeIdentifier(table_name)));
	while (stmt.Step()) {
		column_names.push_back(stmt.GetValue<string>(1));
	}
	for (auto candidate : {"rowid", "_rowid_", "oid"}) {
		bool hidden = false;
		for (auto &column_name : column_names) {
			if (StringUtil::CIEquals(column_name, candidate)) {
				hidden = true;
				break;
			}
		}
		if (!hidden) {
			rowid_name = candidate;
			return true;
		}
	}
	return false;
}

string SQLiteDB::GetRowIdName(const string &table_name) {
	string rowid_name;
	if (!TryGetRowIdName(table_name, rowid_name)) {
		throw BinderException("Cannot address the rows of SQLite table \"%s\": columns named rowid, _rowid_ and oid "
		                      "hide its row id",
		                      table_name);
	}
	return rowid_name;
}

bool SQLiteDB::GetRowIdInfo(const string &table_name, const string &rowid_name, RowIdInfo &row_id_info) {
	if (rowid_name.empty()) {
		return false;
	}
	SQLiteStatement stmt;
	auto sanitized_table_name = SQLiteUtils::SanitizeIdentifier(table_name);
	if (!TryPrepare(StringUtil::Format("SELECT (SELECT MIN(%s) FROM \"%s\"), (SELECT MAX(%s) FROM \"%s\")", rowid_name,
	                                   sanitized_table_name, rowid_name, sanitized_table_name),
	                stmt)) {
		return false;
	}
	if (!stmt.Step()) {
		return false;
	}
	int64_t min_val = stmt.GetValue<int64_t>(0);
	int64_t max_val = stmt.GetValue<int64_t>(1);
	if (min_val < 0 || max_val <= min_val) {
		return false;
	}
	static constexpr int64_t MAX_ROWS = 20000000000000;
	if (max_val - min_val >= MAX_ROWS) {
		// too many rows - this cannot be dense enough to be accurate
		return false;
	}
	row_id_info.min_rowid = NumericCast<idx_t>(min_val);
	row_id_info.max_rowid = NumericCast<idx_t>(max_val);
	return true;
}

vector<IndexInfo> SQLiteDB::GetIndexInfo(const string &table_name) {
	vector<IndexInfo> info;
	// fetch the primary key
	SQLiteStatement stmt;
	stmt = Prepare(StringUtil::Format("SELECT cid FROM pragma_table_info('%s') WHERE pk",
	                                  SQLiteUtils::SanitizeString(table_name)));
	IndexInfo pk_index;
	while (stmt.Step()) {
		auto cid = stmt.GetValue<int64_t>(0);
		pk_index.column_set.insert(cid);
	}
	if (!pk_index.column_set.empty()) {
		// we have a pk - add it
		pk_index.is_primary = true;
		pk_index.is_unique = true;
		pk_index.is_foreign = false;
		info.push_back(std::move(pk_index));
	}

	// now query the set of unique constraints for the table
	stmt = Prepare(StringUtil::Format("SELECT name FROM pragma_index_list('%s') "
	                                  "WHERE \"unique\" AND origin='u'",
	                                  SQLiteUtils::SanitizeString(table_name)));
	vector<string> unique_indexes;
	while (stmt.Step()) {
		auto index_name = stmt.GetValue<string>(0);
		unique_indexes.push_back(index_name);
	}
	for (auto &index_name : unique_indexes) {
		stmt = Prepare(
		    StringUtil::Format("SELECT cid FROM pragma_index_info('%s')", SQLiteUtils::SanitizeString(index_name)));
		IndexInfo unique_index;
		while (stmt.Step()) {
			auto cid = stmt.GetValue<int64_t>(0);
			unique_index.column_set.insert(cid);
		}
		if (!unique_index.column_set.empty()) {
			// we have a pk - add it
			unique_index.is_primary = false;
			unique_index.is_unique = true;
			unique_index.is_foreign = false;
			info.push_back(std::move(unique_index));
		}
	}
	return info;
}

idx_t SQLiteDB::RunPragma(string pragma_name) {
	SQLiteStatement stmt;
	stmt = Prepare("PRAGMA " + pragma_name);
	while (stmt.Step()) {
		return idx_t(stmt.GetValue<int64_t>(0));
	}
	throw InternalException("No result returned from pragma " + pragma_name);
}

void SQLiteDB::DebugSetPrintQueries(bool print) {
	debug_sqlite_print_queries = print;
}

bool SQLiteDB::DebugPrintQueries() {
	return debug_sqlite_print_queries;
}

} // namespace duckdb
