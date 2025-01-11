#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parser.hpp"
#include "sqlite_db.hpp"
#include "sqlite_stmt.hpp"

namespace duckdb {

static bool debug_sqlite_print_queries = false;

SQLiteDB::SQLiteDB() : db(nullptr) {
}

SQLiteDB::SQLiteDB(sqlite3 *db) : db(db) {
}

SQLiteDB::~SQLiteDB() {
	Close();
}

SQLiteDB::SQLiteDB(SQLiteDB &&other) noexcept {
	std::swap(db, other.db);
}

SQLiteDB &SQLiteDB::operator=(SQLiteDB &&other) noexcept {
	std::swap(db, other.db);
	return *this;
}

SQLiteDB SQLiteDB::Open(const string &path, const SQLiteOpenOptions &options, bool is_shared) {
	SQLiteDB result;
	int flags = SQLITE_OPEN_PRIVATECACHE;
	if (options.access_mode == AccessMode::READ_ONLY) {
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
	auto rc = sqlite3_open_v2(path.c_str(), &result.db, flags, nullptr);
	if (rc != SQLITE_OK) {
		throw std::runtime_error("Unable to open database \"" + path + "\": " + string(sqlite3_errstr(rc)));
	}
	// default busy time-out of 5 seconds
	if (options.busy_timeout > 0) {
		if (options.busy_timeout > NumericLimits<int>::Maximum()) {
			throw std::runtime_error("busy_timeout out of range - must be within "
			                         "valid range for type int");
		}
		rc = sqlite3_busy_timeout(result.db, int(options.busy_timeout));
		if (rc != SQLITE_OK) {
			throw std::runtime_error("Failed to set busy timeout");
		}
	}
	if (!options.journal_mode.empty()) {
		result.Execute("PRAGMA journal_mode=" + KeywordHelper::EscapeQuotes(options.journal_mode, '\''));
	}
	return result;
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
	SQLiteStatement stmt = Prepare("SELECT name FROM sqlite_master WHERE type='" + entry_type + "'");
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
	if (!TryPrepare(StringUtil::Format("SELECT MIN(ROWID), MAX(ROWID) FROM \"%s\"",
	                                   SQLiteUtils::SanitizeIdentifier(table_name)),
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
