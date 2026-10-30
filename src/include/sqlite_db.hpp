//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_db.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "sqlite_utils.hpp"
#include "storage/sqlite_options.hpp"

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/parser/column_list.hpp"

namespace duckdb {
class SQLiteStatement;
struct IndexInfo;
class ClientContext;

class SQLiteDB {
public:
	SQLiteDB();
	SQLiteDB(sqlite3 *db);
	~SQLiteDB();
	// disable copy constructors
	SQLiteDB(const SQLiteDB &other) = delete;
	SQLiteDB &operator=(const SQLiteDB &) = delete;
	//! enable move constructors
	SQLiteDB(SQLiteDB &&other) noexcept;
	SQLiteDB &operator=(SQLiteDB &&) noexcept;

	sqlite3 *db = nullptr;

public:
	//! Open a SQLite database (local files only)
	static SQLiteDB Open(const string &path, const SQLiteOpenOptions &options, bool is_shared = false);
	//! Open a SQLite database, routing paths DuckDB's FileSystem owns (remote, WASM) through the VFS
	//! and plain local files through native SQLite
	//! @param context Required for opening through DuckDB's FileSystem VFS
	static SQLiteDB Open(const string &path, const SQLiteOpenOptions &options, ClientContext &context,
	                     bool is_shared = false);
	bool TryPrepare(const string &query, SQLiteStatement &result);
	//! Prepare a query without the legacy double-quoted string literals - a double-quoted identifier that does not
	//! resolve makes the query fail instead of being read as a string. Returns the error message if it fails.
	bool TryPrepareStrict(const string &query, string &error);
	SQLiteStatement Prepare(const string &query);
	int64_t Execute(const string &query);
	vector<string> GetTables();

	vector<string> GetEntries(string entry_type);
	CatalogType GetEntryType(const string &name);
	void GetTableInfo(const string &table_name, ColumnList &columns, vector<unique_ptr<Constraint>> &constraints,
	                  bool all_varchar, vector<bool> &utf8_text_affinity);
	//! The columns that the rows of a table can be looked up by - the row id, and the leading columns of indexes - and
	//! whether a unique index (or primary key) compares a column with a collation other than BINARY
	case_insensitive_set_t GetIndexedColumns(const string &table_name, bool &non_binary_unique_key);
	//! Whether each column of a table compares strings with the BINARY collation
	vector<bool> GetBinaryCollation(const string &table_name, const ColumnList &columns);
	//! Whether each column has REAL affinity (stores no integers)
	vector<bool> GetRealAffinity(const string &table_name, const ColumnList &columns);
	void GetViewInfo(const string &view_name, string &sql);
	void GetIndexInfo(const string &index_name, string &sql, string &table_name);
	idx_t RunPragma(string pragma_name);
	//! Gets the name that addresses the row id of a table: the first of rowid, _rowid_ and oid that is not hidden by
	//! a column of the same name. Returns false if all three are taken by columns
	bool TryGetRowIdName(const string &table_name, string &rowid_name);
	//! Gets the name that addresses the row id of a table, throws if all of rowid, _rowid_ and oid are taken by columns
	string GetRowIdName(const string &table_name);
	//! Gets the min and max row id of a table, returns false if the table does not have a
	//! rowid column or the row ids are not suitable for a parallel scan
	bool GetRowIdInfo(const string &table_name, const string &rowid_name, RowIdInfo &info);
	bool ColumnExists(const string &table_name, const string &column_name);
	vector<IndexInfo> GetIndexInfo(const string &table_name);

	static void DebugSetPrintQueries(bool print);
	static bool DebugPrintQueries();
	//! Throws if the file access settings do not allow opening the given local database file
	static void VerifyCanAccessLocalFile(ClientContext &context, const string &path);

	bool IsOpen();
	void Close();

private:
	static int GetOpenFlags(const SQLiteOpenOptions &options, bool is_shared, bool force_read_only = false);
	static void ApplyBusyTimeout(sqlite3 *db, const SQLiteOpenOptions &options);
	static void HandleOpenError(const string &path, int rc, ClientContext *context = nullptr);
	//! Open read-only through DuckDB's FileSystem (the caching VFS)
	static SQLiteDB OpenWithVFS(const string &path, const SQLiteOpenOptions &options, ClientContext &context,
	                            bool is_shared);
	//! Open a plain local SQLite database file through native SQLite
	static SQLiteDB OpenLocal(const string &path, const SQLiteOpenOptions &options, bool is_shared = false);
};

} // namespace duckdb
