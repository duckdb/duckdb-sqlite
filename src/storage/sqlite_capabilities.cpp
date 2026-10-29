#include "storage/sqlite_capabilities.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/default/default_types.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "sqlite_functions.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
bool SQLiteCapabilities::TryGetConstantValue(const ParsedExpression &expr, Value &result) {
	if (expr.GetExpressionClass() == ExpressionClass::CONSTANT) {
		result = expr.Cast<ConstantExpression>().GetLiteral().ToValue();
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::CAST) {
		// folded constants can also be represented as a cast of a literal
		auto &cast_expr = expr.Cast<CastExpression>();
		if (cast_expr.IsTryCast() || cast_expr.Child().GetExpressionClass() != ExpressionClass::CONSTANT) {
			return false;
		}
		auto target_type = ResolveType(cast_expr.TargetType());
		if (target_type.id() == LogicalTypeId::INVALID) {
			return false;
		}
		auto value = cast_expr.Child().Cast<ConstantExpression>().GetLiteral().ToValue();
		auto cast_result = value.DefaultTryCastAs(target_type);
		if (!cast_result) {
			return false;
		}
		result = std::move(*cast_result);
		return true;
	}
	return false;
}

bool SQLiteCapabilities::TryGetIntegerLiteral(const ParsedExpression &expr, int64_t &result) {
	Value val;
	if (!TryGetConstantValue(expr, val)) {
		return false;
	}
	if (!val.type().IsIntegral() || val.IsNull()) {
		return false;
	}
	auto bigint_value = val.DefaultTryCastAs(LogicalType::BIGINT);
	if (!bigint_value || bigint_value->IsNull()) {
		return false;
	}
	result = BigIntValue::Get(*bigint_value);
	return true;
}

bool SQLiteCapabilities::IsIntegerLiteral(const ParsedExpression &expr) {
	int64_t value;
	return expr.GetExpressionClass() == ExpressionClass::CONSTANT && TryGetIntegerLiteral(expr, value);
}

static bool SQLiteIsIntegerLiteralAtLeast(const ParsedExpression &expr, int64_t minimum_value) {
	int64_t value;
	return SQLiteCapabilities::TryGetIntegerLiteral(expr, value) && value >= minimum_value;
}

static bool SQLiteIsStringLiteral(const ParsedExpression &expr) {
	Value val;
	if (!SQLiteCapabilities::TryGetConstantValue(expr, val)) {
		return false;
	}
	return val.type().id() == LogicalTypeId::VARCHAR && !val.IsNull();
}

bool SQLiteCapabilities::SupportsValue(const Value &value) {
	if (value.IsNull()) {
		return true;
	}
	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		return true;
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
		// SQLite integers are 64-bit
		return !!value.DefaultTryCastAs(LogicalType::BIGINT);
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		// SQLite has no NaN
		return !std::isnan(value.GetValue<double>());
	case LogicalTypeId::DECIMAL:
		// decimals are written as (inexact) REAL literals, they are only supported where DuckDB converts them
		// to DOUBLE - which is verified by the writer
		return true;
	case LogicalTypeId::VARCHAR:
		// SQLite reads statements up to the first NUL byte
		return StringValue::Get(value).find('\0') == string::npos;
	case LogicalTypeId::BLOB:
		return true;
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP: {
		// dates and timestamps are compared as text, which requires four-digit years
		if (value.type().id() == LogicalTypeId::DATE) {
			auto date = DateValue::Get(value);
			if (!Value::IsFinite(date)) {
				return false;
			}
			auto year = Date::ExtractYear(date);
			return year >= 1 && year <= 9999;
		}
		auto ts = TimestampValue::Get(value);
		if (!Value::IsFinite(ts)) {
			return false;
		}
		auto year = Date::ExtractYear(Timestamp::GetDate(ts));
		return year >= 1 && year <= 9999;
	}
	case LogicalTypeId::INTERVAL:
		// intervals are only supported as operands of date / time arithmetic - which is verified by the writer
		return true;
	default:
		// times, nested types, enums, ... have no representation in SQLite
		return false;
	}
}

LogicalType SQLiteCapabilities::ResolveType(const TypeExpression &type_expr) {
	if (!type_expr.GetCatalog().empty() || !type_expr.GetSchema().empty()) {
		// a qualified (user-defined) type
		return LogicalType::INVALID;
	}
	auto type_id = DefaultTypeGenerator::GetDefaultType(type_expr.GetTypeName());
	auto &children = type_expr.GetChildren();
	switch (type_id) {
	case LogicalTypeId::INVALID:
		return LogicalType::INVALID;
	case LogicalTypeId::DECIMAL: {
		int64_t width = 18;
		int64_t scale = 3;
		if (children.size() > 2) {
			return LogicalType::INVALID;
		}
		if (!children.empty() && !TryGetIntegerLiteral(*children[0], width)) {
			return LogicalType::INVALID;
		}
		if (children.size() > 1 && !TryGetIntegerLiteral(*children[1], scale)) {
			return LogicalType::INVALID;
		}
		if (children.size() == 1) {
			scale = 0;
		}
		if (width < 1 || width > Decimal::MAX_WIDTH_DECIMAL || scale < 0 || scale > width) {
			return LogicalType::INVALID;
		}
		return LogicalType::DECIMAL(NumericCast<uint8_t>(width), NumericCast<uint8_t>(scale));
	}
	case LogicalTypeId::VARCHAR:
		// VARCHAR(n) does not truncate in DuckDB - the length modifier is ignored
		for (auto &child : children) {
			if (!IsIntegerLiteral(*child)) {
				return LogicalType::INVALID;
			}
		}
		return LogicalType::VARCHAR;
	default:
		if (!children.empty()) {
			return LogicalType::INVALID;
		}
		return LogicalType(type_id);
	}
}

//! Whether a function expression is a call to the built-in function with the given name, qualified with the
//! system catalog (as produced by ConstantExpression::FromValue)
static bool SQLiteIsSystemFunction(const ParsedExpression &expr, const char *name, idx_t argument_count) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<FunctionExpression>();
	auto &qualified_name = func.GetQualifiedName();
	if (qualified_name.Catalog() != Identifier::SystemCatalog() || qualified_name.Schema() != DEFAULT_SCHEMA ||
	    qualified_name.Name() != name) {
		return false;
	}
	return func.GetArguments().size() == argument_count && !func.Distinct() && !func.Filter() &&
	       !func.GetArguments()[0].HasName();
}

static bool SQLiteTryGetIntervalPart(const ParsedExpression &expr, const char *name, int64_t &result) {
	if (!SQLiteIsSystemFunction(expr, name, 1)) {
		return false;
	}
	return SQLiteCapabilities::TryGetIntegerLiteral(expr.Cast<FunctionExpression>().GetArguments()[0].GetExpression(),
	                                                result);
}

bool SQLiteCapabilities::TryGetIntervalLiteral(const ParsedExpression &expr, interval_t &result) {
	if (!SQLiteIsSystemFunction(expr, "add", 2)) {
		return false;
	}
	auto &arguments = expr.Cast<FunctionExpression>().GetArguments();
	auto &months_and_days = arguments[0].GetExpression();
	if (!SQLiteIsSystemFunction(months_and_days, "add", 2)) {
		return false;
	}
	auto &parts = months_and_days.Cast<FunctionExpression>().GetArguments();
	int64_t months, days, micros;
	if (!SQLiteTryGetIntervalPart(parts[0].GetExpression(), "to_months", months) ||
	    !SQLiteTryGetIntervalPart(parts[1].GetExpression(), "to_days", days) ||
	    !SQLiteTryGetIntervalPart(arguments[1].GetExpression(), "to_microseconds", micros)) {
		return false;
	}
	result.months = NumericCast<int32_t>(months);
	result.days = NumericCast<int32_t>(days);
	result.micros = micros;
	return true;
}

//===--------------------------------------------------------------------===//
// Functions
//===--------------------------------------------------------------------===//
//! Aggregate functions that can be pushed into SQLite - also as window functions
static const case_insensitive_set_t &SQLiteAggregateFunctions() {
	//   "count_star"      - serialized as count(*)
	//   "mean"            - serialized as avg
	//   "string_agg"/"group_concat"/"listagg" - serialized as group_concat, with the default separator ','
	//   "bool_and"/"bool_or" - serialized as min / max of the (0 / 1) booleans
	// Excluded:
	//   "first"/"last"/"any_value" - depend on the (undefined) order of rows
	//   "list"/"array_agg" - SQLite has no lists
	static const case_insensitive_set_t AGGREGATES = {"count",        "count_star", "sum",      "avg",
	                                                  "mean",         "min",        "max",      "string_agg",
	                                                  "group_concat", "listagg",    "bool_and", "bool_or"};
	return AGGREGATES;
}

//! Aggregate functions that are rewritten into an expression or are evaluated with the duckdb_* aggregates of the
//! extension, and can therefore not be used as window functions
static const case_insensitive_set_t &SQLiteRewrittenAggregateFunctions() {
	//   "count_if"        - serialized as count(CASE WHEN x THEN 1 END)
	//   "median"/"quantile*" - serialized as duckdb_quantile_cont / duckdb_quantile_disc
	static const case_insensitive_set_t AGGREGATES = {
	    "count_if", "countif", "stddev", "stddev_samp", "stddev_pop",    "variance",     "var_samp", "var_pop",
	    "product",  "bit_and", "bit_or", "bit_xor",     "arg_min",       "arg_max",      "min_by",   "max_by",
	    "argmin",   "argmax",  "median", "quantile",    "quantile_cont", "quantile_disc"};
	return AGGREGATES;
}

bool SQLiteCapabilities::IsAggregateFunction(const string &name) {
	return SQLiteAggregateFunctions().count(name) > 0 || SQLiteRewrittenAggregateFunctions().count(name) > 0;
}

bool SQLiteCapabilities::SupportsFunction(const FunctionExpression &func) {
	auto &function_name = func.FunctionName().GetIdentifierName();
	// Scalar functions that can be pushed into SQLite - either because name and semantics are identical, or because
	// SQLiteSQLWriter rewrites them during serialization (possibly using the duckdb_* functions of the extension):
	//   "/"            - DuckDB's division always yields a DOUBLE (and inf when dividing by zero)
	//   "//" / "%"     - integer division / modulo, DuckDB raises an error when dividing by zero
	//   "~~"/"!~~"     - [NOT] LIKE, SQLite's LIKE is case insensitive and serialized as GLOB (for constant
	//                    patterns) or duckdb_like
	//   "lower"/"upper" - SQLite only maps ASCII characters, serialized as duckdb_lower / duckdb_upper
	//   "concat"/"concat_ws" - serialized with ||, DuckDB skips NULL arguments
	static const case_insensitive_set_t SCALAR_FUNCTIONS = {
	    // operators
	    "+",
	    "-",
	    "*",
	    "/",
	    "//",
	    "%",
	    "||",
	    "~~",
	    "!~~",
	    "~~*",
	    "!~~*",
	    "~~~",
	    "^@",
	    "&",
	    "|",
	    "<<",
	    ">>",
	    "~",
	    "xor",
	    "like_escape",
	    "not_like_escape",
	    "ilike_escape",
	    "not_ilike_escape",
	    "glob",
	    // string
	    "concat",
	    "concat_ws",
	    "length",
	    "len",
	    "char_length",
	    "character_length",
	    "strlen",
	    "octet_length",
	    "lower",
	    "lcase",
	    "upper",
	    "ucase",
	    "trim",
	    "ltrim",
	    "rtrim",
	    "replace",
	    "substring",
	    "substr",
	    "instr",
	    "strpos",
	    "position",
	    "starts_with",
	    "prefix",
	    "ends_with",
	    "suffix",
	    "contains",
	    "left",
	    "right",
	    "repeat",
	    "lpad",
	    "rpad",
	    "reverse",
	    "unicode",
	    "ascii",
	    "chr",
	    "regexp_matches",
	    "regexp_full_match",
	    "regexp_replace",
	    "md5",
	    "sha256",
	    "typeof",
	    "encode",
	    "regexp_extract",
	    "hex",
	    "to_hex",
	    "bit_length",
	    "to_base64",
	    "base64",
	    "from_base64",
	    // math
	    "abs",
	    "sign",
	    "sqrt",
	    "cbrt",
	    "ln",
	    "log",
	    "log2",
	    "log10",
	    "exp",
	    "sin",
	    "cos",
	    "tan",
	    "cot",
	    "asin",
	    "acos",
	    "atan",
	    "atan2",
	    "sinh",
	    "cosh",
	    "tanh",
	    "asinh",
	    "acosh",
	    "atanh",
	    "degrees",
	    "radians",
	    "ceil",
	    "ceiling",
	    "floor",
	    "trunc",
	    "even",
	    "pow",
	    "power",
	    "^",
	    "**",
	    "round",
	    "round_even",
	    "roundbankers",
	    "isnan",
	    "isinf",
	    "isfinite",
	    // date / time
	    "year",
	    "month",
	    "day",
	    "dayofmonth",
	    "hour",
	    "minute",
	    "second",
	    "millisecond",
	    "microsecond",
	    "quarter",
	    "dayofweek",
	    "isodow",
	    "dayofyear",
	    "week",
	    "weekofyear",
	    "isoyear",
	    "yearweek",
	    "decade",
	    "century",
	    "millennium",
	    "era",
	    "epoch",
	    "epoch_ms",
	    "epoch_us",
	    "epoch_ns",
	    "julian",
	    "dayname",
	    "monthname",
	    "date_part",
	    "datepart",
	    "date_trunc",
	    "datetrunc",
	    "date_diff",
	    "datediff",
	    "date_add",
	    "last_day",
	    "make_date",
	    "strftime",
	    // conditional
	    "coalesce",
	    "nullif",
	    "ifnull",
	    "greatest",
	    "least",
	};
	return SCALAR_FUNCTIONS.count(function_name) > 0 || IsAggregateFunction(function_name);
}

//===--------------------------------------------------------------------===//
// Select alias usage
//===--------------------------------------------------------------------===//
static void SQLiteCollectColumnRefs(const QueryNode &node, case_insensitive_set_t &names);

//! Collect the names of all unqualified column references in an expression tree, descending into subqueries
static void SQLiteCollectColumnRefs(const ParsedExpression &expr, case_insensitive_set_t &names) {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		if (!colref.IsQualified()) {
			names.insert(colref.GetColumnName().GetIdentifierName());
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery = expr.Cast<SubqueryExpression>();
		if (subquery.Subquery() && subquery.Subquery()->node) {
			SQLiteCollectColumnRefs(*subquery.Subquery()->node, names);
		}
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { SQLiteCollectColumnRefs(child, names); });
}

static void SQLiteCollectColumnRefs(const TableRef &ref, case_insensitive_set_t &names) {
	switch (ref.type) {
	case TableReferenceType::JOIN: {
		auto &join = ref.Cast<JoinRef>();
		SQLiteCollectColumnRefs(*join.left, names);
		SQLiteCollectColumnRefs(*join.right, names);
		if (join.condition) {
			SQLiteCollectColumnRefs(*join.condition, names);
		}
		break;
	}
	case TableReferenceType::SUBQUERY: {
		auto &subquery = ref.Cast<SubqueryRef>();
		if (subquery.subquery && subquery.subquery->node) {
			SQLiteCollectColumnRefs(*subquery.subquery->node, names);
		}
		break;
	}
	default:
		break;
	}
}

static void SQLiteCollectColumnRefs(const QueryNode &node, case_insensitive_set_t &names) {
	for (auto &cte_pair : node.cte_map.map) {
		if (cte_pair.second->query_node) {
			SQLiteCollectColumnRefs(*cte_pair.second->query_node, names);
		}
	}
	for (auto &modifier : node.modifiers) {
		if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
			for (auto &order : modifier->Cast<OrderModifier>().orders) {
				SQLiteCollectColumnRefs(*order.expression, names);
			}
		}
	}
	switch (node.type) {
	case QueryNodeType::SELECT_NODE: {
		auto &select = node.Cast<SelectNode>();
		for (auto &expr : select.select_list) {
			SQLiteCollectColumnRefs(*expr, names);
		}
		if (select.from_table) {
			SQLiteCollectColumnRefs(*select.from_table, names);
		}
		if (select.where_clause) {
			SQLiteCollectColumnRefs(*select.where_clause, names);
		}
		for (auto &group : select.groups.group_expressions) {
			SQLiteCollectColumnRefs(*group, names);
		}
		if (select.having) {
			SQLiteCollectColumnRefs(*select.having, names);
		}
		if (select.qualify) {
			SQLiteCollectColumnRefs(*select.qualify, names);
		}
		break;
	}
	case QueryNodeType::SET_OPERATION_NODE: {
		auto &setop = node.Cast<SetOperationNode>();
		for (auto &child : setop.children) {
			SQLiteCollectColumnRefs(*child, names);
		}
		break;
	}
	case QueryNodeType::RECURSIVE_CTE_NODE: {
		auto &cte = node.Cast<RecursiveCTENode>();
		SQLiteCollectColumnRefs(*cte.left, names);
		SQLiteCollectColumnRefs(*cte.right, names);
		break;
	}
	default:
		break;
	}
}

//! Collect the names of the unqualified column references within the subqueries of an expression tree
static void SQLiteCollectSubqueryColumnRefs(const ParsedExpression &expr, case_insensitive_set_t &names) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		SQLiteCollectColumnRefs(expr, names);
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { SQLiteCollectSubqueryColumnRefs(child, names); });
}

//! DuckDB resolves select-list aliases within other select-list expressions, the WHERE / GROUP BY / HAVING / QUALIFY
//! clauses and ORDER BY expressions, when no column of the FROM clause has the name. The writer replaces the
//! references in the select list and the clauses with the aliased expressions (it knows the columns of the tables).
//! References in ORDER BY expressions and within subqueries cannot be resolved that way - they are only a problem
//! when no column with that name exists, which cannot be known pre-bind. We approximate: a name is assumed to be a
//! real column if it is also referenced at a position where alias resolution is impossible (in a select-list item at
//! or before the item that defines the alias). Otherwise the query is conservatively not pushed down.
bool SQLiteCapabilities::SupportsSelectAliasUsage(const SelectNode &select) {
	case_insensitive_map_t<idx_t> alias_definitions;
	for (idx_t i = 0; i < select.select_list.size(); i++) {
		auto &alias = select.select_list[i]->GetAlias().GetIdentifierName();
		if (!alias.empty() && alias_definitions.find(alias) == alias_definitions.end()) {
			alias_definitions[alias] = i;
		}
	}
	if (alias_definitions.empty()) {
		return true;
	}
	vector<case_insensitive_set_t> item_refs(select.select_list.size());
	for (idx_t i = 0; i < select.select_list.size(); i++) {
		SQLiteCollectColumnRefs(*select.select_list[i], item_refs[i]);
	}
	// a reference at or before the defining item cannot be an alias reference - it must be a column
	case_insensitive_set_t column_evidence;
	for (idx_t i = 0; i < select.select_list.size(); i++) {
		for (auto &name : item_refs[i]) {
			auto entry = alias_definitions.find(name);
			if (entry == alias_definitions.end() || entry->second >= i) {
				column_evidence.insert(name);
			}
		}
	}
	// collect the names that DuckDB might resolve as an alias where SQLite cannot
	case_insensitive_set_t suspects;
	for (auto &modifier : select.modifiers) {
		if (modifier->type != ResultModifierType::ORDER_MODIFIER) {
			continue;
		}
		for (auto &order : modifier->Cast<OrderModifier>().orders) {
			if (order.expression->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
				SQLiteCollectColumnRefs(*order.expression, suspects);
			}
		}
	}
	for (auto &expr : select.select_list) {
		SQLiteCollectSubqueryColumnRefs(*expr, suspects);
	}
	for (auto &group : select.groups.group_expressions) {
		SQLiteCollectSubqueryColumnRefs(*group, suspects);
	}
	for (auto clause : {select.where_clause.get(), select.having.get(), select.qualify.get()}) {
		if (clause) {
			SQLiteCollectSubqueryColumnRefs(*clause, suspects);
		}
	}
	for (auto &name : suspects) {
		if (alias_definitions.find(name) != alias_definitions.end() && column_evidence.count(name) == 0) {
			return false;
		}
	}
	return true;
}

//! Whether a join tree contains a NATURAL or USING join - these coalesce the join columns when expanding an
//! unqualified *, which SQLite does not do for the columns of outer joins
static bool SQLiteJoinTreeHasCoalescingJoin(const TableRef &ref) {
	if (ref.type != TableReferenceType::JOIN) {
		return false;
	}
	auto &join = ref.Cast<JoinRef>();
	if (join.ref_type == JoinRefType::NATURAL || !join.using_columns.empty()) {
		return true;
	}
	return SQLiteJoinTreeHasCoalescingJoin(*join.left) || SQLiteJoinTreeHasCoalescingJoin(*join.right);
}

//===--------------------------------------------------------------------===//
// Result modifiers
//===--------------------------------------------------------------------===//
static bool SQLiteIsPlainStar(const ParsedExpression &expr) {
	auto &star = expr.Cast<StarExpression>();
	return star.RelationName().empty() && star.ExcludeList().empty() && star.ReplaceList().empty() &&
	       star.RenameList().empty() && !star.Expression();
}

//! Whether an expression tree contains a COLUMNS(*) expression
static bool SQLiteContainsColumnsStar(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::STAR && expr.Cast<StarExpression>().IsColumns()) {
		return true;
	}
	bool result = false;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (SQLiteContainsColumnsStar(child)) {
			result = true;
		}
	});
	return result;
}

bool SQLiteCapabilities::SupportsOrderEntries(const vector<OrderByNode> &orders,
                                              optional_ptr<const vector<unique_ptr<ParsedExpression>>> select_list) {
	for (auto &order : orders) {
		auto &expr = *order.expression;
		if (expr.GetExpressionClass() == ExpressionClass::STAR) {
			// ORDER BY ALL is serialized as an ORDER BY of all select-list positions - which requires the
			// select list to not contain any stars itself
			if (!select_list || orders.size() != 1 || !SQLiteIsPlainStar(expr)) {
				return false;
			}
			for (auto &select_expr : *select_list) {
				if (select_expr->GetExpressionClass() == ExpressionClass::STAR) {
					return false;
				}
			}
		}
		if (expr.GetExpressionClass() == ExpressionClass::CONSTANT && !IsIntegerLiteral(expr)) {
			// ORDER BY with a non-integer literal is a binder error in DuckDB (unless
			// order_by_non_integer_literal is set)
			return false;
		}
	}
	return true;
}

bool SQLiteCapabilities::SupportsResultModifiers(const QueryNode &node,
                                                 optional_ptr<const vector<unique_ptr<ParsedExpression>>> select_list) {
	for (auto &modifier : node.modifiers) {
		switch (modifier->type) {
		case ResultModifierType::ORDER_MODIFIER: {
			auto &order_mod = modifier->Cast<OrderModifier>();
			if (!SupportsOrderEntries(order_mod.orders, select_list)) {
				return false;
			}
			break;
		}
		case ResultModifierType::LIMIT_MODIFIER: {
			auto &limit_mod = modifier->Cast<LimitModifier>();
			// negative values are a binder error in DuckDB - keep them local so the user gets DuckDB's error
			if (limit_mod.limit_type != LimitValueType::ROW_COUNT) {
				// SQLite has no LIMIT n PERCENT
				return false;
			}
			if (limit_mod.limit && !SQLiteIsIntegerLiteralAtLeast(*limit_mod.limit, 0)) {
				return false;
			}
			if (limit_mod.offset && !SQLiteIsIntegerLiteralAtLeast(*limit_mod.offset, 0)) {
				return false;
			}
			break;
		}
		case ResultModifierType::DISTINCT_MODIFIER: {
			if (!select_list) {
				// DISTINCT is only serialized as part of a SELECT node
				return false;
			}
			break;
		}
		default:
			return false;
		}
	}
	return true;
}

bool SQLiteCapabilities::SupportsCTEMap(const CommonTableExpressionMap &cte_map) {
	for (auto &cte_pair : cte_map.map) {
		auto &cte_info = *cte_pair.second;
		if (!cte_info.key_targets.empty()) {
			// USING KEY is DuckDB-specific
			return false;
		}
	}
	return true;
}

bool SQLiteCapabilities::SupportsWindow(const WindowExpression &window) {
	if (window.IgnoreNulls() || window.Distinct() || !window.ArgOrders().empty()) {
		// IGNORE NULLS, DISTINCT and ORDER BY within the argument list are not supported by SQLite
		return false;
	}
	if (window.GetQualifiedName().Path().size() > 1) {
		return false;
	}
	for (auto boundary : {window.WindowStart(), window.WindowEnd()}) {
		switch (boundary) {
		case WindowBoundary::UNBOUNDED_PRECEDING:
		case WindowBoundary::UNBOUNDED_FOLLOWING:
		case WindowBoundary::CURRENT_ROW_RANGE:
		case WindowBoundary::CURRENT_ROW_ROWS:
		case WindowBoundary::CURRENT_ROW_GROUPS:
		case WindowBoundary::EXPR_PRECEDING_ROWS:
		case WindowBoundary::EXPR_FOLLOWING_ROWS:
		case WindowBoundary::EXPR_PRECEDING_GROUPS:
		case WindowBoundary::EXPR_FOLLOWING_GROUPS:
		case WindowBoundary::EXPR_PRECEDING_RANGE:
		case WindowBoundary::EXPR_FOLLOWING_RANGE:
			break;
		default:
			return false;
		}
	}
	// frame offsets must be non-negative constants in SQLite
	for (auto offset : {window.StartExpr().get(), window.EndExpr().get()}) {
		if (!offset) {
			continue;
		}
		Value value;
		if (!TryGetConstantValue(*offset, value) || value.IsNull() || !value.type().IsNumeric()) {
			return false;
		}
		auto double_value = value.DefaultTryCastAs(LogicalType::DOUBLE);
		if (!double_value || DoubleValue::Get(*double_value) < 0) {
			return false;
		}
	}
	if (!SupportsOrderEntries(window.OrderBy())) {
		return false;
	}
	auto &children = window.GetArguments();
	for (auto &child : children) {
		if (child.HasName()) {
			return false;
		}
	}
	auto &function_name = window.FunctionName().GetIdentifierName();
	if (window.Filter() && SQLiteAggregateFunctions().count(function_name) == 0) {
		// SQLite only supports FILTER for aggregate window functions
		return false;
	}
	if (function_name == "lead" || function_name == "lag") {
		// only push down literal non-negative offsets
		if (children.empty() || children.size() > 3) {
			return false;
		}
		if (children.size() >= 2 && !SQLiteIsIntegerLiteralAtLeast(children[1].GetExpression(), 0)) {
			return false;
		}
	} else if (function_name == "ntile") {
		if (children.size() != 1 || !SQLiteIsIntegerLiteralAtLeast(children[0].GetExpression(), 1)) {
			return false;
		}
	} else if (function_name == "nth_value") {
		if (children.size() != 2 || !SQLiteIsIntegerLiteralAtLeast(children[1].GetExpression(), 1)) {
			return false;
		}
	}
	static const case_insensitive_set_t WINDOW_FUNCTIONS = {
	    "row_number", "rank", "dense_rank", "rank_dense",  "percent_rank", "cume_dist",
	    "ntile",      "lead", "lag",        "first_value", "last_value",   "nth_value",
	};
	if (WINDOW_FUNCTIONS.count(function_name) > 0) {
		return true;
	}
	if (SQLiteAggregateFunctions().count(function_name) == 0) {
		return false;
	}
	// string_agg / group_concat with DISTINCT or as a window function have no separator argument in SQLite
	static const case_insensitive_set_t STRING_AGG_FUNCTIONS = {"string_agg", "group_concat", "listagg"};
	if (STRING_AGG_FUNCTIONS.count(function_name) > 0) {
		return children.size() == 1 || (children.size() == 2 && SQLiteIsStringLiteral(children[1].GetExpression()));
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Capabilities
//===--------------------------------------------------------------------===//
bool SQLiteCapabilities::Supports(RemoteCapability capability) const {
	switch (capability) {
	case RemoteCapability::IS_REMOTE:
	case RemoteCapability::EXECUTE_QUERY_NODE:
	case RemoteCapability::EXECUTE_STATEMENT:
		return true;
	default:
		return false;
	}
}

bool SQLiteCapabilities::SupportsPushdown(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::CONSTANT:
		return SupportsValue(expr.Cast<ConstantExpression>().GetLiteral().ToValue());
	case ExpressionClass::CAST: {
		auto &cast_expr = expr.Cast<CastExpression>();
		auto target_type = ResolveType(cast_expr.TargetType());
		if (target_type.id() == LogicalTypeId::INVALID) {
			return false;
		}
		if (cast_expr.Child().GetExpressionClass() == ExpressionClass::CONSTANT) {
			// casts of literals are evaluated locally and serialized as the resulting value
			Value cast_result;
			return TryGetConstantValue(expr, cast_result) && SupportsValue(cast_result);
		}
		// whether the cast is supported also depends on the source type - which is checked when serializing
		return SQLiteFunctions::HasRepresentation(target_type);
	}
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		auto &function_name = func.FunctionName().GetIdentifierName();
		interval_t interval;
		if (TryGetIntervalLiteral(expr, interval)) {
			// intervals are only supported as operands of date / time arithmetic - which is verified by the writer
			return true;
		}
		if (func.GetQualifiedName().Path().size() > 1) {
			// qualified function names refer to (user-defined) functions in the remote catalog
			return false;
		}
		if (func.ExportState()) {
			return false;
		}
		if (func.OrderBy() && !func.OrderBy()->orders.empty()) {
			// ordered aggregates require SQLite 3.44 - the ORDER BY of aggregates whose result does not depend on the
			// order is skipped (which is verified by the writer), string_agg is evaluated with duckdb_string_agg
			static const case_insensitive_set_t ORDERED_AGGREGATES = {
			    "count", "count_star", "min",  "max",        "bool_and",     "bool_or",
			    "sum",   "avg",        "mean", "string_agg", "group_concat", "listagg"};
			if (ORDERED_AGGREGATES.count(function_name) == 0 || func.Distinct() ||
			    !SupportsOrderEntries(func.OrderBy()->orders)) {
				return false;
			}
		}
		if (func.Filter() && !IsAggregateFunction(function_name)) {
			return false;
		}
		auto &children = func.GetArguments();
		for (auto &child : children) {
			if (child.HasName()) {
				// named arguments are not supported
				return false;
			}
		}
		if (func.Distinct()) {
			// SQLite only supports DISTINCT for aggregates with a single argument
			static const case_insensitive_set_t DISTINCT_AGGREGATES = {
			    "count", "sum", "avg", "mean", "min", "max", "string_agg", "group_concat", "listagg"};
			if (DISTINCT_AGGREGATES.count(function_name) == 0) {
				return false;
			}
		}
		static const case_insensitive_set_t STRING_AGG_FUNCTIONS = {"string_agg", "group_concat", "listagg"};
		if (STRING_AGG_FUNCTIONS.count(function_name) > 0) {
			// a NULL separator yields NULL in DuckDB - only push down literal separators, SQLite does not accept a
			// separator for DISTINCT aggregates
			if (children.empty() || children.size() > 2) {
				return false;
			}
			if (children.size() == 2) {
				Value separator;
				if (!TryGetConstantValue(children[1].GetExpression(), separator) || separator.IsNull() ||
				    separator.type().id() != LogicalTypeId::VARCHAR) {
					return false;
				}
				if (func.Distinct() && StringValue::Get(separator) != ",") {
					return false;
				}
			}
		}
		if ((function_name == "count_if" || function_name == "countif") && (children.size() != 1 || func.Distinct())) {
			return false;
		}
		static const case_insensitive_set_t ARG_FUNCTIONS = {"arg_min", "arg_max", "min_by",
		                                                     "max_by",  "argmin",  "argmax"};
		if (ARG_FUNCTIONS.count(function_name) > 0 && children.size() != 2) {
			return false;
		}
		if (function_name == "median" && children.size() != 1) {
			return false;
		}
		static const case_insensitive_set_t QUANTILE_FUNCTIONS = {"quantile", "quantile_cont", "quantile_disc"};
		if (QUANTILE_FUNCTIONS.count(function_name) > 0) {
			// only a single literal quantile is supported (DuckDB returns a list for a list of quantiles)
			Value quantile;
			if (children.size() != 2 || !TryGetConstantValue(children[1].GetExpression(), quantile) ||
			    !quantile.type().IsNumeric()) {
				return false;
			}
			auto quantile_value = quantile.DefaultTryCastAs(LogicalType::DOUBLE);
			if (!quantile_value || quantile_value->IsNull() || DoubleValue::Get(*quantile_value) < 0 ||
			    DoubleValue::Get(*quantile_value) > 1) {
				return false;
			}
		}
		if ((function_name == "count_star" && !children.empty()) || (function_name == "count" && children.size() > 1)) {
			return false;
		}
		static const case_insensitive_set_t LIKE_FUNCTIONS = {"~~", "!~~", "~~*", "!~~*", "~~~", "glob"};
		if (LIKE_FUNCTIONS.count(function_name) > 0 && children.size() != 2) {
			return false;
		}
		static const case_insensitive_set_t LIKE_ESCAPE_FUNCTIONS = {"like_escape", "not_like_escape", "ilike_escape",
		                                                             "not_ilike_escape"};
		if (LIKE_ESCAPE_FUNCTIONS.count(function_name) > 0 && children.size() != 3) {
			return false;
		}
		if ((function_name == "trim" || function_name == "ltrim" || function_name == "rtrim") && children.size() > 2) {
			return false;
		}
		if ((function_name == "concat_ws" && children.size() < 2) || (function_name == "concat" && children.empty())) {
			return false;
		}
		return SupportsFunction(func);
	}
	case ExpressionClass::WINDOW:
		return SupportsWindow(expr.Cast<WindowExpression>());
	case ExpressionClass::COMPARISON:
		switch (expr.GetExpressionType()) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOTEQUAL:
		case ExpressionType::COMPARE_DISTINCT_FROM:
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			return true;
		default:
			return false;
		}
	case ExpressionClass::BETWEEN:
		return true;
	case ExpressionClass::OPERATOR:
		switch (expr.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
		case ExpressionType::OPERATOR_IS_NOT_NULL:
		case ExpressionType::OPERATOR_NOT:
		case ExpressionType::OPERATOR_COALESCE:
		case ExpressionType::OPERATOR_NULLIF:
		case ExpressionType::COMPARE_IN:
		case ExpressionType::COMPARE_NOT_IN:
			return true;
		default:
			// TRY(...), list / struct operators, GROUPING(...), ... are not supported
			return false;
		}
	case ExpressionClass::SUBQUERY: {
		auto &subquery = expr.Cast<SubqueryExpression>();
		switch (subquery.GetSubqueryType()) {
		case SubqueryType::EXISTS:
		case SubqueryType::NOT_EXISTS:
		case SubqueryType::SCALAR:
			return true;
		case SubqueryType::ANY:
			// x = ANY (subquery) is x IN (subquery) - SQLite has no other quantified comparisons
			return subquery.GetComparisonType() == ExpressionType::COMPARE_EQUAL;
		default:
			return false;
		}
	}
	case ExpressionClass::STAR: {
		auto &star = expr.Cast<StarExpression>();
		if (!star.ExcludeList().empty() || !star.ReplaceList().empty() || !star.RenameList().empty()) {
			// EXCLUDE / REPLACE / RENAME are DuckDB-specific
			return false;
		}
		if (star.Expression()) {
			// COLUMNS(...) expressions are DuckDB-specific
			return false;
		}
		// COLUMNS(*) is DuckDB-specific, but ORDER BY ALL is represented the same way - those are verified
		// by SupportsPushdown(QueryNode)
		return true;
	}
	case ExpressionClass::COLUMN_REF: {
		auto &colref = expr.Cast<ColumnRefExpression>();
		if (!colref.IsQualified()) {
			// SQL value functions without parentheses (CURRENT_DATE, ...) are parsed as column references - these
			// depend on DuckDB's time zone
			static const case_insensitive_set_t VALUE_FUNCTIONS = {
			    "current_date",  "current_time", "current_timestamp", "localtime",    "localtimestamp",
			    "current_user",  "user",         "session_user",      "current_role", "current_catalog",
			    "current_schema"};
			if (VALUE_FUNCTIONS.count(colref.GetColumnName().GetIdentifierName()) > 0) {
				return false;
			}
		}
		// struct field accesses are not supported
		return colref.ColumnNames().size() <= 3;
	}
	case ExpressionClass::CASE:
	case ExpressionClass::CONJUNCTION:
		return true;
	case ExpressionClass::DEFAULT:
		// DEFAULT in an INSERT VALUES list - anywhere else it is rejected by the binder or the writer
		return true;
	default:
		// parameters, COLLATE, lambdas, positional references, ... - not supported
		return false;
	}
}

bool SQLiteCapabilities::SupportsPushdown(const TableRef &ref) {
	if (ref.sample) {
		// TABLESAMPLE is not supported by SQLite
		return false;
	}
	switch (ref.type) {
	case TableReferenceType::BASE_TABLE: {
		auto &base = ref.Cast<BaseTableRef>();
		if (base.at_clause) {
			// AT (...) time travel clauses are DuckDB-specific
			return false;
		}
		return true;
	}
	case TableReferenceType::JOIN: {
		auto &join = ref.Cast<JoinRef>();
		switch (join.ref_type) {
		case JoinRefType::REGULAR:
		case JoinRefType::NATURAL:
		case JoinRefType::CROSS:
			break;
		default:
			// ASOF / POSITIONAL / ... joins are DuckDB-specific
			return false;
		}
		switch (join.type) {
		case JoinType::INNER:
		case JoinType::LEFT:
			break;
		case JoinType::RIGHT:
			// RIGHT joins require SQLite 3.39 - they are serialized as a LEFT join with the sides swapped, which
			// changes the order of the columns of an unqualified * (which is verified by the writer)
			if (join.ref_type != JoinRefType::REGULAR || !join.condition) {
				return false;
			}
			break;
		case JoinType::SEMI:
		case JoinType::ANTI:
			// serialized as a [NOT] EXISTS filter over the left side - which must be a single (named) relation
			if (join.ref_type != JoinRefType::REGULAR || !join.condition || !join.using_columns.empty()) {
				return false;
			}
			if (join.left->type == TableReferenceType::BASE_TABLE) {
				break;
			}
			if (join.left->type == TableReferenceType::SUBQUERY && !join.left->alias.empty()) {
				break;
			}
			return false;
		default:
			return false;
		}
		if (!join.duplicate_eliminated_columns.empty()) {
			return false;
		}
		return true;
	}
	case TableReferenceType::SUBQUERY:
	case TableReferenceType::EMPTY_FROM:
		return true;
	case TableReferenceType::EXPRESSION_LIST: {
		auto &expression_list = ref.Cast<ExpressionListRef>();
		return !expression_list.values.empty();
	}
	default:
		// table functions, pivots, ... - not supported
		return false;
	}
}

static void SQLiteCollectTableNames(QueryNode &node, identifier_set_t &cte_names, identifier_set_t &table_names,
                                    bool all_tables);

static void SQLiteCollectTableNames(ParsedExpression &expr, identifier_set_t &cte_names, identifier_set_t &table_names,
                                    bool all_tables) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		SQLiteCollectTableNames(*expr.Cast<SubqueryExpression>().SubqueryMutable()->node, cte_names, table_names,
		                        all_tables);
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](ParsedExpression &child) { SQLiteCollectTableNames(child, cte_names, table_names, all_tables); });
}

//! Collect the names of all CTEs, and of all qualified table references (or of all table references)
static void SQLiteCollectTableNames(QueryNode &node, identifier_set_t &cte_names, identifier_set_t &table_names,
                                    bool all_tables) {
	for (auto &cte : node.cte_map.map) {
		cte_names.insert(cte.first);
	}
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    node,
	    [&](unique_ptr<ParsedExpression> &child) {
		    SQLiteCollectTableNames(*child, cte_names, table_names, all_tables);
	    },
	    [&](TableRef &ref) {
		    if (ref.type == TableReferenceType::BASE_TABLE) {
			    auto &base = ref.Cast<BaseTableRef>();
			    if (all_tables || base.GetQualifiedName().Path().size() > 1) {
				    table_names.insert(base.Table());
			    }
		    }
	    });
}

//! The catalog qualification is stripped from table references before the query is serialized - a qualified
//! reference to a table with the same name as a CTE would then refer to the CTE
static bool SQLiteHasCTETableNameConflict(const QueryNode &node) {
	auto copy = node.Copy();
	identifier_set_t cte_names;
	identifier_set_t qualified_tables;
	SQLiteCollectTableNames(*copy, cte_names, qualified_tables, false);
	for (auto &name : cte_names) {
		if (qualified_tables.count(name) > 0) {
			return true;
		}
	}
	return false;
}

bool SQLiteCapabilities::SupportsPushdown(const QueryNode &node) {
	if (!SupportsCTEMap(node.cte_map)) {
		return false;
	}
	if (!node.cte_map.map.empty() && SQLiteHasCTETableNameConflict(node)) {
		return false;
	}
	switch (node.type) {
	case QueryNodeType::SELECT_NODE: {
		auto &select = node.Cast<SelectNode>();
		if (select.sample) {
			// SQLite has no USING SAMPLE
			return false;
		}
		// QUALIFY / DISTINCT ON are serialized as a filter over a subquery - which requires named result columns
		bool has_distinct_on = false;
		for (auto &modifier : node.modifiers) {
			if (modifier->type == ResultModifierType::DISTINCT_MODIFIER &&
			    !modifier->Cast<DistinctModifier>().distinct_on_targets.empty()) {
				has_distinct_on = true;
			}
		}
		if (select.qualify || has_distinct_on || (select.having && select.groups.grouping_sets.empty())) {
			for (auto &expr : select.select_list) {
				if (expr->GetExpressionClass() == ExpressionClass::STAR) {
					return false;
				}
			}
		}
		if (select.aggregate_handling == AggregateHandling::FORCE_AGGREGATES) {
			// GROUP BY ALL is DuckDB-specific
			return false;
		}
		if (select.groups.grouping_sets.size() > 1) {
			// SQLite has no GROUPING SETS / ROLLUP / CUBE
			return false;
		}
		if (!SupportsSelectAliasUsage(select)) {
			return false;
		}
		vector<reference<const ParsedExpression>> expressions;
		for (auto &expr : select.select_list) {
			expressions.push_back(*expr);
		}
		for (auto &expr : select.groups.group_expressions) {
			expressions.push_back(*expr);
		}
		if (select.where_clause) {
			expressions.push_back(*select.where_clause);
		}
		if (select.having) {
			expressions.push_back(*select.having);
		}
		for (auto &expr : expressions) {
			if (SQLiteContainsColumnsStar(expr.get())) {
				// COLUMNS(*) is DuckDB-specific
				return false;
			}
		}
		if (select.from_table && SQLiteJoinTreeHasCoalescingJoin(*select.from_table)) {
			for (auto &expr : select.select_list) {
				if (expr->GetExpressionClass() == ExpressionClass::STAR &&
				    expr->Cast<StarExpression>().RelationName().empty()) {
					return false;
				}
			}
		}
		return SupportsResultModifiers(node, &select.select_list);
	}
	case QueryNodeType::SET_OPERATION_NODE: {
		auto &setop = node.Cast<SetOperationNode>();
		switch (setop.setop_type) {
		case SetOperationType::UNION:
			break;
		case SetOperationType::EXCEPT:
		case SetOperationType::INTERSECT:
			if (setop.setop_all) {
				// SQLite has no EXCEPT ALL / INTERSECT ALL
				return false;
			}
			break;
		default:
			// UNION BY NAME is DuckDB-specific
			return false;
		}
		for (auto &modifier : node.modifiers) {
			if (modifier->type != ResultModifierType::ORDER_MODIFIER) {
				continue;
			}
			for (auto &order : modifier->Cast<OrderModifier>().orders) {
				// SQLite can only order the result of a set operation by output column names or positions
				auto &expr = *order.expression;
				bool is_name = expr.GetExpressionClass() == ExpressionClass::COLUMN_REF &&
				               !expr.Cast<ColumnRefExpression>().IsQualified();
				if (!is_name && !IsIntegerLiteral(expr)) {
					return false;
				}
			}
		}
		return SupportsResultModifiers(node);
	}
	case QueryNodeType::RECURSIVE_CTE_NODE: {
		auto &cte = node.Cast<RecursiveCTENode>();
		if (!cte.key_targets.empty()) {
			// USING KEY is DuckDB-specific
			return false;
		}
		return node.modifiers.empty();
	}
	case QueryNodeType::UPDATE_QUERY_NODE: {
		auto &update = node.Cast<UpdateQueryNode>();
		if (read_only) {
			// the read-only check is performed by DuckDB when executing the statement locally
			return false;
		}
		if (!update.set_info || !update.table || update.table->type != TableReferenceType::BASE_TABLE) {
			return false;
		}
		// SQLite does not support ORDER BY / LIMIT in UPDATE statements (by default)
		return node.modifiers.empty();
	}
	case QueryNodeType::DELETE_QUERY_NODE: {
		auto &del = node.Cast<DeleteQueryNode>();
		if (read_only) {
			return false;
		}
		if (!del.table || del.table->type != TableReferenceType::BASE_TABLE) {
			return false;
		}
		return node.modifiers.empty();
	}
	case QueryNodeType::INSERT_QUERY_NODE: {
		auto &insert = node.Cast<InsertQueryNode>();
		if (read_only) {
			return false;
		}
		if (insert.column_order != InsertColumnOrder::INSERT_BY_POSITION) {
			// INSERT BY NAME is DuckDB-specific
			return false;
		}
		if (!insert.select_statement && !insert.default_values) {
			return false;
		}
		if (insert.on_conflict_info) {
			auto &on_conflict = *insert.on_conflict_info;
			if (on_conflict.condition) {
				// conflict targets on partial indexes are not supported
				return false;
			}
			switch (on_conflict.action_type) {
			case OnConflictAction::THROW:
			case OnConflictAction::NOTHING:
			case OnConflictAction::REPLACE:
				break;
			case OnConflictAction::UPDATE:
				if (!on_conflict.set_info) {
					return false;
				}
				break;
			default:
				return false;
			}
		}
		return node.modifiers.empty();
	}
	default:
		// MERGE is not supported by SQLite
		return false;
	}
}

//! Statements cannot be executed locally instead when they turn out not to be translatable - a CREATE TABLE AS is only
//! pushed down if its query only uses constructs that are translated whatever types they operate on (otherwise only
//! the query is pushed down, and DuckDB inserts its result)
bool SQLiteCapabilities::SupportsStatementExpression(ParsedExpression &expr) {
	if (!SupportsPushdown(expr)) {
		return false;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::CONSTANT: {
		auto type_id = expr.Cast<ConstantExpression>().GetLiteral().ToValue().type().id();
		if (type_id == LogicalTypeId::DECIMAL || type_id == LogicalTypeId::FLOAT) {
			// these are not represented exactly
			return false;
		}
		break;
	}
	case ExpressionClass::COLUMN_REF:
	case ExpressionClass::STAR:
	case ExpressionClass::COMPARISON:
	case ExpressionClass::CONJUNCTION:
	case ExpressionClass::BETWEEN:
		break;
	case ExpressionClass::OPERATOR:
		switch (expr.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
		case ExpressionType::OPERATOR_IS_NOT_NULL:
		case ExpressionType::OPERATOR_NOT:
		case ExpressionType::COMPARE_IN:
		case ExpressionType::COMPARE_NOT_IN:
			break;
		default:
			return false;
		}
		break;
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		static const case_insensitive_set_t TYPE_INDEPENDENT_FUNCTIONS = {"count", "count_star", "min", "max"};
		if (TYPE_INDEPENDENT_FUNCTIONS.count(func.FunctionName().GetIdentifierName()) == 0) {
			return false;
		}
		break;
	}
	case ExpressionClass::SUBQUERY: {
		auto &subquery = expr.Cast<SubqueryExpression>();
		if (subquery.GetSubqueryType() != SubqueryType::EXISTS &&
		    subquery.GetSubqueryType() != SubqueryType::NOT_EXISTS) {
			return false;
		}
		if (!SupportsStatementQuery(*subquery.SubqueryMutable()->node)) {
			return false;
		}
		break;
	}
	default:
		return false;
	}
	bool result = true;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](ParsedExpression &child) {
		if (result && !SupportsStatementExpression(child)) {
			result = false;
		}
	});
	return result;
}

bool SQLiteCapabilities::SupportsStatementQuery(QueryNode &node) {
	if (!SupportsPushdown(node)) {
		return false;
	}
	bool result = true;
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    node,
	    [&](unique_ptr<ParsedExpression> &child) {
		    if (result && !SupportsStatementExpression(*child)) {
			    result = false;
		    }
	    },
	    [&](TableRef &ref) {
		    if (!result) {
			    return;
		    }
		    if (!SupportsPushdown(ref) || !ref.column_name_alias.empty()) {
			    result = false;
		    } else if (ref.type == TableReferenceType::SUBQUERY &&
		               !SupportsStatementQuery(*ref.Cast<SubqueryRef>().subquery->node)) {
			    result = false;
		    }
	    });
	if (!result) {
		return false;
	}
	for (auto &cte : node.cte_map.map) {
		if (cte.second->query_node && !SupportsStatementQuery(*cte.second->query_node)) {
			return false;
		}
	}
	switch (node.type) {
	case QueryNodeType::SELECT_NODE:
		break;
	case QueryNodeType::SET_OPERATION_NODE:
		for (auto &child : node.Cast<SetOperationNode>().children) {
			if (!SupportsStatementQuery(*child)) {
				return false;
			}
		}
		break;
	default:
		return false;
	}
	return true;
}

bool SQLiteCapabilities::SupportsPushdown(const SQLStatement &statement) {
	if (read_only) {
		// the read-only check is performed by DuckDB when executing the statement locally
		return false;
	}
	switch (statement.type) {
	case StatementType::CREATE_STATEMENT: {
		auto &create_info = *statement.Cast<CreateStatement>().info;
		if (create_info.temporary) {
			return false;
		}
		switch (create_info.type) {
		case CatalogType::TABLE_ENTRY: {
			auto &table_info = create_info.Cast<CreateTableInfo>();
			if (!table_info.query) {
				// a regular CREATE TABLE issues the same statement
				return false;
			}
			if (table_info.on_conflict != OnCreateConflict::ERROR_ON_CONFLICT &&
			    table_info.on_conflict != OnCreateConflict::REPLACE_ON_CONFLICT) {
				// the query must not be executed when the table exists
				return false;
			}
			if (!table_info.partition_keys.empty() || !table_info.sort_keys.empty() || !table_info.options.empty()) {
				return false;
			}
			auto query = table_info.query->node->Copy();
			if (table_info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
				// the existing table is dropped before the query is executed - the query must not read it
				identifier_set_t cte_names;
				identifier_set_t table_names;
				SQLiteCollectTableNames(*query, cte_names, table_names, true);
				if (table_names.count(table_info.GetQualifiedName().Name()) > 0) {
					return false;
				}
			}
			return SupportsStatementQuery(*query);
		}
		case CatalogType::VIEW_ENTRY: {
			// views that cannot be translated are stored with the SQL of a regular CREATE VIEW
			auto &view_info = create_info.Cast<CreateViewInfo>();
			return view_info.query != nullptr;
		}
		default:
			return false;
		}
	}
	case StatementType::DROP_STATEMENT: {
		auto &drop_info = *statement.Cast<DropStatement>().info;
		switch (drop_info.type) {
		case CatalogType::TABLE_ENTRY:
		case CatalogType::VIEW_ENTRY:
		case CatalogType::INDEX_ENTRY:
			// SQLite has no DROP ... CASCADE
			return !drop_info.cascade && drop_info.GetQualifiedName().Path().size() <= 2;
		default:
			return false;
		}
	}
	case StatementType::ALTER_STATEMENT: {
		auto &alter_info = *statement.Cast<AlterStatement>().info;
		if (alter_info.type != AlterType::ALTER_TABLE) {
			return false;
		}
		auto &alter_table_info = alter_info.Cast<AlterTableInfo>();
		switch (alter_table_info.alter_table_type) {
		case AlterTableType::RENAME_TABLE:
		case AlterTableType::RENAME_COLUMN:
			return true;
		case AlterTableType::REMOVE_COLUMN:
			return !alter_table_info.Cast<RemoveColumnInfo>().cascade;
		case AlterTableType::ADD_COLUMN: {
			auto &column = alter_table_info.Cast<AddColumnInfo>().new_column;
			if (column.HasDefaultValue() || column.Generated()) {
				// DuckDB fills in the default values of the existing rows itself
				return false;
			}
			LogicalType type = column.Type();
			if (type.id() == LogicalTypeId::UNBOUND) {
				auto &type_expr = UnboundType::GetTypeExpression(type);
				if (!type_expr || type_expr->GetExpressionClass() != ExpressionClass::TYPE) {
					return false;
				}
				type = ResolveType(type_expr->Cast<TypeExpression>());
			}
			return type.id() != LogicalTypeId::INVALID;
		}
		default:
			// SQLite cannot change the NOT NULL constraints or the types of columns
			return false;
		}
	}
	default:
		return false;
	}
}

} // namespace duckdb
