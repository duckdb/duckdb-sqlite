#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_table_entry.hpp"
#include "storage/sqlite_transaction.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "sqlite_scanner.hpp"
#include "sqlite_filter_pushdown.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {

SQLiteTableEntry::SQLiteTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                   bool all_varchar, vector<bool> utf8_text_affinity_p, vector<bool> binary_collation_p)
    : TableCatalogEntry(catalog, schema, info), all_varchar(all_varchar),
      utf8_text_affinity(std::move(utf8_text_affinity_p)), binary_collation(std::move(binary_collation_p)),
      columns(std::move(info.columns)) {
}

const ColumnList &SQLiteTableEntry::GetColumns() const {
	return columns;
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
		result->names.emplace_back(col.GetName().GetIdentifierName());
		result->types.push_back(col.GetType());
	}
	auto &sqlite_catalog = catalog.Cast<SQLiteCatalog>();
	result->file_name = sqlite_catalog.path;
	result->table_name = name.GetIdentifierName();
	result->all_varchar = all_varchar;
	result->utf8_text_affinity = utf8_text_affinity;

	auto &transaction = Transaction::Get(context, catalog).Cast<SQLiteTransaction>();
	auto &db = transaction.GetDB();

	db.TryGetRowIdName(name.GetIdentifierName(), result->rowid_name);
	if (!db.GetRowIdInfo(name.GetIdentifierName(), result->rowid_name, result->row_id_info)) {
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

	bool use_global_db = !transaction.IsReadOnly() || !context.transaction.IsAutoCommit() ||
	                     sqlite_catalog.InMemory() || threads <= 1 || disable_multithreaded_scans;

	if (use_global_db) {
		// only the transaction's connection sees its changes and its snapshot, in-memory databases have no other
		result->catalog_name = sqlite_catalog.GetName();
		result->rows_per_group = optional_idx();
	}
	result->qualified_table_name = QualifiedName(ParentCatalog().GetName(), ParentSchema().name, name);
	result->context_ptr = transaction.context;

	bind_data = std::move(result);
	TableFunction function = SqliteScanFunction();
	Value filter_pushdown;
	if (context.TryGetCurrentSetting("sqlite_enable_filter_pushdown", filter_pushdown)) {
		function.filter_pushdown = BooleanValue::Get(filter_pushdown);
	}
	if (function.filter_pushdown) {
		function.pushdown_expression = SQLiteFilterPushdown::CanPushExpressionDown;
	}
	return function;
}

TableStorageInfo SQLiteTableEntry::GetStorageInfo(ClientContext &context) {
	auto &transaction = Transaction::Get(context, catalog).Cast<SQLiteTransaction>();
	auto &db = transaction.GetDB();
	TableStorageInfo result;

	RowIdInfo info;
	string rowid_name;
	db.TryGetRowIdName(name.GetIdentifierName(), rowid_name);
	if (!db.GetRowIdInfo(name.GetIdentifierName(), rowid_name, info)) {
		// probably
		result.cardinality = 10000;
	} else {
		result.cardinality = info.max_rowid.GetIndex() - info.min_rowid.GetIndex();
	}

	result.index_info = db.GetIndexInfo(name.GetIdentifierName());
	return result;
}

dbconnector::attached::AttachedTable SQLiteTableEntry::Lookup(ClientContext &ctx, QualifiedName name) {
	using namespace dbconnector::attached;

	AttachedTable table = AttachedTable::Lookup(ctx, "sqlite", name);
	if (!table) {
		throw InvalidInputException("Attached SQLite table, name: %s is not found in the specified client session",
		                            name.ToString());
	}
	return table;
}

} // namespace duckdb
