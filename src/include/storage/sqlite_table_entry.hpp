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
