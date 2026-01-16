#include "storage/sqlite_update.hpp"
#include "storage/sqlite_table_entry.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_transaction.hpp"
#include "sqlite_db.hpp"
#include "sqlite_stmt.hpp"

namespace duckdb {

SQLiteUpdate::SQLiteUpdate(PhysicalPlan &physical_plan, LogicalOperator &op, TableCatalogEntry &table,
                           vector<PhysicalIndex> columns_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1), table(table),
      columns(std::move(columns_p)) {
}

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
class SQLiteUpdateGlobalState : public GlobalSinkState {
public:
	explicit SQLiteUpdateGlobalState(SQLiteTableEntry &table) : table(table), update_count(0) {
	}

	SQLiteTableEntry &table;
	SQLiteStatement statement;
	idx_t update_count;
};

string GetUpdateSQL(SQLiteTableEntry &table, const vector<PhysicalIndex> &index) {
	string result;
	result = "UPDATE " + KeywordHelper::WriteOptionallyQuoted(table.name);
	result += " SET ";
	for (idx_t i = 0; i < index.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		auto &col = table.GetColumn(LogicalIndex(index[i].index));
		result += KeywordHelper::WriteOptionallyQuoted(col.GetName());
		result += " = ?";
	}
	result += " WHERE rowid = ?";
	return result;
}

unique_ptr<GlobalSinkState> SQLiteUpdate::GetGlobalSinkState(ClientContext &context) const {
	auto &sqlite_table = table.Cast<SQLiteTableEntry>();

	auto &transaction = SQLiteTransaction::Get(context, sqlite_table.catalog);
	auto result = make_uniq<SQLiteUpdateGlobalState>(sqlite_table);
	result->statement = transaction.GetDB().Prepare(GetUpdateSQL(sqlite_table, columns));
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType SQLiteUpdate::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<SQLiteUpdateGlobalState>();

	chunk.Flatten();
	auto &row_identifiers = chunk.data[chunk.ColumnCount() - 1];
	auto row_data = FlatVector::GetData<row_t>(row_identifiers);
	auto &stmt = gstate.statement;
	auto update_columns = chunk.ColumnCount() - 1;
	for (idx_t r = 0; r < chunk.size(); r++) {
		// bind the SET values
		for (idx_t c = 0; c < update_columns; c++) {
			auto &col = chunk.data[c];
			stmt.BindValue(col, c, r);
		}
		// bind the row identifier
		stmt.Bind<int64_t>(update_columns, row_data[r]);
		stmt.Step();
		stmt.Reset();
	}
	gstate.update_count += chunk.size();
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//
SourceResultType SQLiteUpdate::GetDataInternal(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const {
	auto &insert_gstate = sink_state->Cast<SQLiteUpdateGlobalState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(insert_gstate.update_count));

	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
string SQLiteUpdate::GetName() const {
	return "UPDATE";
}

InsertionOrderPreservingMap<string> SQLiteUpdate::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table.name;
	return result;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//
PhysicalOperator &SQLiteCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                            PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING clause not yet supported for updates of a SQLite table");
	}
	for (auto &expr : op.expressions) {
		if (expr->type == ExpressionType::VALUE_DEFAULT) {
			throw BinderException("SET DEFAULT is not yet supported for updates of a SQLite table");
		}
	}
	auto &update = planner.Make<SQLiteUpdate>(op, op.table, std::move(op.columns));
	update.children.push_back(plan);
	return update;
}

} // namespace duckdb
