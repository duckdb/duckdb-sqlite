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
#include "duckdb/common/mutex.hpp"
#include "sqlite_db.hpp"
#include "sqlite_utils.hpp"

namespace duckdb {

class SQLiteTableEntry : public TableCatalogEntry {
public:
	SQLiteTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, bool all_varchar,
	                 vector<bool> utf8_text_affinity, SQLiteTableShape shape);

	bool all_varchar;
	//! Whether each column has TEXT affinity in a UTF-8 database
	vector<bool> utf8_text_affinity;
	//! The name that addresses the row id in scans, empty if columns hide it
	string rowid_name;
	//! The column of the INTEGER PRIMARY KEY, which is an alias of the row id - empty if there is none
	string integer_primary_key;

	//! The metadata that is only used by remote pushdown - it is only read from SQLite (has_pushdown_info) for
	//! databases attached with sqlite_enable_remote_pushdown
	bool has_pushdown_info = false;
	//! Whether each column compares strings with the BINARY collation
	vector<bool> binary_collation;
	//! Whether each column has REAL affinity - other columns that are read as DOUBLE can store integers
	vector<bool> real_affinity;
	//! Whether the table has generated columns - which DuckDB does not read, but which SQLite expands a * to
	bool generated_columns = false;

public:
	const ColumnList &GetColumns() const override;

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                           ClientContext &context) override;

	//! The name by which UPDATE and DELETE address the row ids that a scan of this entry read. Throws if the table
	//! changed since the entry was read
	string GetWriteRowIdName(SQLiteDB &db) const;

	static dbconnector::attached::AttachedTable Lookup(ClientContext &ctx, QualifiedName name);

	//! The columns that rows can be looked up by - the row id, and the leading columns of indexes
	const case_insensitive_set_t &GetIndexedColumns(ClientContext &context);
	//! Whether a unique index (or the primary key) compares a column with a collation other than BINARY - SQLite then
	//! finds conflicts that DuckDB does not
	bool HasNonBinaryUniqueKey(ClientContext &context);
	//! The range of the row ids, read once per entry - false if it is not suitable for a parallel scan
	bool GetRowIdInfo(SQLiteDB &db, RowIdInfo &info);

protected:
	ColumnList columns;

private:
	//! Reads the indexes, which only remote pushdown uses, when it first needs them
	void LoadIndexes(ClientContext &context);

private:
	mutex metadata_lock;
	bool indexes_loaded = false;
	case_insensitive_set_t indexed_columns;
	bool non_binary_unique_key = false;
	bool row_id_info_loaded = false;
	bool has_row_id_info = false;
	RowIdInfo row_id_info;
};

} // namespace duckdb
