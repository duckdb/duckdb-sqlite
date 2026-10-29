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
	//! Whether each column has TEXT affinity in a UTF-8 database
	vector<bool> utf8_text_affinity;

	optional_idx rows_per_group = 122880;

	bool command_only = false;
	//! Whether the result columns are converted to their types whatever their SQLite storage class (sqlite_query
	//! with column_types)
	bool typed_result = false;
	//! Statements that are executed before the query (sqlite_query)
	vector<string> preliminary_queries;
	//! Whether the cached catalog entries of the transaction are cleared after executing the statement
	bool clear_catalog_cache = false;

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
