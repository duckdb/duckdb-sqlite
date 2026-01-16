#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_index.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {

SQLiteCreateIndex::SQLiteCreateIndex(PhysicalPlan &physical_plan, unique_ptr<CreateIndexInfo> info,
                                     TableCatalogEntry &table)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, {LogicalType::BIGINT}, 1), info(std::move(info)),
      table(table) {
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
SourceResultType SQLiteCreateIndex::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSourceInput &input) const {
	auto &catalog = table.catalog;
	auto &schema = catalog.GetSchema(context.client, info->schema);
	auto transaction = schema.GetCatalogTransaction(context.client);
	schema.CreateIndex(transaction, *info, table);

	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Logical Operator
//===--------------------------------------------------------------------===//
class LogicalSQLiteCreateIndex : public LogicalExtensionOperator {
public:
	LogicalSQLiteCreateIndex(unique_ptr<CreateIndexInfo> info_p, TableCatalogEntry &table)
	    : info(std::move(info_p)), table(table) {
	}

	unique_ptr<CreateIndexInfo> info;
	TableCatalogEntry &table;

	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override {
		return planner.Make<SQLiteCreateIndex>(std::move(info), table);
	}

	void Serialize(Serializer &writer) const override {
		throw InternalException("Cannot serialize SQLite Create index");
	}

	void ResolveTypes() override {
		types = {LogicalType::BIGINT};
	}
};

unique_ptr<LogicalOperator> SQLiteCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
                                                           TableCatalogEntry &table, unique_ptr<LogicalOperator> plan) {
	return make_uniq<LogicalSQLiteCreateIndex>(unique_ptr_cast<CreateInfo, CreateIndexInfo>(std::move(stmt.info)),
	                                           table);
}

} // namespace duckdb
