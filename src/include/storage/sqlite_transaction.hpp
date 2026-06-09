//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/sqlite_transaction.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/transaction/transaction.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/atomic.hpp"
#include "sqlite_db.hpp"

namespace duckdb {
class SQLiteCatalog;
class SQLiteTableEntry;
class SQLiteCatalogMap;

class SQLiteTransaction : public Transaction {
public:
	SQLiteTransaction(SQLiteCatalog &sqlite_catalog, TransactionManager &manager, ClientContext &context);
	~SQLiteTransaction() override;

	void Start();
	void Commit();
	void Rollback();

	SQLiteDB &GetDB();
	optional_ptr<CatalogEntry> GetCatalogEntry(const string &table_name);
	void DropEntry(CatalogType type, const string &table_name, bool cascade);
	void ClearTableEntry(const string &table_name);

	static SQLiteTransaction &Get(ClientContext &context, Catalog &catalog);

private:
	SQLiteCatalog &sqlite_catalog;
	SQLiteDB *db;
	SQLiteDB owned_db;
	unique_ptr<SQLiteCatalogMap> catalog_map;

	// Remote (HTTP/S3) databases defer their connection open + BEGIN to first use (GetDB()) so no
	// network I/O runs while the MetaTransaction lock is held in StartTransaction(). Local and
	// in-memory databases open eagerly in the constructor. `initialized` is atomic for the
	// lock-free fast path in GetDB(); `started` is written in Start() (before any scan thread) or under
	// init_lock in GetDB(), and read in Start(), Commit, and Rollback (the latter two after the scans
	// join), so it needs no atomic.
	mutex init_lock;
	atomic<bool> initialized {false};
	bool started = false;
};

} // namespace duckdb
