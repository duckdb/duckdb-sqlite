#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_table_entry.hpp"
#include "storage/sqlite_transaction.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "sqlite_scanner.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {

SQLiteTableEntry::SQLiteTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                   bool all_varchar)
    : TableCatalogEntry(catalog, schema, info), all_varchar(all_varchar) {
}

unique_ptr<BaseStatistics> SQLiteTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

void SQLiteTableEntry::BindUpdateConstraints(Binder &, LogicalGet &, LogicalProjection &, LogicalUpdate &,
                                             ClientContext &) {
}

TableFunction SQLiteTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto result = make_uniq<SqliteBindData>();
	for (auto &col : columns.Logical()) {
		result->names.push_back(col.GetName());
		result->types.push_back(col.GetType());
	}
	auto &sqlite_catalog = catalog.Cast<SQLiteCatalog>();
	result->file_name = sqlite_catalog.path;
	result->table_name = name;
	result->all_varchar = all_varchar;

	auto &transaction = Transaction::Get(context, catalog).Cast<SQLiteTransaction>();
	auto &db = transaction.GetDB();

	if (!db.GetRowIdInfo(name, result->row_id_info)) {
		result->rows_per_group = optional_idx();
	}

	int64_t threads = 1;
	Value threads_val;
	if (context.TryGetCurrentSetting("threads", threads_val)) {
		threads = BigIntValue::Get(threads_val);
	}

	bool disable_multithreaded_scans = false;
	Value disable_multithreaded_scans_val;
	if (context.TryGetCurrentSetting("sqlite_disable_multithreaded_scans", disable_multithreaded_scans_val)) {
		disable_multithreaded_scans = BooleanValue::Get(disable_multithreaded_scans_val);
	}

	bool use_global_db = !transaction.IsReadOnly() || sqlite_catalog.InMemory() || threads <= 1 || disable_multithreaded_scans;

	if (use_global_db) {
		// for in-memory databases or if we have transaction-local changes we can
		// only do a single-threaded scan set up the transaction's connection object
		// as the global db
		result->global_db = &db;
		result->rows_per_group = optional_idx();
	}
	result->table = this;

	bind_data = std::move(result);
	return static_cast<TableFunction>(SqliteScanFunction());
}

TableStorageInfo SQLiteTableEntry::GetStorageInfo(ClientContext &context) {
	auto &transaction = Transaction::Get(context, catalog).Cast<SQLiteTransaction>();
	auto &db = transaction.GetDB();
	TableStorageInfo result;

	RowIdInfo info;
	if (!db.GetRowIdInfo(name, info)) {
		// probably
		result.cardinality = 10000;
	} else {
		result.cardinality = info.max_rowid.GetIndex() - info.min_rowid.GetIndex();
	}

	result.index_info = db.GetIndexInfo(name);
	return result;
}

} // namespace duckdb
