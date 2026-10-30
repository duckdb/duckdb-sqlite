//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_sql_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/tableref.hpp"

namespace duckdb {
class BaseTableRef;
class CreateStatement;
class CaseExpression;
class ConjunctionExpression;
class ColumnDefinition;
class ColumnRefExpression;
class DeleteQueryNode;
class ExpressionListRef;
class OnConflictInfo;
class FunctionArgument;
class FunctionExpression;
class InsertQueryNode;
class OperatorExpression;
class RecursiveCTENode;
class SelectNode;
class SetOperationNode;
class SQLiteCatalog;
class SQLiteTableEntry;
class SubqueryExpression;
class TableCatalogEntry;
class UpdateQueryNode;
class UpdateSetInfo;
class WindowExpression;
struct CreateViewInfo;
struct DropInfo;
struct AlterInfo;

//! Thrown by the writer when a construct turns out not to be translatable once its types are known - the query is
//! then executed locally instead
class SQLitePushdownNotSupported : public std::exception {
public:
	explicit SQLitePushdownNotSupported(string reason_p) : reason(std::move(reason_p)) {
	}
	const char *what() const noexcept override {
		return reason.c_str();
	}

private:
	string reason;
};

//! The result of serializing a query or statement for SQLite
struct SQLiteWriterResult {
	//! Statements executed before the main statement (e.g. creating the table of a CREATE TABLE AS)
	vector<string> preliminary_queries;
	//! The main statement, its result is returned to DuckDB
	string sql;
	//! Whether the main statement is a DML statement whose number of changed rows is the result
	bool changed_rows = false;
	//! Whether the statement modifies catalog entries (DDL) - the cached entries are cleared after executing it
	bool clear_catalog_cache = false;
};

//! Serializes a parsed query tree into SQLite SQL.
//! This mirrors the structure of the DuckDB ToString() methods, with SQLite-specific differences applied during
//! serialization:
//! * columns that SQLite stores differently than DuckDB reads them (dates, timestamps, untyped columns, columns
//!   with a collation, ...) are converted the way the table scan converts them
//! * functions and operators are remapped where semantics are identical under a different name or form (e.g.
//!   count_star() -> count(*), a / b -> REAL division, LIKE -> GLOB), or are evaluated with the duckdb_* functions
//!   that the extension registers on its SQLite connections
//! * ORDER BY entries get an explicit NULLS FIRST / NULLS LAST
//! Constructs that cannot be serialized are rejected up-front by SQLiteCapabilities, or with
//! SQLitePushdownNotSupported when that depends on the types of the expressions.
class SQLiteSQLWriter {
public:
	SQLiteSQLWriter(ClientContext &context, SQLiteCatalog &catalog, bool statement_mode);

	//! Serialize a query node
	static SQLiteWriterResult WriteQuery(ClientContext &context, SQLiteCatalog &catalog, QueryNode &node);
	//! Serialize a (DDL) statement
	static SQLiteWriterResult WriteStatement(ClientContext &context, SQLiteCatalog &catalog, SQLStatement &statement);

	//! Collect the names of all CTEs defined anywhere in the query
	static void CollectCTENames(QueryNode &node, identifier_set_t &cte_names);
	//! Qualify all base table references with the catalog name again (the optimizer strips it before pushdown), so
	//! that the query can be bound by DuckDB itself - except for references to the CTEs that are visible where they are
	//! (cte_names are the CTEs that are visible to the query)
	static void QualifyTables(QueryNode &node, const Identifier &catalog_name, const identifier_set_t &cte_names);

private:
	//! Collect the columns that are referenced by the query - columns that are not referenced are not converted
	void CollectColumnReferences(QueryNode &node);
	optional_ptr<SQLiteTableEntry> LookupTable(const BaseTableRef &ref);
	optional_ptr<SQLiteTableEntry> LookupTable(const QualifiedName &qualified_name);
	bool IsCTEReference(const BaseTableRef &ref);

	//! The type DuckDB binds an expression at the current position in the query to - INVALID if unknown
	LogicalType InferType(const ParsedExpression &expr);
	//! As InferType, but throws if the type cannot be determined
	LogicalType RequireType(const ParsedExpression &expr);
	//! The type of a column of a base table of an enclosing query (e.g. a correlated reference)
	LogicalType InferOuterColumnType(const ColumnRefExpression &column_ref);
	//! The result types and names of a query (empty if they cannot be determined)
	vector<LogicalType> InferQueryTypes(const QueryNode &node, vector<string> *names = nullptr);
	void CollectScopeTables(const TableRef &ref, vector<reference<const BaseTableRef>> &tables, bool &only_base_tables);
	optional_ptr<TableCatalogEntry> FindBindingTable(const Identifier &binding);

	//! The functions of the extension cannot be used in statements, they are executed (or stored) as is
	void RequireExtensionFunctions(const string &construct);

	string WriteQueryNode(const QueryNode &node);
	void WriteCreateTable(CreateStatement &statement, SQLiteWriterResult &result);
	void WriteCreateView(CreateViewInfo &info, SQLiteWriterResult &result);
	string WriteDrop(const DropInfo &info);
	string WriteAlter(const AlterInfo &info);
	void VerifyEntryExists(CatalogType type, const QualifiedName &name);
	string WriteUpdateNode(const UpdateQueryNode &node);
	string WriteDeleteNode(const DeleteQueryNode &node);
	string WriteInsertNode(const InsertQueryNode &node);
	string WriteOnConflict(const OnConflictInfo &info, SQLiteTableEntry &table, const Identifier &target_binding,
	                       const vector<reference<const ColumnDefinition>> &columns,
	                       optional_ptr<const ExpressionListRef> values_list);
	string WriteSetClause(const UpdateSetInfo &set_info, SQLiteTableEntry &table);
	string WriteAssignment(const ParsedExpression &expr, const ColumnDefinition &column);
	string WriteReturningList(const vector<unique_ptr<ParsedExpression>> &returning_list, SQLiteTableEntry &table);
	SQLiteTableEntry &GetTargetTable(const TableRef &ref);
	//! Whether a FROM clause item provides a column with the given name
	bool ProvidesColumn(const TableRef &ref, const Identifier &name);
	//! The target table column a reference refers to, if it is a reference to a column of the target of a DML
	//! statement
	optional_ptr<const ColumnDefinition> ResolveTargetColumn(const ColumnRefExpression &column_ref);
	string WriteSelectNode(const SelectNode &node);
	//! Writes a SELECT whose alias references have been expanded
	string WriteExpandedSelectNode(const SelectNode &node);
	string WriteFilteredSelectNode(const SelectNode &node);
	string WriteSetOperationNode(const SetOperationNode &node);
	string WriteSetOperationChild(const QueryNode &node);
	//! A double that SQLite reads exactly
	string WriteDouble(double value);
	//! The WHERE clause of an UPDATE / DELETE with the given condition, which checks the values of the columns the
	//! statement reads
	string WriteDMLCondition(const string &condition);
	//! A copy of a SELECT whose references to aliases of its select list are replaced with the aliased expressions
	//! (nullptr if it has none) - SQLite resolves aliases in fewer places than DuckDB
	unique_ptr<QueryNode> ExpandSelectAliases(const SelectNode &node);
	//! A copy of a member of a set operation whose result columns are cast to the result types of the set operation
	//! (nullptr if they have these types)
	unique_ptr<QueryNode> CastSetOperationChild(const QueryNode &node, const vector<LogicalType> &types);
	string WriteRecursiveCTENode(const RecursiveCTENode &node);
	string WriteCTEMap(const CommonTableExpressionMap &cte_map);
	string WriteResultModifiers(const QueryNode &node);
	string WriteOrderList(const vector<OrderByNode> &orders, optional_ptr<const QueryNode> node = nullptr);
	string WriteOrderingKey(const ParsedExpression &expr);
	//! The name of the (single) result column of a query
	string GetResultColumnName(const QueryNode &node);

	string WriteTableRef(const TableRef &ref);
	string WriteBaseTableRef(const BaseTableRef &ref);
	//! Rename the columns of a relation (SQLite has no column alias lists for relations)
	string WriteColumnAliases(const string &relation, const vector<Identifier> &aliases, const vector<string> &names,
	                          const string &alias);

	string WriteExpression(const ParsedExpression &expr);
	string WriteExpressionList(const vector<unique_ptr<ParsedExpression>> &expressions, const string &separator = ", ");
	string WriteColumnRef(const ColumnRefExpression &column_ref);
	string WriteConstant(const Value &value);
	string WriteFunction(const FunctionExpression &func);
	string WriteAggregate(const FunctionExpression &func, const string &name);
	string WriteAggregateCall(const string &name, const vector<FunctionArgument> &children, bool distinct);
	bool TryWriteScalarFunction(const FunctionExpression &func, const string &name,
	                            const vector<FunctionArgument> &children, string &result);
	bool TryWriteMathFunction(const FunctionExpression &func, const string &name,
	                          const vector<FunctionArgument> &children, string &result);
	bool TryWriteDateFunction(const string &name, const vector<FunctionArgument> &children, string &result);
	string WriteIntervalArithmetic(const ParsedExpression &value, const ParsedExpression &interval, bool subtract);
	string WriteArithmetic(const FunctionExpression &func, const string &name,
	                       const vector<FunctionArgument> &children);
	string WriteDivision(const ParsedExpression &left, const ParsedExpression &right, bool integer_division);
	string WriteLike(const string &name, const vector<FunctionArgument> &children);
	string WriteWindow(const WindowExpression &window);
	string WriteOperator(const OperatorExpression &op);
	string WriteCase(const CaseExpression &case_expr);
	//! Writes an expression that is used as a condition - converted to BOOLEAN like DuckDB does
	string WriteCondition(const ParsedExpression &expr);
	string WriteSubquery(const SubqueryExpression &subquery);
	//! Verify that SQLite can evaluate a (correlated) subquery efficiently
	void VerifyCorrelatedSubquery(const QueryNode &node);
	string WriteDisjunction(const ConjunctionExpression &disjunction);
	string WriteCast(const ParsedExpression &child, const LogicalType &target_type, bool try_cast);
	string WriteCast(const string &child_str, const LogicalType &source_type, const LogicalType &target_type,
	                 bool try_cast = false);
	//! Write an expression converted to a type (e.g. the result type of a CASE)
	string WriteAs(const ParsedExpression &expr, const LogicalType &target_type);
	//! The common type that DuckDB converts a set of expressions to (e.g. the arguments of COALESCE)
	LogicalType CommonType(const vector<reference<const ParsedExpression>> &expressions);
	//! Write the operands of a comparison (or of IN / BETWEEN) - converted to the type DuckDB compares them as
	vector<string> WriteComparisonOperands(const vector<reference<const ParsedExpression>> &operands);
	string WriteIdentifier(const Identifier &identifier);
	static string WriteIdentifier(const string &identifier);

private:
	ClientContext &context;
	SQLiteCatalog &catalog;
	//! Whether a view is serialized - which is stored, so it cannot use the functions of the extension, and is read
	//! back by DuckDB, so it can only use SQL that DuckDB understands
	bool statement_mode;
	//! The CTE names visible at the current position in the query
	vector<reference<const CommonTableExpressionMap>> cte_scopes;
	//! The tables in scope at the current position in the query (e.g. the FROM clause of the enclosing SELECT)
	vector<vector<reference<const unique_ptr<TableRef>>>> table_scopes;
	//! Cache of inferred expression types
	unordered_map<const ParsedExpression *, LogicalType> type_cache;
	//! Rewritten copies of query nodes - kept alive while writing, the type cache refers to their expressions
	vector<unique_ptr<QueryNode>> owned_nodes;
	//! Counter for generating aliases for unnamed subqueries
	idx_t unnamed_subquery_index = 0;
	//! The target table of the DML statement that is being written, the names it is bound to, and the scope of the
	//! statement - references to its columns are converted the way the table scan converts them
	optional_ptr<SQLiteTableEntry> dml_target;
	vector<Identifier> dml_bindings;
	//! The columns of the target of the DML statement that the statement reads
	case_insensitive_set_t dml_read_columns;
	idx_t dml_scope = 0;
	//! Whether column references are written without their table (in a RETURNING list)
	bool unqualified_columns = false;
	//! Whether the select list of the SELECT that is being written has an unqualified *
	bool unqualified_star = false;
	//! The names of the columns that are referenced by the query, and whether it could reference all columns (e.g.
	//! with a star)
	case_insensitive_set_t referenced_columns;
	bool all_columns_referenced = true;
};

} // namespace duckdb
