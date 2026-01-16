#include "storage/sqlite_delete.hpp"
#include "storage/sqlite_table_entry.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_transaction.hpp"
#include "sqlite_db.hpp"
#include "sqlite_stmt.hpp"

namespace duckdb {

SQLiteDelete::SQLiteDelete(PhysicalPlan &physical_plan, LogicalOperator &op, TableCatalogEntry &table,
                           idx_t row_id_index)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1), table(table),
      row_id_index(row_id_index) {
}

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
class SQLiteDeleteGlobalState : public GlobalSinkState {
public:
	explicit SQLiteDeleteGlobalState(SQLiteTableEntry &table) : table(table), delete_count(0) {
	}

	SQLiteTableEntry &table;
	SQLiteStatement statement;
	idx_t delete_count;
};

string GetDeleteSQL(const string &table_name) {
	string result;
	result = "DELETE FROM " + KeywordHelper::WriteOptionallyQuoted(table_name);
	result += " WHERE rowid = ?";
	return result;
}

unique_ptr<GlobalSinkState> SQLiteDelete::GetGlobalSinkState(ClientContext &context) const {
	auto &sqlite_table = table.Cast<SQLiteTableEntry>();

	auto &transaction = SQLiteTransaction::Get(context, sqlite_table.catalog);
	auto result = make_uniq<SQLiteDeleteGlobalState>(sqlite_table);
	result->statement = transaction.GetDB().Prepare(GetDeleteSQL(sqlite_table.name));
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType SQLiteDelete::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<SQLiteDeleteGlobalState>();

	chunk.Flatten();
	auto &row_identifiers = chunk.data[row_id_index];
	auto row_data = FlatVector::GetData<row_t>(row_identifiers);
	for (idx_t i = 0; i < chunk.size(); i++) {
		gstate.statement.Bind<int64_t>(0, row_data[i]);
		gstate.statement.Step();
		gstate.statement.Reset();
	}
	gstate.delete_count += chunk.size();
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//
SourceResultType SQLiteDelete::GetDataInternal(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const {
	auto &insert_gstate = sink_state->Cast<SQLiteDeleteGlobalState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(insert_gstate.delete_count));

	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
string SQLiteDelete::GetName() const {
	return "DELETE";
}

InsertionOrderPreservingMap<string> SQLiteDelete::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table.name;
	return result;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//
PhysicalOperator &SQLiteCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                            PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING clause not yet supported for deletion of a SQLite table");
	}
	auto &bound_ref = op.expressions[0]->Cast<BoundReferenceExpression>();
	auto &delete_op = planner.Make<SQLiteDelete>(op, op.table, bound_ref.index);
	delete_op.children.push_back(plan);
	return delete_op;
}

} // namespace duckdb
