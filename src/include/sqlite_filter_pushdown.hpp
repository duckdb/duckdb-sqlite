//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_filter_pushdown.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/planner/table_filter_state.hpp"

namespace duckdb {
struct SqliteBindData;

//! A filter that the scan evaluates in DuckDB, because SQLite would compare the values differently
struct SQLiteScanFilter {
	//! The index of the filtered column in the scanned columns
	idx_t column_index;
	unique_ptr<TableFilter> filter;
	unique_ptr<TableFilterState> state;
};

class SQLiteFilterPushdown {
public:
	static bool CanPushExpressionDown(ClientContext &context, const LogicalGet &get, Expression &expr);

	//! The WHERE condition of the filters that SQLite can evaluate, empty if there is none. The other filters are
	//! added to scan_filters.
	static string TransformFilters(ClientContext &context, const SqliteBindData &bind_data,
	                               const vector<column_t> &column_ids, optional_ptr<TableFilterSet> filters,
	                               vector<SQLiteScanFilter> &scan_filters);
};

} // namespace duckdb
