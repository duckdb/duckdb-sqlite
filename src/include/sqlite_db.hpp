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

	sqlite3 *db;

public:
	//! Open a SQLite database (local files only)
	static SQLiteDB Open(const string &path, const SQLiteOpenOptions &options, bool is_shared = false);
	//! Open a SQLite database, routing paths DuckDB's FileSystem owns (remote, WASM) through the VFS
	//! and plain local files through native SQLite
	//! @param context Required for opening through DuckDB's FileSystem VFS
	static SQLiteDB Open(const string &path, const SQLiteOpenOptions &options, ClientContext &context,
	                     bool is_shared = false);
	bool TryPrepare(const string &query, SQLiteStatement &result);
	SQLiteStatement Prepare(const string &query);
	int64_t Execute(const string &query);
	vector<string> GetTables();

	vector<string> GetEntries(string entry_type);
	CatalogType GetEntryType(const string &name);
	void GetTableInfo(const string &table_name, ColumnList &columns, vector<unique_ptr<Constraint>> &constraints,
	                  bool all_varchar);
	void GetViewInfo(const string &view_name, string &sql);
	void GetIndexInfo(const string &index_name, string &sql, string &table_name);
	idx_t RunPragma(string pragma_name);
	//! Gets the max row id of a table, returns false if the table does not have a
	//! rowid column
	bool GetRowIdInfo(const string &table_name, RowIdInfo &info);
	bool ColumnExists(const string &table_name, const string &column_name);
	vector<IndexInfo> GetIndexInfo(const string &table_name);

	static void DebugSetPrintQueries(bool print);

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
