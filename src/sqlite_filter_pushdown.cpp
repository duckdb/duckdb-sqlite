#include "sqlite_filter_pushdown.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

#include "dbconnector/table_scan/filter_pushdown.hpp"
#include "dbconnector/table_scan/filter_util.hpp"

#include "sqlite_scanner.hpp"
#include "sqlite_utils.hpp"

namespace duckdb {

using dbconnector::table_scan::FilterPushdown;
using dbconnector::table_scan::FilterUtil;

static string WriteDistinctFrom(ExpressionType distinct_type, const string &column_name,
                                const string &constant_string) {
	// IS [NOT] DISTINCT FROM needs SQLite 3.39, IS and IS NOT compare in the same way
	switch (distinct_type) {
	case ExpressionType::COMPARE_DISTINCT_FROM:
		return StringUtil::Format("%s IS NOT %s", column_name, constant_string);
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return StringUtil::Format("%s IS %s", column_name, constant_string);
	default:
		throw InvalidInputException("Unsupported DISTINCT FROM comparison type: %s", EnumUtil::ToString(distinct_type));
	}
}

static FilterPushdown::Config CreateSQLiteConfig() {
	return FilterPushdown::CreateConfig('"', '\'', dbconnector::query::QuoteEscapeStyle::DOUBLE_QUOTE, "X'", "'",
	                                    string(), WriteDistinctFrom);
}

//! SQLite reads a statement up to its first NUL byte, so such a string cannot be written into the query
static bool HasNulByteConstant(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		auto &value = expr.Cast<BoundConstantExpression>().GetValue();
		return value.type().id() == LogicalTypeId::VARCHAR && !value.IsNull() &&
		       StringValue::Get(value).find('\0') != string::npos;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.Function().GetName() == OptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &child = func.BindInfo()->Cast<OptionalFilterFunctionData>().child_filter_expr;
			return child && HasNulByteConstant(*child);
		}
		if (func.Function().GetName() == SelectivityOptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &child = func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>().child_filter_expr;
			return child && HasNulByteConstant(*child);
		}
	}
	bool result = false;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { result = result || HasNulByteConstant(child); });
	return result;
}

//! Adds the conditions of a filter that SQLite can evaluate, returns false if part of the filter must be evaluated in
//! DuckDB
static bool TransformFilter(const FilterPushdown::Config &config, const string &column_name, const Expression &expr,
                            vector<string> &conditions) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
	    expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		bool complete = true;
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			complete = TransformFilter(config, column_name, *child, conditions) && complete;
		}
		return complete;
	}
	string condition;
	if (!HasNulByteConstant(expr)) {
		condition = FilterPushdown::TransformFilterExpression(config, column_name, expr);
	}
	if (!condition.empty()) {
		conditions.push_back(std::move(condition));
		return true;
	}
	// internal filters only prune rows, they can be skipped
	return FilterUtil::IsInternalExpression(expr);
}

//! Whether SQLite compares the values of a column the same way DuckDB does
static bool CanPushColumn(const SqliteBindData &bind_data, column_t column_id) {
	if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
		return !bind_data.rowid_name.empty();
	}
	if (bind_data.all_varchar || column_id >= bind_data.types.size() ||
	    column_id >= bind_data.utf8_text_affinity.size()) {
		return false;
	}
	switch (bind_data.types[column_id].id()) {
	case LogicalTypeId::BIGINT:
		return true;
	case LogicalTypeId::VARCHAR:
		// other affinities compare a numeric string constant as a number, and other encodings order strings by their
		// UTF-16 bytes
		return bind_data.utf8_text_affinity[column_id];
	default:
		// dates and timestamps are stored in several formats, and SQLite has no NaN
		return false;
	}
}

bool SQLiteFilterPushdown::CanPushExpressionDown(ClientContext &, const LogicalGet &, Expression &expr) {
	auto config = CreateSQLiteConfig();
	return !HasNulByteConstant(expr) && !FilterPushdown::TransformFilterExpression(config, "dummy", expr).empty();
}

string SQLiteFilterPushdown::TransformFilters(ClientContext &context, const SqliteBindData &bind_data,
                                              const vector<column_t> &column_ids, optional_ptr<TableFilterSet> filters,
                                              vector<SQLiteScanFilter> &scan_filters) {
	if (!filters || !filters->HasFilters()) {
		return string();
	}
	auto config = CreateSQLiteConfig();
	vector<string> conditions;
	for (auto &entry : *filters) {
		auto column_index = entry.GetIndex();
		auto column_id = column_ids[column_index];
		auto &filter = entry.Filter();
		bool complete = false;
		if (CanPushColumn(bind_data, column_id)) {
			string column_name;
			if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
				column_name = bind_data.rowid_name;
			} else {
				column_name = '"' + SQLiteUtils::SanitizeIdentifier(bind_data.names[column_id]) + '"';
				if (bind_data.types[column_id].id() == LogicalTypeId::VARCHAR) {
					// DuckDB compares strings byte by byte, whatever collation the column has in SQLite
					column_name += " COLLATE BINARY";
				}
			}
			auto &expr = FilterUtil::GetExpression(filter, "SQLiteFilterPushdown::TransformFilters");
			complete = TransformFilter(config, column_name, expr, conditions);
		}
		if (!complete) {
			SQLiteScanFilter scan_filter;
			scan_filter.column_index = column_index;
			scan_filter.filter =
			    ExpressionFilter::GetExpressionFilter(filter, "SQLiteFilterPushdown::TransformFilters").Copy();
			scan_filter.state = TableFilterState::Initialize(context, *scan_filter.filter);
			scan_filters.push_back(std::move(scan_filter));
		}
	}
	return StringUtil::Join(conditions, " AND ");
}

} // namespace duckdb
