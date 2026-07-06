#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"
#include "sqlite_db.hpp"
#include "sqlite_stmt.hpp"
#include "sqlite_duckdb_vfs_cache.hpp"

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

	ApplyBusyTimeout(result.db, options);

	if (!options.journal_mode.empty()) {
		result.Execute("PRAGMA journal_mode=" + KeywordHelper::EscapeQuotes(options.journal_mode, '\''));
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
	return OpenLocal(path, options, is_shared);
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

SQLiteStatement SQLiteDB::Prepare(const string &query) {
	SQLiteStatement stmt;
	if (!TryPrepare(query, stmt)) {
		string error = "Failed to prepare query \"" + query + "\": " + string(sqlite3_errmsg(db));
		throw std::runtime_error(error);
	}
	return stmt;
}

void SQLiteDB::Execute(const string &query) {
	if (debug_sqlite_print_queries) {
		Printer::Print(query + "\n");
	}
	auto rc = sqlite3_exec(db, query.c_str(), nullptr, nullptr, nullptr);
	if (rc != SQLITE_OK) {
		string error = "Failed to execute query \"" + query + "\": " + string(sqlite3_errmsg(db));
		throw std::runtime_error(error);
	}
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

void SQLiteDB::GetTableInfo(const string &table_name, ColumnList &columns, vector<unique_ptr<Constraint>> &constraints,
                            bool all_varchar) {
	SQLiteStatement stmt;

	idx_t primary_key_index = idx_t(-1);
	vector<string> primary_keys;

	bool found = false;

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

		if (pk) {
			primary_key_index = cid;
			primary_keys.push_back(sqlite_colname);
		}
		ColumnDefinition column(std::move(sqlite_colname), std::move(column_type));
		if (!default_value.empty() && default_value != "\"\"") {
			auto expressions = Parser::ParseExpressionList(default_value);
			if (expressions.empty()) {
				throw InternalException("Expression list is empty");
			}
			column.SetDefaultValue(std::move(expressions[0]));
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
		if (primary_keys.size() == 1) {
			constraints.push_back(make_uniq<UniqueConstraint>(LogicalIndex(primary_key_index), true));
		} else {
			constraints.push_back(make_uniq<UniqueConstraint>(std::move(primary_keys), true));
		}
	}
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

bool SQLiteDB::GetRowIdInfo(const string &table_name, RowIdInfo &row_id_info) {
	SQLiteStatement stmt;
	auto sanitized_table_name = SQLiteUtils::SanitizeIdentifier(table_name);
	if (!TryPrepare(StringUtil::Format("SELECT (SELECT MIN(ROWID) FROM \"%s\"), (SELECT MAX(ROWID) FROM \"%s\")",
	                                   sanitized_table_name, sanitized_table_name),
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

} // namespace duckdb
