#include "storage/sqlite_transaction_manager.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

SQLiteTransactionManager::SQLiteTransactionManager(AttachedDatabase &db_p, SQLiteCatalog &sqlite_catalog)
    : TransactionManager(db_p), sqlite_catalog(sqlite_catalog) {
}

Transaction &SQLiteTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<SQLiteTransaction>(sqlite_catalog, *this, context);
	transaction->Start();
	auto &result = *transaction;
	lock_guard<mutex> l(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData SQLiteTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	auto &sqlite_transaction = transaction.Cast<SQLiteTransaction>();
	sqlite_transaction.Commit();
	ExtractAndCloseAfterUnlock(transaction);
	return ErrorData();
}

void SQLiteTransactionManager::RollbackTransaction(Transaction &transaction) {
	auto &sqlite_transaction = transaction.Cast<SQLiteTransaction>();
	sqlite_transaction.Rollback();
	ExtractAndCloseAfterUnlock(transaction);
}

void SQLiteTransactionManager::ExtractAndCloseAfterUnlock(Transaction &transaction) {
	// Destroy the SQLite connection with transaction_lock released: ~SQLiteTransaction -> sqlite3_close_v2
	// runs connection teardown that can re-enter paths taking transaction_lock, so holding it across the
	// close risks a lock-order inversion. Extract the transaction under the lock; let the moved-out
	// unique_ptr destruct after the lock is dropped.
	unique_ptr<SQLiteTransaction> to_close;
	{
		lock_guard<mutex> l(transaction_lock);
		auto entry = transactions.find(transaction);
		if (entry == transactions.end()) {
			return;
		}
		to_close = std::move(entry->second);
		transactions.erase(entry);
	}
}

void SQLiteTransactionManager::Checkpoint(ClientContext &context, bool force) {
	auto &transaction = SQLiteTransaction::Get(context, db.GetCatalog());
	auto &db = transaction.GetDB();
	db.Execute("PRAGMA wal_checkpoint");
}

} // namespace duckdb
