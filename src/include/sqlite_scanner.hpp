//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_scanner.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "sqlite_utils.hpp"
#include "storage/sqlite_catalog.hpp"

namespace duckdb {
class SQLiteDB;
class TableCatalogEntry;

struct SqliteBindData : public TableFunctionData {
	string file_name;
	string table_name;

	vector<string> names;
	vector<LogicalType> types;
	string sql;
	vector<Value> params;

	RowIdInfo row_id_info;
	bool all_varchar = false;

	optional_idx rows_per_group = 122880;
	SQLiteDB *global_db = nullptr;

	optional_ptr<TableCatalogEntry> table;
	bool command_only = false;
	optional_ptr<SQLiteCatalog> catalog = nullptr;
};

class SqliteScanFunction : public TableFunction {
public:
	SqliteScanFunction();
};

class SqliteAttachFunction : public TableFunction {
public:
	SqliteAttachFunction();
};

class SQLiteQueryFunction : public TableFunction {
public:
	SQLiteQueryFunction();
};

} // namespace duckdb
