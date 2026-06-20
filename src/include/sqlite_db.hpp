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

namespace duckdb {
class SQLiteStatement;
struct IndexInfo;

struct SQLiteDBLocation {
	string temp_directory;
	string db_file_path;

	explicit SQLiteDBLocation(std::string db_file_path_p)
			: db_file_path(std::move(db_file_path_p)) {}

	explicit SQLiteDBLocation(std::string temp_directory_p,
														std::string db_file_path_p)
			: temp_directory(std::move(temp_directory_p)),
				db_file_path(std::move(db_file_path_p)) {}
};

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
	static SQLiteDB Open(const string &path, const SQLiteOpenOptions &options, bool is_shared = false);
	bool TryPrepare(const string &query, SQLiteStatement &result);
	SQLiteStatement Prepare(const string &query);
	void Execute(const string &query);
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
};

} // namespace duckdb
