//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/sqlite_schema_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"

namespace duckdb {
class SQLiteTransaction;
struct CreateTableInfo;
struct CreateViewInfo;

//! The CREATE TABLE statement for a (bound) table definition - the column types are converted to SQLite types
string GetCreateTableSQL(CreateTableInfo &info);
//! The CREATE VIEW statement for a view definition (written in DuckDB's SQL dialect)
string GetCreateViewSQL(CreateViewInfo &info);

class SQLiteSchemaEntry : public SchemaCatalogEntry {
public:
	SQLiteSchemaEntry(Catalog &catalog, CreateSchemaInfo &info);

public:
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

private:
	void AlterTable(SQLiteTransaction &transaction, RenameTableInfo &info);
	void AlterTable(SQLiteTransaction &transaction, RenameColumnInfo &info);
	void AlterTable(SQLiteTransaction &transaction, AddColumnInfo &info);
	void AlterTable(SQLiteTransaction &transaction, RemoveColumnInfo &info);

	void TryDropEntry(ClientContext &context, CatalogType catalog_type, const string &name);
};

} // namespace duckdb
