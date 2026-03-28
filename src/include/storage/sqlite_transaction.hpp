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
#include "sqlite_db.hpp"
#include <atomic>
#include <mutex>

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
	// Transaction state machine
	enum class TransactionState {
		INIT,       // Initial state, transaction not started
		STARTED,    // Start() called, BEGIN TRANSACTION pending
		EXECUTING   // BEGIN TRANSACTION executed, transaction is active
	};

	SQLiteCatalog &sqlite_catalog;
	SQLiteDB *db;
	SQLiteDB owned_db;
	unique_ptr<SQLiteCatalogMap> catalog_map;

	// Lazy opening support (thread-safe)
	string pending_path;                      // Path to open when GetDB() is called
	std::atomic<bool> db_opened{false};       // Whether the database has been opened
	std::atomic<TransactionState> state{TransactionState::INIT};  // Transaction state
	std::mutex db_mutex;                      // Mutex for double-checked locking in GetDB()
};

} // namespace duckdb
