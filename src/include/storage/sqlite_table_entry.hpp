//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/sqlite_table_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "dbconnector/attached.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {

class SQLiteTableEntry : public TableCatalogEntry {
public:
	SQLiteTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, bool all_varchar,
	                 vector<bool> utf8_text_affinity);

	bool all_varchar;
	//! Whether each column has TEXT affinity in a UTF-8 database
	vector<bool> utf8_text_affinity;

	//! The metadata that is only used by remote pushdown - it is only read from SQLite (has_pushdown_info) for
	//! databases attached with sqlite_enable_remote_pushdown
	bool has_pushdown_info = false;
	//! Whether each column compares strings with the BINARY collation
	vector<bool> binary_collation;
	//! Whether each column has REAL affinity - other columns that are read as DOUBLE can store integers
	vector<bool> real_affinity;
	//! The columns that rows can be looked up by - the row id, and the leading columns of indexes
	case_insensitive_set_t indexed_columns;

public:
	const ColumnList &GetColumns() const override;

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                           ClientContext &context) override;

	static dbconnector::attached::AttachedTable Lookup(ClientContext &ctx, QualifiedName name);

protected:
	ColumnList columns;
};

} // namespace duckdb
