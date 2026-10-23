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
class TableCatalogEntry;

struct SqliteBindData : public TableFunctionData {
	string file_name;
	string table_name;

	vector<string> names;
	vector<LogicalType> types;
	string sql;
	vector<Value> params;

	//! The name that addresses the row id of the table, empty if all row id names are taken by columns
	string rowid_name;
	RowIdInfo row_id_info;
	bool all_varchar = false;

	optional_idx rows_per_group = 122880;

	bool command_only = false;

	QualifiedName qualified_table_name;
	Identifier catalog_name;

	// required for get_bind_info and only used there
	weak_ptr<ClientContext> context_ptr;
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
