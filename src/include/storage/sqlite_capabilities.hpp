//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/sqlite_capabilities.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/tableref.hpp"

namespace duckdb {

//! Decides which parsed constructs can be serialized by SQLiteSQLWriter and executed in SQLite with the same
//! semantics as in DuckDB. Checks that depend on the types of the expressions are performed by the writer.
class SQLiteCapabilities {
public:
	explicit SQLiteCapabilities(bool read_only_p = false, Identifier catalog_name_p = Identifier())
	    : read_only(read_only_p), catalog_name(std::move(catalog_name_p)) {
	}

	bool Supports(RemoteCapability capability) const;
	bool SupportsPushdown(const ParsedExpression &expression);
	bool SupportsPushdown(const TableRef &ref);
	bool SupportsPushdown(const QueryNode &node);
	bool SupportsPushdown(const SQLStatement &statement);

	//! Resolve a cast target / column type expression against the built-in DuckDB types,
	//! returns LogicalTypeId::INVALID for types that are not built-in
	static LogicalType ResolveType(const TypeExpression &type_expr);
	//! Constant INTERVAL values are represented as add(add(to_months(m), to_days(d)), to_microseconds(us)) -
	//! recognize this representation
	static bool TryGetIntervalLiteral(const ParsedExpression &expr, interval_t &result);
	//! Whether a constant value can be serialized as a SQLite literal
	static bool SupportsValue(const Value &value);
	//! Get the value of a constant expression (or of a cast of a constant)
	static bool TryGetConstantValue(const ParsedExpression &expr, Value &result);
	static bool TryGetIntegerLiteral(const ParsedExpression &expr, int64_t &result);
	//! Whether an expression is a plain integer literal - in ORDER BY / GROUP BY these are positional references
	static bool IsIntegerLiteral(const ParsedExpression &expr);
	//! Whether a (supported) function is an aggregate function
	static bool IsAggregateFunction(const string &name);

	//! Whether an expression / query can be serialized without depending on the types it operates on, and without
	//! using the functions of the extension - statements that are pushed down cannot be executed locally instead
	bool SupportsStatementExpression(ParsedExpression &expr);
	bool SupportsStatementQuery(QueryNode &node);

private:
	bool SupportsFunction(const FunctionExpression &func);
	bool SupportsWindow(const WindowExpression &window);
	bool SupportsCTEMap(const CommonTableExpressionMap &cte_map);
	bool SupportsResultModifiers(const QueryNode &node,
	                             optional_ptr<const vector<unique_ptr<ParsedExpression>>> select_list = nullptr);
	bool SupportsOrderEntries(const vector<OrderByNode> &orders,
	                          optional_ptr<const vector<unique_ptr<ParsedExpression>>> select_list = nullptr);
	bool SupportsSelectAliasUsage(const SelectNode &select);

private:
	//! Whether the database was attached read-only - DML and DDL are never pushed down then
	bool read_only;
	//! The name of the attached database - a statement can name an entry as <catalog>.<name>
	Identifier catalog_name;
};

} // namespace duckdb
