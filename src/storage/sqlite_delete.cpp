#include "storage/sqlite_delete.hpp"
#include "storage/sqlite_table_entry.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_transaction.hpp"
#include "sqlite_db.hpp"
#include "sqlite_stmt.hpp"
#include <mutex>

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
	std::mutex rowid_lock;
	vector<row_t> rowids;
	idx_t delete_count;
	bool delete_done = false;
};

string GetDeleteSQL(const string &table_name) {
	string result;
	result = "DELETE FROM " + KeywordHelper::WriteOptionallyQuoted(table_name);
	result += " WHERE rowid = ?";
	return result;
}

unique_ptr<GlobalSinkState> SQLiteDelete::GetGlobalSinkState(ClientContext &context) const {
	auto &sqlite_table = table.Cast<SQLiteTableEntry>();

	auto result = make_uniq<SQLiteDeleteGlobalState>(sqlite_table);
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
	std::lock_guard<std::mutex> lock(gstate.rowid_lock);
	gstate.rowids.insert(gstate.rowids.end(), row_data, row_data + chunk.size());
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//
SourceResultType SQLiteDelete::GetDataInternal(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const {
	auto &insert_gstate = sink_state->Cast<SQLiteDeleteGlobalState>();
	if (!insert_gstate.delete_done) {
		if (!insert_gstate.statement.IsOpen()) {
			auto &transaction = SQLiteTransaction::Get(context.client, insert_gstate.table.catalog);
			insert_gstate.statement = transaction.GetDB().Prepare(GetDeleteSQL(insert_gstate.table.name));
		}
		for (auto row_id : insert_gstate.rowids) {
			insert_gstate.statement.Bind<int64_t>(0, row_id);
			insert_gstate.statement.Step();
			insert_gstate.statement.Reset();
		}
		insert_gstate.delete_count = insert_gstate.rowids.size();
		insert_gstate.delete_done = true;
	}
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(insert_gstate.delete_count));
	FlatVector::SetSize(chunk.data[0], count_t(1));

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
