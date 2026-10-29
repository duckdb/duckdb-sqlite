#include "sqlite_sql_writer.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/function/function_binder.hpp"

#include "sqlite_functions.hpp"
#include "sqlite_utils.hpp"
#include "storage/sqlite_capabilities.hpp"
#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_schema_entry.hpp"
#include "storage/sqlite_table_entry.hpp"
#include "storage/sqlite_transaction.hpp"
#include "sqlite_db.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"

namespace duckdb {

static string SQLiteColumnRead(const SQLiteTableEntry &table, const ColumnDefinition &column, const string &name);

SQLiteSQLWriter::SQLiteSQLWriter(ClientContext &context, SQLiteCatalog &catalog, bool statement_mode)
    : context(context), catalog(catalog), statement_mode(statement_mode) {
}

SQLiteWriterResult SQLiteSQLWriter::WriteQuery(ClientContext &context, SQLiteCatalog &catalog, QueryNode &node) {
	SQLiteSQLWriter writer(context, catalog, false);
	writer.CollectColumnReferences(node);
	SQLiteWriterResult result;
	result.sql = writer.WriteQueryNode(node);
	return result;
}

static void SQLiteCollectColumnReferences(QueryNode &node, case_insensitive_set_t &names, bool &all_columns);

static void SQLiteCollectColumnReferences(ParsedExpression &expr, case_insensitive_set_t &names, bool &all_columns) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF:
		names.insert(expr.Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName());
		break;
	case ExpressionClass::STAR:
		all_columns = true;
		break;
	case ExpressionClass::SUBQUERY:
		SQLiteCollectColumnReferences(*expr.Cast<SubqueryExpression>().SubqueryMutable()->node, names, all_columns);
		break;
	default:
		break;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](ParsedExpression &child) { SQLiteCollectColumnReferences(child, names, all_columns); });
}

//! Collect the names of all columns that are referenced in a query - and whether all columns could be referenced
static void SQLiteCollectColumnReferences(QueryNode &node, case_insensitive_set_t &names, bool &all_columns) {
	for (auto &cte : node.cte_map.map) {
		if (cte.second->query_node) {
			SQLiteCollectColumnReferences(*cte.second->query_node, names, all_columns);
		}
	}
	for (auto &modifier : node.modifiers) {
		switch (modifier->type) {
		case ResultModifierType::ORDER_MODIFIER:
			for (auto &order : modifier->Cast<OrderModifier>().orders) {
				SQLiteCollectColumnReferences(*order.expression, names, all_columns);
			}
			break;
		case ResultModifierType::DISTINCT_MODIFIER:
			for (auto &target : modifier->Cast<DistinctModifier>().distinct_on_targets) {
				SQLiteCollectColumnReferences(*target, names, all_columns);
			}
			break;
		default:
			break;
		}
	}
	std::function<void(TableRef &)> collect_ref = [&](TableRef &ref) {
		if (ref.type == TableReferenceType::JOIN) {
			auto &join = ref.Cast<JoinRef>();
			if (join.ref_type == JoinRefType::NATURAL) {
				all_columns = true;
			}
			for (auto &column : join.using_columns) {
				names.insert(column.GetIdentifierName());
			}
			collect_ref(*join.left);
			collect_ref(*join.right);
		} else if (ref.type == TableReferenceType::SUBQUERY) {
			SQLiteCollectColumnReferences(*ref.Cast<SubqueryRef>().subquery->node, names, all_columns);
		}
	};
	switch (node.type) {
	case QueryNodeType::SELECT_NODE:
		if (node.Cast<SelectNode>().from_table) {
			collect_ref(*node.Cast<SelectNode>().from_table);
		}
		break;
	case QueryNodeType::UPDATE_QUERY_NODE:
		if (node.Cast<UpdateQueryNode>().from_table) {
			collect_ref(*node.Cast<UpdateQueryNode>().from_table);
		}
		break;
	case QueryNodeType::DELETE_QUERY_NODE:
		for (auto &using_clause : node.Cast<DeleteQueryNode>().using_clauses) {
			collect_ref(*using_clause);
		}
		break;
	case QueryNodeType::INSERT_QUERY_NODE: {
		auto &insert = node.Cast<InsertQueryNode>();
		if (insert.select_statement) {
			SQLiteCollectColumnReferences(*insert.select_statement->node, names, all_columns);
		}
		break;
	}
	default:
		break;
	}
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    node, [&](unique_ptr<ParsedExpression> &child) { SQLiteCollectColumnReferences(*child, names, all_columns); },
	    [&](TableRef &ref) {});
}

void SQLiteSQLWriter::CollectColumnReferences(QueryNode &node) {
	referenced_columns.clear();
	all_columns_referenced = false;
	SQLiteCollectColumnReferences(node, referenced_columns, all_columns_referenced);
}

//===--------------------------------------------------------------------===//
// Local binding
//===--------------------------------------------------------------------===//
static void SQLiteCollectCTENamesExpr(ParsedExpression &expr, identifier_set_t &cte_names) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		SQLiteSQLWriter::CollectCTENames(*expr.Cast<SubqueryExpression>().SubqueryMutable()->node, cte_names);
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](ParsedExpression &child) { SQLiteCollectCTENamesExpr(child, cte_names); });
}

void SQLiteSQLWriter::CollectCTENames(QueryNode &node, identifier_set_t &cte_names) {
	for (auto &cte : node.cte_map.map) {
		cte_names.insert(cte.first);
	}
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    node, [&](unique_ptr<ParsedExpression> &child) { SQLiteCollectCTENamesExpr(*child, cte_names); });
}

static void SQLiteQualifyTablesExpr(ParsedExpression &expr, const Identifier &catalog_name,
                                    const identifier_set_t &cte_names) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		SQLiteSQLWriter::QualifyTables(*expr.Cast<SubqueryExpression>().SubqueryMutable()->node, catalog_name,
		                               cte_names);
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](ParsedExpression &child) { SQLiteQualifyTablesExpr(child, catalog_name, cte_names); });
}

void SQLiteSQLWriter::QualifyTables(QueryNode &node, const Identifier &catalog_name,
                                    const identifier_set_t &cte_names) {
	if (node.type == QueryNodeType::INSERT_QUERY_NODE) {
		auto &insert = node.Cast<InsertQueryNode>();
		vector<Identifier> schema_path;
		if (!insert.qualified_name.Schema().empty()) {
			schema_path.push_back(insert.qualified_name.Schema());
		}
		insert.qualified_name =
		    QualifiedName::FromCatalogSchema(catalog_name, std::move(schema_path), insert.qualified_name.Name());
		if (insert.table_ref && insert.table_ref->type == TableReferenceType::BASE_TABLE) {
			// the catalog name is only stripped from the qualified name of the INSERT target
			insert.table_ref->Cast<BaseTableRef>().SetQualifiedName(insert.qualified_name);
		}
	}
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    node, [&](unique_ptr<ParsedExpression> &child) { SQLiteQualifyTablesExpr(*child, catalog_name, cte_names); },
	    [&](TableRef &ref) {
		    if (ref.type != TableReferenceType::BASE_TABLE) {
			    return;
		    }
		    auto &base = ref.Cast<BaseTableRef>();
		    auto &name = base.GetQualifiedName();
		    if (name.Path().size() == 1 && cte_names.count(name.Name()) > 0) {
			    // (possibly) a CTE reference
			    return;
		    }
		    if (name.Path().size() > 2) {
			    return;
		    }
		    vector<Identifier> schema_path;
		    if (!name.Schema().empty()) {
			    schema_path.push_back(name.Schema());
		    }
		    base.SetQualifiedName(QualifiedName::FromCatalogSchema(catalog_name, std::move(schema_path), name.Name()));
	    });
}

optional_ptr<SQLiteTableEntry> SQLiteSQLWriter::LookupTable(const BaseTableRef &ref) {
	return LookupTable(ref.GetQualifiedName());
}

optional_ptr<SQLiteTableEntry> SQLiteSQLWriter::LookupTable(const QualifiedName &qualified_name) {
	if (qualified_name.Path().size() > 2) {
		// the catalog name has been stripped by the optimizer, nested schemas do not exist
		return nullptr;
	}
	Identifier schema_name = qualified_name.Schema();
	if (schema_name.empty()) {
		schema_name = Identifier(DEFAULT_SCHEMA);
	}
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY,
	                       QualifiedName(catalog.GetName(), schema_name, qualified_name.Name()));
	auto entry = Catalog::GetEntry(context, lookup, OnEntryNotFound::RETURN_NULL);
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return &entry->Cast<SQLiteTableEntry>();
}

bool SQLiteSQLWriter::IsCTEReference(const BaseTableRef &ref) {
	if (ref.GetQualifiedName().Path().size() > 1) {
		return false;
	}
	for (auto &scope : cte_scopes) {
		if (scope.get().map.find(ref.Table()) != scope.get().map.end()) {
			return true;
		}
	}
	return false;
}

void SQLiteSQLWriter::CollectScopeTables(const TableRef &ref, vector<reference<const BaseTableRef>> &tables,
                                         bool &only_base_tables) {
	switch (ref.type) {
	case TableReferenceType::BASE_TABLE:
		if (IsCTEReference(ref.Cast<BaseTableRef>())) {
			only_base_tables = false;
		} else {
			tables.push_back(ref.Cast<BaseTableRef>());
		}
		break;
	case TableReferenceType::JOIN: {
		auto &join = ref.Cast<JoinRef>();
		if (join.type == JoinType::SEMI || join.type == JoinType::ANTI || !join.using_columns.empty() ||
		    join.ref_type == JoinRefType::NATURAL) {
			// the bindings of the join differ from the bindings of the joined tables
			only_base_tables = false;
		}
		CollectScopeTables(*join.left, tables, only_base_tables);
		CollectScopeTables(*join.right, tables, only_base_tables);
		break;
	}
	default:
		only_base_tables = false;
		break;
	}
}

optional_ptr<TableCatalogEntry> SQLiteSQLWriter::FindBindingTable(const Identifier &binding) {
	for (idx_t i = table_scopes.size(); i > 0; i--) {
		vector<reference<const BaseTableRef>> tables;
		bool only_base_tables = true;
		for (auto &table_ref : table_scopes[i - 1]) {
			if (!table_ref.get()) {
				continue;
			}
			if (table_ref.get()->alias == binding && table_ref.get()->type != TableReferenceType::BASE_TABLE) {
				// e.g. a subquery
				return nullptr;
			}
			CollectScopeTables(*table_ref.get(), tables, only_base_tables);
		}
		for (auto &base : tables) {
			auto base_binding = base.get().alias.empty() ? base.get().Table() : base.get().alias;
			if (base_binding == binding) {
				return LookupTable(base.get()).get();
			}
		}
	}
	return nullptr;
}

LogicalType SQLiteSQLWriter::InferType(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::CONSTANT) {
		return expr.Cast<ConstantExpression>().GetLiteral().ToValue().type();
	}
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF && !table_scopes.empty()) {
		// fast path: a column of one of the base tables in scope
		auto &names = expr.Cast<ColumnRefExpression>().ColumnNames();
		vector<reference<const BaseTableRef>> tables;
		bool only_base_tables = true;
		for (auto &table_ref : table_scopes.back()) {
			if (table_ref.get()) {
				CollectScopeTables(*table_ref.get(), tables, only_base_tables);
			}
		}
		optional_ptr<TableCatalogEntry> found;
		idx_t matches = 0;
		for (auto &base : tables) {
			auto table = LookupTable(base.get());
			if (!table) {
				only_base_tables = false;
				continue;
			}
			auto binding = base.get().alias.empty() ? base.get().Table() : base.get().alias;
			if (!base.get().column_name_alias.empty()) {
				// the columns are renamed
				only_base_tables = false;
				continue;
			}
			if (names.size() == 1 && table->ColumnExists(names[0])) {
				found = table.get();
				matches++;
			} else if (names.size() == 2 && names[0] == binding && table->ColumnExists(names[1])) {
				return table->GetColumn(names[1]).Type();
			}
		}
		if (names.size() == 1 && only_base_tables && matches == 1) {
			return found->GetColumn(names[0]).Type();
		}
	}
	interval_t interval;
	if (SQLiteCapabilities::TryGetIntervalLiteral(expr, interval)) {
		return LogicalType::INTERVAL;
	}
	auto entry = type_cache.find(&expr);
	if (entry != type_cache.end()) {
		return entry->second;
	}
	// bind a probe query that evaluates the expression over the FROM clause of the enclosing SELECT
	auto probe = make_uniq<SelectNode>();
	auto probe_expr = expr.Copy();
	if (dml_target && table_scopes.size() == dml_scope) {
		// unqualified references to the columns of the target of a DML statement (e.g. in ON CONFLICT DO UPDATE)
		// are qualified, so that they are not ambiguous with the excluded values
		std::function<void(unique_ptr<ParsedExpression> &)> qualify = [&](unique_ptr<ParsedExpression> &child) {
			if (child->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
				auto &column_ref = child->Cast<ColumnRefExpression>();
				if (!column_ref.IsQualified() && ResolveTargetColumn(column_ref)) {
					auto alias = child->GetAlias();
					child = make_uniq<ColumnRefExpression>(column_ref.GetColumnName(), dml_bindings[0]);
					child->SetAlias(alias);
				}
				return;
			}
			if (child->GetExpressionClass() == ExpressionClass::SUBQUERY) {
				return;
			}
			ParsedExpressionIterator::EnumerateChildren(*child, qualify);
		};
		qualify(probe_expr);
	}
	probe->select_list.push_back(std::move(probe_expr));
	if (!table_scopes.empty()) {
		for (auto &table : table_scopes.back()) {
			auto table_copy = table.get()->Copy();
			if (!probe->from_table) {
				probe->from_table = std::move(table_copy);
				continue;
			}
			auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
			join->left = std::move(probe->from_table);
			join->right = std::move(table_copy);
			probe->from_table = std::move(join);
		}
	}
	if (!probe->from_table) {
		probe->from_table = make_uniq<EmptyTableRef>();
	}
	for (idx_t i = cte_scopes.size(); i > 0; i--) {
		// inner CTEs shadow outer ones - insert those first
		for (auto &cte : cte_scopes[i - 1].get().map) {
			probe->cte_map.map.insert(cte.first, cte.second->Copy());
		}
	}
	identifier_set_t cte_names;
	CollectCTENames(*probe, cte_names);
	QualifyTables(*probe, catalog.GetName(), cte_names);
	LogicalType result = LogicalType::INVALID;
	try {
		auto binder = Binder::CreateBinder(context);
		auto bound = binder->Bind(*probe);
		if (bound.types.size() == 1) {
			result = bound.types[0];
		}
	} catch (std::exception &) {
		// e.g. a correlated column reference or a reference to a select-list alias
	}
	if (result.id() == LogicalTypeId::INVALID && expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		result = InferOuterColumnType(expr.Cast<ColumnRefExpression>());
	}
	type_cache[&expr] = result;
	return result;
}

LogicalType SQLiteSQLWriter::RequireType(const ParsedExpression &expr) {
	auto result = InferType(expr);
	if (result.id() == LogicalTypeId::INVALID) {
		throw SQLitePushdownNotSupported("cannot determine the type of " + expr.ToString());
	}
	return result;
}

vector<LogicalType> SQLiteSQLWriter::InferQueryTypes(const QueryNode &node, vector<string> *names) {
	auto probe = node.Copy();
	probe->modifiers.clear();
	for (idx_t i = cte_scopes.size(); i > 0; i--) {
		for (auto &cte : cte_scopes[i - 1].get().map) {
			if (probe->cte_map.map.find(cte.first) == probe->cte_map.map.end()) {
				probe->cte_map.map.insert(cte.first, cte.second->Copy());
			}
		}
	}
	identifier_set_t cte_names;
	CollectCTENames(*probe, cte_names);
	QualifyTables(*probe, catalog.GetName(), cte_names);
	try {
		auto binder = Binder::CreateBinder(context);
		auto bound = binder->Bind(*probe);
		if (names) {
			for (auto &name : bound.names) {
				names->push_back(name.GetIdentifierName());
			}
		}
		return bound.types;
	} catch (std::exception &) {
		// e.g. a correlated column reference
		return vector<LogicalType>();
	}
}

LogicalType SQLiteSQLWriter::InferOuterColumnType(const ColumnRefExpression &column_ref) {
	auto &names = column_ref.ColumnNames();
	if (names.size() == 2) {
		auto table = FindBindingTable(names[0]);
		if (table && table->ColumnExists(names[1])) {
			return table->GetColumn(names[1]).Type();
		}
		return LogicalType::INVALID;
	}
	if (names.size() != 1 || table_scopes.size() < 2) {
		return LogicalType::INVALID;
	}
	// a correlated reference - the innermost enclosing query that has the column decides
	for (idx_t i = table_scopes.size() - 1; i > 0; i--) {
		vector<reference<const BaseTableRef>> tables;
		bool only_base_tables = true;
		for (auto &table_ref : table_scopes[i - 1]) {
			if (table_ref.get()) {
				CollectScopeTables(*table_ref.get(), tables, only_base_tables);
			}
		}
		optional_ptr<TableCatalogEntry> found;
		idx_t matches = 0;
		for (auto &base : tables) {
			auto table = LookupTable(base.get());
			if (!table) {
				only_base_tables = false;
				continue;
			}
			if (table->ColumnExists(names[0])) {
				found = table.get();
				matches++;
			}
		}
		if (matches == 1) {
			return found->GetColumn(names[0]).Type();
		}
		if (matches > 1 || !only_base_tables) {
			return LogicalType::INVALID;
		}
	}
	return LogicalType::INVALID;
}

void SQLiteSQLWriter::RequireExtensionFunctions(const string &construct) {
	if (statement_mode) {
		throw SQLitePushdownNotSupported(construct + " requires the functions of the extension");
	}
}

//===--------------------------------------------------------------------===//
// Types
//===--------------------------------------------------------------------===//
//! How values of a type are represented in SQLite
enum class SQLiteRepresentation : uint8_t { NONE, INTEGER, REAL, TEXT, BLOB, OTHER };

static SQLiteRepresentation SQLiteGetRepresentation(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::SQLNULL:
		return SQLiteRepresentation::NONE;
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
		return SQLiteRepresentation::INTEGER;
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
		return SQLiteRepresentation::REAL;
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
		return SQLiteRepresentation::TEXT;
	case LogicalTypeId::BLOB:
		return SQLiteRepresentation::BLOB;
	default:
		return SQLiteRepresentation::OTHER;
	}
}

static bool SQLiteIsIntegerType(const LogicalType &type) {
	return type.id() != LogicalTypeId::BOOLEAN && SQLiteGetRepresentation(type) == SQLiteRepresentation::INTEGER;
}

//! Numbers whose arithmetic is exact in SQLite (integers) or matches DuckDB's (doubles)
static bool SQLiteIsNumericType(const LogicalType &type) {
	return SQLiteIsIntegerType(type) || type.id() == LogicalTypeId::DOUBLE;
}

//! Numbers that are represented as integers or reals - decimals are only exact where DuckDB converts them to DOUBLE
static bool SQLiteIsNumericOrDecimalType(const LogicalType &type) {
	return SQLiteIsNumericType(type) || type.id() == LogicalTypeId::DECIMAL;
}

static bool SQLiteIsTimestampType(const LogicalType &type) {
	return type.id() == LogicalTypeId::DATE || type.id() == LogicalTypeId::TIMESTAMP;
}

static bool SQLiteIsStringType(const LogicalType &type) {
	return type.id() == LogicalTypeId::VARCHAR || type.id() == LogicalTypeId::SQLNULL;
}

//! Whether all values of the source type are values of the target type
static bool SQLiteIsIntegerSubtype(const LogicalType &source, const LogicalType &target) {
	if (!SQLiteIsIntegerType(source) || !SQLiteIsIntegerType(target)) {
		return false;
	}
	if (source == target || target.id() == LogicalTypeId::HUGEINT) {
		return true;
	}
	auto source_min = Value::MinimumValue(source).DefaultCastAs(LogicalType::HUGEINT);
	auto source_max = Value::MaximumValue(source).DefaultCastAs(LogicalType::HUGEINT);
	auto target_min = Value::MinimumValue(target).DefaultCastAs(LogicalType::HUGEINT);
	auto target_max = Value::MaximumValue(target).DefaultCastAs(LogicalType::HUGEINT);
	return HugeIntValue::Get(source_min) >= HugeIntValue::Get(target_min) &&
	       HugeIntValue::Get(source_max) <= HugeIntValue::Get(target_max);
}

//===--------------------------------------------------------------------===//
// Constants
//===--------------------------------------------------------------------===//
static string SQLiteWriteDouble(double value) {
	if (std::isinf(value)) {
		// SQLite reads literals beyond the range of doubles as infinity
		return value > 0 ? "9e999" : "(-9e999)";
	}
	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%.17g", value);
	string result(buffer);
	if (result.find_first_of(".eE") == string::npos) {
		// SQLite reads literals without a decimal point or exponent as integers
		result += ".0";
	}
	return value < 0 || StringUtil::StartsWith(result, "-") ? "(" + result + ")" : result;
}

string SQLiteSQLWriter::WriteConstant(const Value &value) {
	if (value.IsNull()) {
		return "NULL";
	}
	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
		return BooleanValue::Get(value) ? "1" : "0";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT: {
		auto bigint_value = value.DefaultTryCastAs(LogicalType::BIGINT);
		if (!bigint_value) {
			throw SQLitePushdownNotSupported("integer literal out of range " + value.ToString());
		}
		auto str = bigint_value->ToString();
		return StringUtil::StartsWith(str, "-") ? "(" + str + ")" : str;
	}
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE: {
		auto double_value = value.GetValue<double>();
		if (std::isnan(double_value)) {
			throw SQLitePushdownNotSupported("SQLite has no NaN");
		}
		return SQLiteWriteDouble(double_value);
	}
	case LogicalTypeId::DECIMAL:
		// decimals are only supported where DuckDB converts them to DOUBLE
		return SQLiteWriteDouble(value.GetValue<double>());
	case LogicalTypeId::VARCHAR: {
		auto &str = StringValue::Get(value);
		if (str.find('\0') != string::npos) {
			throw SQLitePushdownNotSupported("string literal with a NUL byte");
		}
		return SQLiteUtils::WriteQuotedAndEscaped(str, '\'');
	}
	case LogicalTypeId::BLOB: {
		static const char *HEX_DIGITS = "0123456789ABCDEF";
		auto &data = StringValue::Get(value);
		string hex = "X'";
		for (auto c : data) {
			auto byte = static_cast<uint8_t>(c);
			hex += HEX_DIGITS[byte >> 4];
			hex += HEX_DIGITS[byte & 0x0F];
		}
		return hex + "'";
	}
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP: {
		string text;
		if (!SQLiteFunctions::TryGetCanonicalText(value, text)) {
			throw SQLitePushdownNotSupported("unsupported " + value.type().ToString() + " literal " + value.ToString());
		}
		return SQLiteUtils::WriteQuotedAndEscaped(text, '\'');
	}
	default:
		throw SQLitePushdownNotSupported("unsupported literal of type " + value.type().ToString());
	}
}

string SQLiteSQLWriter::WriteIdentifier(const string &identifier) {
	return SQLiteUtils::WriteQuotedAndEscaped(identifier, '"');
}

string SQLiteSQLWriter::WriteIdentifier(const Identifier &identifier) {
	return WriteIdentifier(identifier.GetIdentifierName());
}

//===--------------------------------------------------------------------===//
// Casts
//===--------------------------------------------------------------------===//
static string SQLiteTypeName(const LogicalType &type) {
	return LogicalTypeIdToString(type.id());
}

string SQLiteSQLWriter::WriteCast(const ParsedExpression &child, const LogicalType &target_type, bool try_cast) {
	auto source_type = RequireType(child);
	return WriteCast(WriteExpression(child), source_type, target_type, try_cast);
}

string SQLiteSQLWriter::WriteCast(const string &child_str, const LogicalType &source_type,
                                  const LogicalType &target_type, bool try_cast) {
	if (source_type == target_type) {
		return child_str;
	}
	auto source_id = source_type.id();
	if (source_id == LogicalTypeId::SQLNULL) {
		return "NULL";
	}
	if (!SQLiteFunctions::HasRepresentation(target_type)) {
		throw SQLitePushdownNotSupported("unsupported cast to " + target_type.ToString());
	}
	switch (target_type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		if (source_id == LogicalTypeId::BOOLEAN || SQLiteIsIntegerSubtype(source_type, target_type)) {
			return child_str;
		}
		break;
	case LogicalTypeId::BOOLEAN:
		if (SQLiteIsIntegerType(source_type)) {
			return "(" + child_str + " <> 0)";
		}
		break;
	case LogicalTypeId::DOUBLE:
		if (SQLiteIsIntegerType(source_type) || source_id == LogicalTypeId::BOOLEAN) {
			return "CAST(" + child_str + " AS DOUBLE)";
		}
		if (source_id == LogicalTypeId::FLOAT || source_id == LogicalTypeId::DECIMAL) {
			// both are represented as REAL
			return child_str;
		}
		break;
	case LogicalTypeId::VARCHAR:
		if (SQLiteIsIntegerType(source_type)) {
			return "CAST(" + child_str + " AS TEXT)";
		}
		if (source_id == LogicalTypeId::BOOLEAN) {
			return "(CASE " + child_str + " WHEN 1 THEN 'true' WHEN 0 THEN 'false' END)";
		}
		if (source_id == LogicalTypeId::DATE || source_id == LogicalTypeId::TIMESTAMP) {
			// dates and timestamps are represented as their text
			return child_str;
		}
		break;
	case LogicalTypeId::TIMESTAMP:
		if (source_id == LogicalTypeId::DATE) {
			return "(" + child_str + " || ' 00:00:00')";
		}
		break;
	case LogicalTypeId::DATE:
		if (source_id == LogicalTypeId::TIMESTAMP) {
			return "substr(" + child_str + ", 1, 10)";
		}
		break;
	default:
		break;
	}
	if (!SQLiteFunctions::HasRepresentation(source_type) && source_id != LogicalTypeId::FLOAT &&
	    source_id != LogicalTypeId::HUGEINT) {
		throw SQLitePushdownNotSupported("unsupported cast from " + source_type.ToString());
	}
	if (source_id == LogicalTypeId::FLOAT || source_id == LogicalTypeId::DECIMAL) {
		throw SQLitePushdownNotSupported("unsupported cast from " + source_type.ToString());
	}
	RequireExtensionFunctions("a cast from " + source_type.ToString() + " to " + target_type.ToString());
	return string(try_cast ? "duckdb_try_cast(" : "duckdb_cast(") + child_str + ", '" + SQLiteTypeName(source_type) +
	       "', '" + SQLiteTypeName(target_type) + "')";
}

string SQLiteSQLWriter::WriteAs(const ParsedExpression &expr, const LogicalType &target_type) {
	Value value;
	if (SQLiteCapabilities::TryGetConstantValue(expr, value)) {
		if (value.IsNull()) {
			return "NULL";
		}
		auto cast_value = value.TryCastAs(context, target_type);
		if (!cast_value) {
			throw SQLitePushdownNotSupported("constant cannot be converted to " + target_type.ToString());
		}
		return WriteConstant(*cast_value);
	}
	auto source_type = RequireType(expr);
	auto expr_str = WriteExpression(expr);
	if (source_type == target_type) {
		return expr_str;
	}
	auto source_repr = SQLiteGetRepresentation(source_type);
	auto target_repr = SQLiteGetRepresentation(target_type);
	if (source_repr == SQLiteRepresentation::INTEGER && target_repr == SQLiteRepresentation::INTEGER &&
	    source_type.id() != LogicalTypeId::BOOLEAN && SQLiteIsIntegerSubtype(source_type, target_type)) {
		return expr_str;
	}
	if (source_repr == SQLiteRepresentation::REAL && target_repr == SQLiteRepresentation::REAL) {
		return expr_str;
	}
	return WriteCast(expr_str, source_type, target_type);
}

//! The type DuckDB converts the operands of e.g. COALESCE to
LogicalType SQLiteSQLWriter::CommonType(const vector<reference<const ParsedExpression>> &expressions) {
	LogicalType result = LogicalType::SQLNULL;
	for (auto &expr : expressions) {
		auto type = RequireType(expr.get());
		if (expr.get().GetExpressionClass() == ExpressionClass::CONSTANT && type.id() == LogicalTypeId::VARCHAR) {
			// a string literal is converted to the type of the other expressions
			type = LogicalType(LogicalTypeId::STRING_LITERAL);
		}
		if (!LogicalType::TryGetMaxLogicalType(context, result, type, result)) {
			throw SQLitePushdownNotSupported("no common type for " + expr.get().ToString());
		}
	}
	if (result.id() == LogicalTypeId::STRING_LITERAL) {
		return LogicalType::VARCHAR;
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Expressions
//===--------------------------------------------------------------------===//
string SQLiteSQLWriter::WriteExpressionList(const vector<unique_ptr<ParsedExpression>> &expressions,
                                            const string &separator) {
	string result;
	for (auto &expr : expressions) {
		if (!result.empty()) {
			result += separator;
		}
		result += WriteExpression(*expr);
	}
	return result;
}

string SQLiteSQLWriter::WriteColumnRef(const ColumnRefExpression &column_ref) {
	auto &names = column_ref.ColumnNames();
	// tables have a single schema - a reference qualified with it is written qualified with the table only
	idx_t start = names.size() > 2 ? names.size() - 2 : 0;
	if (unqualified_columns) {
		start = names.size() - 1;
	}
	string result;
	for (idx_t i = start; i < names.size(); i++) {
		if (i > start) {
			result += ".";
		}
		result += WriteIdentifier(names[i]);
	}
	if (dml_target) {
		// the target of a DML statement cannot be read through a subquery - convert its columns where they are used
		auto column = ResolveTargetColumn(column_ref);
		if (column) {
			auto read = SQLiteColumnRead(*dml_target, *column, result);
			if (!read.empty()) {
				if (column->Type().id() == LogicalTypeId::DATE || column->Type().id() == LogicalTypeId::TIMESTAMP) {
					RequireExtensionFunctions("reading a " + column->Type().ToString() + " column");
				}
				return read;
			}
		}
	}
	return result;
}

//! The names a FROM clause item is bound to
static void SQLiteGetBindings(const TableRef &ref, vector<Identifier> &bindings) {
	if (ref.type == TableReferenceType::JOIN) {
		auto &join = ref.Cast<JoinRef>();
		SQLiteGetBindings(*join.left, bindings);
		if (join.type != JoinType::SEMI && join.type != JoinType::ANTI) {
			SQLiteGetBindings(*join.right, bindings);
		}
		return;
	}
	if (!ref.alias.empty()) {
		bindings.push_back(ref.alias);
	} else if (ref.type == TableReferenceType::BASE_TABLE) {
		bindings.push_back(ref.Cast<BaseTableRef>().Table());
	} else if (ref.type == TableReferenceType::EXPRESSION_LIST) {
		bindings.push_back(Identifier("valueslist"));
	}
}

bool SQLiteSQLWriter::ProvidesColumn(const TableRef &ref, const Identifier &name) {
	auto has_name = [&](const vector<string> &names) {
		for (idx_t i = 0; i < names.size(); i++) {
			auto &column_name =
			    i < ref.column_name_alias.size() ? ref.column_name_alias[i].GetIdentifierName() : names[i];
			if (StringUtil::CIEquals(column_name, name.GetIdentifierName())) {
				return true;
			}
		}
		return false;
	};
	switch (ref.type) {
	case TableReferenceType::BASE_TABLE: {
		auto &base = ref.Cast<BaseTableRef>();
		vector<string> names;
		if (IsCTEReference(base)) {
			auto probe = make_uniq<SelectNode>();
			probe->select_list.push_back(make_uniq<StarExpression>());
			auto cte_ref = make_uniq<BaseTableRef>();
			cte_ref->SetQualifiedName(base.GetQualifiedName());
			probe->from_table = std::move(cte_ref);
			InferQueryTypes(*probe, &names);
		} else {
			auto table = LookupTable(base);
			if (table) {
				for (auto &column : table->GetColumns().Logical()) {
					names.push_back(column.Name().GetIdentifierName());
				}
			}
		}
		if (names.empty()) {
			throw SQLitePushdownNotSupported("cannot determine the columns of " + base.Table().GetIdentifierName());
		}
		return has_name(names);
	}
	case TableReferenceType::JOIN: {
		auto &join = ref.Cast<JoinRef>();
		return ProvidesColumn(*join.left, name) ||
		       (join.type != JoinType::SEMI && join.type != JoinType::ANTI && ProvidesColumn(*join.right, name));
	}
	case TableReferenceType::SUBQUERY: {
		vector<string> names;
		InferQueryTypes(*ref.Cast<SubqueryRef>().subquery->node, &names);
		if (names.empty()) {
			throw SQLitePushdownNotSupported("cannot determine the columns of a subquery");
		}
		return has_name(names);
	}
	case TableReferenceType::EXPRESSION_LIST: {
		auto &expression_list = ref.Cast<ExpressionListRef>();
		vector<string> names;
		for (idx_t col = 0; col < expression_list.values[0].size(); col++) {
			names.push_back(col < expression_list.expected_names.size()
			                    ? expression_list.expected_names[col].GetIdentifierName()
			                    : "col" + to_string(col));
		}
		return has_name(names);
	}
	case TableReferenceType::EMPTY_FROM:
		return false;
	default:
		throw SQLitePushdownNotSupported("cannot determine the columns of a table reference");
	}
}

optional_ptr<const ColumnDefinition> SQLiteSQLWriter::ResolveTargetColumn(const ColumnRefExpression &column_ref) {
	auto &names = column_ref.ColumnNames();
	auto &column_name = names.back();
	if (!dml_target->ColumnExists(column_name)) {
		return nullptr;
	}
	optional_ptr<const Identifier> binding;
	if (names.size() >= 2) {
		binding = names[names.size() - 2];
	}
	// tables of queries nested in the statement shadow the target
	for (idx_t i = table_scopes.size(); i > dml_scope; i--) {
		for (auto &ref : table_scopes[i - 1]) {
			if (!ref.get()) {
				continue;
			}
			if (binding) {
				vector<Identifier> bindings;
				SQLiteGetBindings(*ref.get(), bindings);
				for (auto &name : bindings) {
					if (name == *binding) {
						return nullptr;
					}
				}
			} else if (ProvidesColumn(*ref.get(), column_name)) {
				return nullptr;
			}
		}
	}
	if (binding) {
		for (auto &name : dml_bindings) {
			if (name == *binding) {
				return &dml_target->GetColumn(column_name);
			}
		}
		return nullptr;
	}
	// the other tables of the statement (e.g. the FROM clause of an UPDATE) - DuckDB rejects ambiguous references
	if (dml_scope > 0) {
		for (auto &ref : table_scopes[dml_scope - 1]) {
			if (!ref.get()) {
				continue;
			}
			vector<Identifier> bindings;
			SQLiteGetBindings(*ref.get(), bindings);
			bool is_target = false;
			for (auto &name : bindings) {
				for (auto &target_name : dml_bindings) {
					is_target = is_target || (name == target_name && ref.get()->type == TableReferenceType::BASE_TABLE);
				}
			}
			if (!is_target && ProvidesColumn(*ref.get(), column_name)) {
				return nullptr;
			}
		}
	}
	return &dml_target->GetColumn(column_name);
}

static string SQLiteComparisonOperator(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return "=";
	case ExpressionType::COMPARE_NOTEQUAL:
		return "<>";
	case ExpressionType::COMPARE_LESSTHAN:
		return "<";
	case ExpressionType::COMPARE_GREATERTHAN:
		return ">";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return "<=";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ">=";
	case ExpressionType::COMPARE_DISTINCT_FROM:
		// IS [NOT] DISTINCT FROM requires SQLite 3.39
		return "IS NOT";
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return "IS";
	default:
		throw InternalException("SQLiteSQLWriter: unsupported comparison type - should have been blocked by "
		                        "SupportsPushdown");
	}
}

vector<string> SQLiteSQLWriter::WriteComparisonOperands(const vector<reference<const ParsedExpression>> &operands) {
	vector<Value> values(operands.size());
	vector<bool> is_constant(operands.size());
	vector<LogicalType> types;
	for (idx_t i = 0; i < operands.size(); i++) {
		is_constant[i] = SQLiteCapabilities::TryGetConstantValue(operands[i].get(), values[i]);
		types.push_back(is_constant[i] ? values[i].type() : RequireType(operands[i].get()));
	}
	// the type DuckDB compares the operands as
	LogicalType target = types[0];
	for (idx_t i = 1; i < types.size(); i++) {
		if (!BoundComparisonExpression::TryBindComparison(context, target, types[i], target,
		                                                  ExpressionType::COMPARE_EQUAL)) {
			throw SQLitePushdownNotSupported("operands cannot be compared");
		}
	}
	auto target_repr = SQLiteGetRepresentation(target);
	if (target_repr == SQLiteRepresentation::OTHER) {
		throw SQLitePushdownNotSupported("unsupported comparison of " + target.ToString());
	}
	vector<string> result;
	for (idx_t i = 0; i < operands.size(); i++) {
		if (is_constant[i]) {
			if (values[i].IsNull()) {
				result.push_back("NULL");
				continue;
			}
			auto cast_value = values[i].TryCastAs(context, target);
			if (!cast_value || !SQLiteCapabilities::SupportsValue(*cast_value)) {
				// e.g. DuckDB raises an error
				throw SQLitePushdownNotSupported("constant cannot be converted to the compared type");
			}
			result.push_back(WriteConstant(*cast_value));
			continue;
		}
		auto expr_str = WriteExpression(operands[i].get());
		auto repr = SQLiteGetRepresentation(types[i]);
		bool numeric = (repr == SQLiteRepresentation::INTEGER || repr == SQLiteRepresentation::REAL) &&
		               (target_repr == SQLiteRepresentation::INTEGER || target_repr == SQLiteRepresentation::REAL) &&
		               types[i].id() != LogicalTypeId::BOOLEAN && target.id() != LogicalTypeId::BOOLEAN;
		if (types[i] == target || numeric) {
			// SQLite compares integers and reals by their numeric value
			result.push_back(expr_str);
		} else {
			result.push_back(WriteCast(expr_str, types[i], target));
		}
	}
	return result;
}

string SQLiteSQLWriter::WriteOperator(const OperatorExpression &op) {
	auto &children = op.GetChildren();
	switch (op.GetExpressionType()) {
	case ExpressionType::OPERATOR_IS_NULL:
		return "(" + WriteExpression(*children[0]) + " IS NULL)";
	case ExpressionType::OPERATOR_IS_NOT_NULL:
		return "(" + WriteExpression(*children[0]) + " IS NOT NULL)";
	case ExpressionType::OPERATOR_NOT:
		return "(NOT " + WriteCondition(*children[0]) + ")";
	case ExpressionType::OPERATOR_COALESCE: {
		vector<reference<const ParsedExpression>> args;
		for (auto &child : children) {
			args.push_back(*child);
		}
		auto type = CommonType(args);
		string result = "COALESCE(";
		for (idx_t i = 0; i < children.size(); i++) {
			result += (i > 0 ? ", " : "") + WriteAs(*children[i], type);
		}
		if (children.size() == 1) {
			// SQLite requires at least two arguments
			result += ", NULL";
		}
		return result + ")";
	}
	case ExpressionType::OPERATOR_NULLIF: {
		vector<reference<const ParsedExpression>> operands {*children[0], *children[1]};
		auto written = WriteComparisonOperands(operands);
		auto type = RequireType(*children[0]);
		// the result is the first operand - but compared as the common type
		auto first = WriteAs(*children[0], type);
		return "(CASE WHEN " + written[0] + " = " + written[1] + " THEN NULL ELSE " + first + " END)";
	}
	case ExpressionType::COMPARE_IN:
	case ExpressionType::COMPARE_NOT_IN: {
		vector<reference<const ParsedExpression>> operands;
		for (auto &child : children) {
			operands.push_back(*child);
		}
		auto written = WriteComparisonOperands(operands);
		string result = "(" + written[0];
		result += op.GetExpressionType() == ExpressionType::COMPARE_IN ? " IN (" : " NOT IN (";
		for (idx_t i = 1; i < written.size(); i++) {
			result += (i > 1 ? ", " : "") + written[i];
		}
		return result + "))";
	}
	default:
		throw InternalException("SQLiteSQLWriter: unsupported operator type - should have been blocked by "
		                        "SupportsPushdown");
	}
}

string SQLiteSQLWriter::WriteCase(const CaseExpression &case_expr) {
	vector<reference<const ParsedExpression>> results;
	for (auto &check : case_expr.CaseChecks()) {
		results.push_back(*check.then_expr);
	}
	results.push_back(case_expr.Else());
	auto type = CommonType(results);
	string result = "(CASE";
	for (auto &check : case_expr.CaseChecks()) {
		result += " WHEN " + WriteCondition(*check.when_expr);
		result += " THEN " + WriteAs(*check.then_expr, type);
	}
	result += " ELSE " + WriteAs(case_expr.Else(), type);
	return result + " END)";
}

//! Whether an expression is a BOOLEAN whatever the types of its operands
static bool SQLiteIsBooleanExpression(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COMPARISON:
	case ExpressionClass::CONJUNCTION:
	case ExpressionClass::BETWEEN:
		return true;
	case ExpressionClass::OPERATOR:
		switch (expr.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
		case ExpressionType::OPERATOR_IS_NOT_NULL:
		case ExpressionType::OPERATOR_NOT:
		case ExpressionType::COMPARE_IN:
		case ExpressionType::COMPARE_NOT_IN:
			return true;
		default:
			return false;
		}
	case ExpressionClass::SUBQUERY:
		return expr.Cast<SubqueryExpression>().GetSubqueryType() != SubqueryType::SCALAR;
	default:
		return false;
	}
}

string SQLiteSQLWriter::WriteCondition(const ParsedExpression &expr) {
	if (SQLiteIsBooleanExpression(expr)) {
		return WriteExpression(expr);
	}
	auto type = RequireType(expr);
	if (type.id() == LogicalTypeId::BOOLEAN || type.id() == LogicalTypeId::SQLNULL || SQLiteIsIntegerType(type)) {
		// integers are true when they are not zero, in SQLite as well
		return WriteExpression(expr);
	}
	// e.g. the string 'true' is false in SQLite, and DuckDB raises an error for a DATE
	return WriteCast(expr, LogicalType::BOOLEAN, false);
}

//! The conjuncts of an expression (the expression itself if it is not an AND)
static vector<reference<const ParsedExpression>> SQLiteGetConjuncts(const ParsedExpression &expr) {
	vector<reference<const ParsedExpression>> result;
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &child : expr.Cast<ConjunctionExpression>().GetChildren()) {
			result.push_back(*child);
		}
	} else {
		result.push_back(expr);
	}
	return result;
}

static bool SQLiteContainsExpression(const vector<reference<const ParsedExpression>> &expressions,
                                     const ParsedExpression &expr) {
	for (auto &entry : expressions) {
		if (entry.get().Equals(expr)) {
			return true;
		}
	}
	return false;
}

string SQLiteSQLWriter::WriteDisjunction(const ConjunctionExpression &disjunction) {
	// conjuncts that every disjunct has are factored out - (a AND b) OR (a AND c) is a AND (b OR c), also in
	// three-valued logic - so that SQLite can use them (e.g. as a join condition)
	vector<vector<reference<const ParsedExpression>>> disjuncts;
	for (auto &child : disjunction.GetChildren()) {
		disjuncts.push_back(SQLiteGetConjuncts(*child));
	}
	vector<reference<const ParsedExpression>> common;
	for (auto &conjunct : disjuncts[0]) {
		bool in_all = !SQLiteContainsExpression(common, conjunct.get());
		for (idx_t i = 1; i < disjuncts.size() && in_all; i++) {
			in_all = SQLiteContainsExpression(disjuncts[i], conjunct.get());
		}
		if (in_all) {
			common.push_back(conjunct);
		}
	}
	if (common.empty()) {
		string result = "(";
		for (idx_t i = 0; i < disjunction.GetChildren().size(); i++) {
			result += (i > 0 ? " OR " : "") + WriteCondition(*disjunction.GetChildren()[i]);
		}
		return result + ")";
	}
	string result = "(";
	for (idx_t i = 0; i < common.size(); i++) {
		result += (i > 0 ? " AND " : "") + WriteCondition(common[i].get());
	}
	string remaining;
	for (auto &disjunct : disjuncts) {
		string conjuncts;
		for (auto &conjunct : disjunct) {
			if (!SQLiteContainsExpression(common, conjunct.get())) {
				conjuncts += (conjuncts.empty() ? "" : " AND ") + WriteCondition(conjunct.get());
			}
		}
		if (conjuncts.empty()) {
			// this disjunct is true when the common conjuncts are
			return result + ")";
		}
		remaining += (remaining.empty() ? "(" : " OR (") + conjuncts + ")";
	}
	return result + " AND (" + remaining + "))";
}

//! Whether an expression contains an aggregate or a window function (outside of subqueries)
static bool SQLiteContainsAggregateOrWindow(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION &&
	    SQLiteCapabilities::IsAggregateFunction(expr.Cast<FunctionExpression>().FunctionName().GetIdentifierName())) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return false;
	}
	bool result = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { result = result || SQLiteContainsAggregateOrWindow(child); });
	return result;
}

//! Whether a query returns at most one row - an aggregate without GROUP BY, or with LIMIT 0 / 1
static bool SQLiteReturnsSingleRow(const QueryNode &node) {
	for (auto &modifier : node.modifiers) {
		if (modifier->type == ResultModifierType::LIMIT_MODIFIER) {
			auto &limit = modifier->Cast<LimitModifier>();
			int64_t value;
			if (limit.limit && SQLiteCapabilities::TryGetIntegerLiteral(*limit.limit, value) && value <= 1) {
				return true;
			}
		}
	}
	if (node.type != QueryNodeType::SELECT_NODE) {
		return false;
	}
	auto &select = node.Cast<SelectNode>();
	if (!select.groups.grouping_sets.empty() || select.select_list.empty()) {
		return false;
	}
	// every select-list entry must be an aggregate (or a constant)
	std::function<bool(const ParsedExpression &)> has_aggregate = [&](const ParsedExpression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::FUNCTION &&
		    SQLiteCapabilities::IsAggregateFunction(
		        expr.Cast<FunctionExpression>().FunctionName().GetIdentifierName())) {
			return true;
		}
		if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY ||
		    expr.GetExpressionClass() == ExpressionClass::WINDOW) {
			return false;
		}
		bool result = false;
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](const ParsedExpression &child) { result = result || has_aggregate(child); });
		return result;
	};
	for (auto &expr : select.select_list) {
		if (!has_aggregate(*expr)) {
			return false;
		}
	}
	return true;
}

string SQLiteSQLWriter::GetResultColumnName(const QueryNode &node) {
	switch (node.type) {
	case QueryNodeType::SELECT_NODE: {
		auto &select = node.Cast<SelectNode>();
		if (select.select_list.size() != 1) {
			throw SQLitePushdownNotSupported("subquery with more than one column");
		}
		auto &expr = *select.select_list[0];
		if (!expr.GetAlias().empty()) {
			return expr.GetAlias().GetIdentifierName();
		}
		if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			return expr.Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName();
		}
		if (expr.GetExpressionClass() == ExpressionClass::STAR) {
			throw SQLitePushdownNotSupported("subquery with a star");
		}
		return expr.ToString();
	}
	case QueryNodeType::SET_OPERATION_NODE:
		return GetResultColumnName(*node.Cast<SetOperationNode>().children[0]);
	default:
		throw SQLitePushdownNotSupported("unsupported scalar subquery");
	}
}

//! Whether a column of a table can be looked up through an index (or is the rowid)
static bool SQLiteIsIndexedColumn(SQLiteTableEntry &table, const Identifier &column) {
	return table.indexed_columns.count(column.GetIdentifierName()) > 0;
}

void SQLiteSQLWriter::VerifyCorrelatedSubquery(const QueryNode &node) {
	if (!InferQueryTypes(node).empty()) {
		// the subquery can be bound on its own - it is not correlated
		return;
	}
	// SQLite evaluates a correlated subquery for every row - by scanning the table of the subquery, unless the
	// correlated rows can be looked up through an index (DuckDB decorrelates the subquery instead)
	if (node.type != QueryNodeType::SELECT_NODE) {
		throw SQLitePushdownNotSupported("correlated subquery");
	}
	auto &select = node.Cast<SelectNode>();
	if (!select.from_table || !select.where_clause) {
		throw SQLitePushdownNotSupported("correlated subquery");
	}
	vector<reference<const BaseTableRef>> tables;
	bool only_base_tables = true;
	CollectScopeTables(*select.from_table, tables, only_base_tables);
	auto inner_table = [&](const ColumnRefExpression &column_ref) -> optional_ptr<SQLiteTableEntry> {
		auto &names = column_ref.ColumnNames();
		optional_ptr<SQLiteTableEntry> result;
		for (auto &base : tables) {
			auto table = LookupTable(base.get());
			if (!table || !table->ColumnExists(names.back())) {
				continue;
			}
			auto binding = base.get().alias.empty() ? base.get().Table() : base.get().alias;
			if (names.size() >= 2 && names[names.size() - 2] != binding) {
				continue;
			}
			if (result) {
				// ambiguous
				return nullptr;
			}
			result = table;
		}
		return result;
	};
	auto is_outer = [&](const ParsedExpression &expr) {
		if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			return false;
		}
		auto &column_ref = expr.Cast<ColumnRefExpression>();
		if (column_ref.ColumnNames().size() >= 2) {
			auto &binding = column_ref.ColumnNames()[column_ref.ColumnNames().size() - 2];
			for (auto &ref : vector<reference<const unique_ptr<TableRef>>> {select.from_table}) {
				vector<Identifier> bindings;
				SQLiteGetBindings(*ref.get(), bindings);
				for (auto &name : bindings) {
					if (name == binding) {
						return false;
					}
				}
			}
			return true;
		}
		return !ProvidesColumn(*select.from_table, column_ref.GetColumnName());
	};
	for (auto &conjunct : SQLiteGetConjuncts(*select.where_clause)) {
		auto &expr = conjunct.get();
		if (expr.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
			continue;
		}
		auto &comparison = expr.Cast<ComparisonExpression>();
		for (auto pair : {std::make_pair(&comparison.Left(), &comparison.Right()),
		                  std::make_pair(&comparison.Right(), &comparison.Left())}) {
			if (pair.first->GetExpressionClass() != ExpressionClass::COLUMN_REF || !is_outer(*pair.second)) {
				continue;
			}
			auto &column_ref = pair.first->Cast<ColumnRefExpression>();
			auto table = inner_table(column_ref);
			if (table && SQLiteIsIndexedColumn(*table, column_ref.GetColumnName())) {
				return;
			}
		}
	}
	throw SQLitePushdownNotSupported("correlated subquery without an index on the correlated column");
}

string SQLiteSQLWriter::WriteSubquery(const SubqueryExpression &subquery) {
	auto &node = *subquery.Subquery()->node;
	VerifyCorrelatedSubquery(node);
	switch (subquery.GetSubqueryType()) {
	case SubqueryType::EXISTS:
		return "EXISTS (" + WriteQueryNode(node) + ")";
	case SubqueryType::NOT_EXISTS:
		return "(NOT EXISTS (" + WriteQueryNode(node) + "))";
	case SubqueryType::SCALAR: {
		Value error_on_multiple_rows;
		bool check_rows = true;
		if (context.TryGetCurrentSetting("scalar_subquery_error_on_multiple_rows", error_on_multiple_rows) &&
		    !error_on_multiple_rows.IsNull()) {
			check_rows = BooleanValue::Get(error_on_multiple_rows);
		}
		if (!check_rows || SQLiteReturnsSingleRow(node)) {
			return "(" + WriteQueryNode(node) + ")";
		}
		// SQLite returns the first row of a scalar subquery, DuckDB raises an error for more than one row
		RequireExtensionFunctions("a scalar subquery");
		auto name = GetResultColumnName(node);
		return "(SELECT duckdb_scalar_subquery(" + WriteIdentifier(name) + ") FROM (SELECT * FROM (" +
		       WriteQueryNode(node) + ") LIMIT 2))";
	}
	case SubqueryType::ANY: {
		// x = ANY (subquery)
		auto child_type = RequireType(*subquery.GetChild());
		auto subquery_types = InferQueryTypes(node);
		if (subquery_types.size() != 1) {
			throw SQLitePushdownNotSupported("cannot determine the type of an IN subquery");
		}
		LogicalType target;
		if (!BoundComparisonExpression::TryBindComparison(context, child_type, subquery_types[0], target,
		                                                  ExpressionType::COMPARE_EQUAL)) {
			throw SQLitePushdownNotSupported("operands cannot be compared");
		}
		auto child_repr = SQLiteGetRepresentation(child_type);
		auto subquery_repr = SQLiteGetRepresentation(subquery_types[0]);
		bool numeric = (child_repr == SQLiteRepresentation::INTEGER || child_repr == SQLiteRepresentation::REAL) &&
		               (subquery_repr == SQLiteRepresentation::INTEGER || subquery_repr == SQLiteRepresentation::REAL);
		if (child_type != subquery_types[0] && !numeric) {
			throw SQLitePushdownNotSupported("IN subquery with operands of different types");
		}
		return "(" + WriteExpression(*subquery.GetChild()) + " IN (" + WriteQueryNode(node) + "))";
	}
	default:
		throw InternalException("SQLiteSQLWriter: unsupported subquery type - should have been blocked by "
		                        "SupportsPushdown");
	}
}

string SQLiteSQLWriter::WriteExpression(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::CONSTANT:
		return WriteConstant(expr.Cast<ConstantExpression>().GetLiteral().ToValue());
	case ExpressionClass::COLUMN_REF:
		return WriteColumnRef(expr.Cast<ColumnRefExpression>());
	case ExpressionClass::STAR: {
		auto &star = expr.Cast<StarExpression>();
		if (!star.RelationName().empty()) {
			return WriteIdentifier(star.RelationName()) + ".*";
		}
		return "*";
	}
	case ExpressionClass::FUNCTION: {
		interval_t interval;
		if (SQLiteCapabilities::TryGetIntervalLiteral(expr, interval)) {
			throw SQLitePushdownNotSupported("unsupported use of an interval");
		}
		return WriteFunction(expr.Cast<FunctionExpression>());
	}
	case ExpressionClass::WINDOW:
		return WriteWindow(expr.Cast<WindowExpression>());
	case ExpressionClass::COMPARISON: {
		auto &comparison = expr.Cast<ComparisonExpression>();
		vector<reference<const ParsedExpression>> operands {comparison.Left(), comparison.Right()};
		auto written = WriteComparisonOperands(operands);
		return "(" + written[0] + " " + SQLiteComparisonOperator(expr.GetExpressionType()) + " " + written[1] + ")";
	}
	case ExpressionClass::CONJUNCTION: {
		auto &conjunction = expr.Cast<ConjunctionExpression>();
		if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
			return WriteDisjunction(conjunction);
		}
		string result = "(";
		for (idx_t i = 0; i < conjunction.GetChildren().size(); i++) {
			result += (i > 0 ? " AND " : "") + WriteCondition(*conjunction.GetChildren()[i]);
		}
		return result + ")";
	}
	case ExpressionClass::OPERATOR:
		return WriteOperator(expr.Cast<OperatorExpression>());
	case ExpressionClass::CASE:
		return WriteCase(expr.Cast<CaseExpression>());
	case ExpressionClass::CAST: {
		auto &cast_expr = expr.Cast<CastExpression>();
		auto target_type = SQLiteCapabilities::ResolveType(cast_expr.TargetType());
		if (cast_expr.Child().GetExpressionClass() == ExpressionClass::CONSTANT) {
			// casts of literals are evaluated locally and serialized as the resulting value
			Value value;
			if (!SQLiteCapabilities::TryGetConstantValue(expr, value)) {
				throw InternalException("SQLiteSQLWriter: cast of literal failed - should have been blocked by "
				                        "SupportsPushdown");
			}
			return WriteConstant(value);
		}
		return WriteCast(cast_expr.Child(), target_type, cast_expr.IsTryCast());
	}
	case ExpressionClass::BETWEEN: {
		auto &between = expr.Cast<BetweenExpression>();
		vector<reference<const ParsedExpression>> operands {between.Input(), between.LowerBound(),
		                                                    between.UpperBound()};
		auto written = WriteComparisonOperands(operands);
		return "(" + written[0] + " BETWEEN " + written[1] + " AND " + written[2] + ")";
	}
	case ExpressionClass::SUBQUERY:
		return WriteSubquery(expr.Cast<SubqueryExpression>());
	default:
		throw InternalException("SQLiteSQLWriter: unsupported expression class - should have been blocked by "
		                        "SupportsPushdown");
	}
}

//===--------------------------------------------------------------------===//
// Functions
//===--------------------------------------------------------------------===//
//! Whether DuckDB's division semantics are the default ones (a DOUBLE division that yields inf for a zero divisor,
//! an error for a zero divisor of an integer division)
static bool SQLiteHasDefaultDivisionSemantics(ClientContext &context) {
	Value ieee_floating_point_ops;
	if (context.TryGetCurrentSetting("ieee_floating_point_ops", ieee_floating_point_ops) &&
	    !ieee_floating_point_ops.IsNull() && !BooleanValue::Get(ieee_floating_point_ops)) {
		return false;
	}
	Value error_on_division_by_zero;
	if (context.TryGetCurrentSetting("error_on_division_by_zero", error_on_division_by_zero) &&
	    !error_on_division_by_zero.IsNull() && !BooleanValue::Get(error_on_division_by_zero)) {
		return false;
	}
	return true;
}

string SQLiteSQLWriter::WriteDivision(const ParsedExpression &left, const ParsedExpression &right,
                                      bool integer_division) {
	if (!SQLiteHasDefaultDivisionSemantics(context)) {
		throw SQLitePushdownNotSupported("non-default division semantics");
	}
	auto x = WriteExpression(left);
	auto y = WriteExpression(right);
	if (!integer_division) {
		x = "CAST(" + x + " AS DOUBLE)";
	}
	auto op = integer_division ? " / " : " / ";
	Value value;
	if (SQLiteCapabilities::TryGetConstantValue(right, value)) {
		auto double_value = value.DefaultTryCastAs(LogicalType::DOUBLE);
		if (double_value && !double_value->IsNull() && DoubleValue::Get(*double_value) != 0) {
			// division by a non-zero constant
			return "(" + x + op + y + ")";
		}
	}
	RequireExtensionFunctions("a division");
	if (integer_division) {
		// SQLite yields NULL, DuckDB raises an error
		return "(CASE WHEN " + y + " = 0 THEN duckdb_error('Division by zero') ELSE " + x + op + y + " END)";
	}
	// SQLite yields NULL, DuckDB yields inf / -inf (depending on the sign of the zero) - or NaN, which SQLite cannot
	// represent
	return "(CASE WHEN " + y + " = 0 THEN duckdb_divide(" + x + ", " + y + ") ELSE " + x + op + y + " END)";
}

string SQLiteSQLWriter::WriteArithmetic(const FunctionExpression &func, const string &name,
                                        const vector<FunctionArgument> &children) {
	auto &left_expr = children[0].GetExpression();
	if (children.size() == 1) {
		auto type = RequireType(left_expr);
		if (name == "-" && SQLiteIsNumericType(type)) {
			if (!SQLiteIsIntegerType(type)) {
				// SQLite's negation of 0.0 is 0.0, not -0.0
				return "(" + WriteExpression(left_expr) + " * -1.0)";
			}
			return "(-" + WriteExpression(left_expr) + ")";
		}
		if (name == "+" && SQLiteIsNumericType(type)) {
			return WriteExpression(left_expr);
		}
		if (name == "~" && SQLiteIsIntegerType(type)) {
			return "(~" + WriteExpression(left_expr) + ")";
		}
		throw SQLitePushdownNotSupported("unsupported unary operator " + name);
	}
	auto &right_expr = children[1].GetExpression();
	auto left_type = RequireType(left_expr);
	auto right_type = RequireType(right_expr);
	auto binary = [&](const string &op) {
		return "(" + WriteExpression(left_expr) + " " + op + " " + WriteExpression(right_expr) + ")";
	};
	if (name == "/") {
		if (!SQLiteIsNumericOrDecimalType(left_type) || !SQLiteIsNumericOrDecimalType(right_type)) {
			throw SQLitePushdownNotSupported("unsupported division");
		}
		return WriteDivision(left_expr, right_expr, false);
	}
	if (name == "//" || name == "%") {
		if (!SQLiteIsIntegerType(left_type) || !SQLiteIsIntegerType(right_type)) {
			// DuckDB computes these for floating point numbers as well
			throw SQLitePushdownNotSupported("unsupported integer division / modulo");
		}
		if (name == "//") {
			return WriteDivision(left_expr, right_expr, true);
		}
		// the minimum of a signed type modulo -1 overflows in DuckDB
		LogicalType common_type;
		if (!LogicalType::TryGetMaxLogicalType(context, left_type, right_type, common_type)) {
			throw SQLitePushdownNotSupported("unsupported modulo");
		}
		string minimum;
		switch (common_type.id()) {
		case LogicalTypeId::TINYINT:
			minimum = to_string(NumericLimits<int8_t>::Minimum());
			break;
		case LogicalTypeId::SMALLINT:
			minimum = to_string(NumericLimits<int16_t>::Minimum());
			break;
		case LogicalTypeId::INTEGER:
			minimum = to_string(NumericLimits<int32_t>::Minimum());
			break;
		case LogicalTypeId::BIGINT:
			minimum = to_string(NumericLimits<int64_t>::Minimum());
			break;
		default:
			break;
		}
		Value value;
		if (SQLiteCapabilities::TryGetConstantValue(right_expr, value) && !value.IsNull() &&
		    value.GetValue<int64_t>() != 0 && (value.GetValue<int64_t>() != -1 || minimum.empty())) {
			return binary("%");
		}
		RequireExtensionFunctions("a modulo");
		auto x = WriteExpression(left_expr);
		auto y = WriteExpression(right_expr);
		string result = "(CASE WHEN " + y + " = 0 THEN duckdb_error('Division by zero')";
		if (!minimum.empty()) {
			result +=
			    " WHEN " + y + " = -1 AND " + x + " = (" + minimum + ") THEN duckdb_error('Overflow in division')";
		}
		return result + " ELSE " + x + " % " + y + " END)";
	}
	if (name == "||") {
		// DuckDB converts non-string operands to strings - and concatenates blobs and lists
		if (left_type.id() == LogicalTypeId::BLOB || right_type.id() == LogicalTypeId::BLOB) {
			throw SQLitePushdownNotSupported("unsupported concatenation");
		}
		return "(" + WriteAs(left_expr, LogicalType::VARCHAR) + " || " + WriteAs(right_expr, LogicalType::VARCHAR) +
		       ")";
	}
	if (name == "&" || name == "|" || name == "xor") {
		if (!SQLiteIsIntegerType(left_type) || !SQLiteIsIntegerType(right_type)) {
			throw SQLitePushdownNotSupported("unsupported bitwise operator");
		}
		if (name == "xor") {
			auto x = WriteExpression(left_expr);
			auto y = WriteExpression(right_expr);
			return "((" + x + " | " + y + ") - (" + x + " & " + y + "))";
		}
		return binary(name);
	}
	if (name == "<<" || name == ">>") {
		LogicalType common_type;
		if (!SQLiteIsIntegerType(left_type) || !SQLiteIsIntegerType(right_type) ||
		    !LogicalType::TryGetMaxLogicalType(context, left_type, right_type, common_type)) {
			throw SQLitePushdownNotSupported("unsupported shift");
		}
		idx_t bits;
		bool is_signed = true;
		switch (common_type.id()) {
		case LogicalTypeId::UTINYINT:
			is_signed = false;
			DUCKDB_EXPLICIT_FALLTHROUGH;
		case LogicalTypeId::TINYINT:
			bits = 8;
			break;
		case LogicalTypeId::USMALLINT:
			is_signed = false;
			DUCKDB_EXPLICIT_FALLTHROUGH;
		case LogicalTypeId::SMALLINT:
			bits = 16;
			break;
		case LogicalTypeId::UINTEGER:
			is_signed = false;
			DUCKDB_EXPLICIT_FALLTHROUGH;
		case LogicalTypeId::INTEGER:
			bits = 32;
			break;
		case LogicalTypeId::BIGINT:
			bits = 64;
			break;
		default:
			throw SQLitePushdownNotSupported("unsupported shift of " + common_type.ToString());
		}
		RequireExtensionFunctions("a shift");
		// DuckDB raises an error for an overflow, SQLite shifts the bits out
		auto shift_bits = name == "<<" ? bits + (is_signed ? 0 : 1) : bits;
		return string(name == "<<" ? "duckdb_shift_left(" : "duckdb_shift_right(") + WriteExpression(left_expr) + ", " +
		       WriteExpression(right_expr) + ", " + to_string(shift_bits) + ")";
	}
	// + - *
	if (SQLiteIsNumericOrDecimalType(left_type) && SQLiteIsNumericOrDecimalType(right_type)) {
		// DECIMAL arithmetic is exact in DuckDB - it is only supported where the result is a DOUBLE
		if (!SQLiteIsNumericType(RequireType(func))) {
			throw SQLitePushdownNotSupported("unsupported arithmetic for " + left_type.ToString() + " and " +
			                                 right_type.ToString());
		}
		return binary(name);
	}
	if (SQLiteIsTimestampType(left_type) && right_type.id() == LogicalTypeId::INTERVAL &&
	    (name == "+" || name == "-")) {
		return WriteIntervalArithmetic(left_expr, right_expr, name == "-");
	}
	if (left_type.id() == LogicalTypeId::INTERVAL && SQLiteIsTimestampType(right_type) && name == "+") {
		return WriteIntervalArithmetic(right_expr, left_expr, false);
	}
	if (left_type.id() == LogicalTypeId::DATE && SQLiteIsIntegerType(right_type) && (name == "+" || name == "-")) {
		// days are added to a date
		auto days = WriteExpression(right_expr);
		if (name == "-") {
			days = "(-" + days + ")";
		}
		return "date(" + WriteExpression(left_expr) + ", " + days + " || ' days')";
	}
	if (SQLiteIsIntegerType(left_type) && right_type.id() == LogicalTypeId::DATE && name == "+") {
		return "date(" + WriteExpression(right_expr) + ", " + WriteExpression(left_expr) + " || ' days')";
	}
	if (left_type.id() == LogicalTypeId::DATE && right_type.id() == LogicalTypeId::DATE && name == "-") {
		// the number of days between the dates
		return "CAST(julianday(" + WriteExpression(left_expr) + ") - julianday(" + WriteExpression(right_expr) +
		       ") AS INTEGER)";
	}
	throw SQLitePushdownNotSupported("unsupported operator " + name + " for " + left_type.ToString() + " and " +
	                                 right_type.ToString());
}

//! Convert a LIKE pattern into a GLOB pattern - returns false if the pattern cannot be converted
static bool SQLiteLikeToGlob(const string &pattern, bool has_escape, char escape, string &result) {
	result.clear();
	for (idx_t i = 0; i < pattern.size(); i++) {
		auto c = pattern[i];
		if (has_escape && c == escape) {
			if (i + 1 == pattern.size()) {
				// DuckDB raises an error
				return false;
			}
			c = pattern[++i];
		} else if (c == '%') {
			result += '*';
			continue;
		} else if (c == '_') {
			result += '?';
			continue;
		}
		// characters that are special in GLOB patterns are matched literally within brackets
		if (c == '*' || c == '?' || c == '[') {
			result += '[';
			result += c;
			result += ']';
		} else {
			result += c;
		}
	}
	return true;
}

string SQLiteSQLWriter::WriteLike(const string &name, const vector<FunctionArgument> &children) {
	bool negated = StringUtil::StartsWith(name, "!") || StringUtil::StartsWith(name, "not_");
	bool case_insensitive = name == "~~*" || name == "!~~*" || name == "ilike_escape" || name == "not_ilike_escape";
	bool glob = name == "~~~" || name == "glob";
	for (auto &child : children) {
		if (!SQLiteIsStringType(RequireType(child.GetExpression()))) {
			throw SQLitePushdownNotSupported("unsupported " + name + " argument");
		}
	}
	auto str = WriteExpression(children[0].GetExpression());
	string result;
	Value pattern;
	Value escape;
	bool has_escape = children.size() > 2;
	bool constant_pattern =
	    SQLiteCapabilities::TryGetConstantValue(children[1].GetExpression(), pattern) &&
	    (!has_escape || SQLiteCapabilities::TryGetConstantValue(children[2].GetExpression(), escape));
	if (constant_pattern && !pattern.IsNull() && (!has_escape || !escape.IsNull()) && !case_insensitive) {
		auto &pattern_str = StringValue::Get(pattern);
		string escape_str = has_escape ? StringValue::Get(escape) : string();
		string glob_pattern;
		if (glob) {
			// DuckDB's GLOB matches like SQLite's, except for character classes and escapes - and a ? matches a byte
			// in DuckDB, a character in SQLite
			if (pattern_str.find_first_of("[]\\?") == string::npos) {
				glob_pattern = pattern_str;
				result = "(" + str + " GLOB " + WriteConstant(Value(glob_pattern)) + ")";
			}
		} else if (escape_str.size() <= 1 &&
		           SQLiteLikeToGlob(pattern_str, !escape_str.empty(), escape_str.empty() ? '\0' : escape_str[0],
		                            glob_pattern)) {
			// SQLite's LIKE is case insensitive - GLOB is not
			result = "(" + str + " GLOB " + WriteConstant(Value(glob_pattern)) + ")";
		}
	}
	if (result.empty()) {
		RequireExtensionFunctions("LIKE with a non-constant pattern");
		string function = glob ? "duckdb_glob(" : case_insensitive ? "duckdb_ilike(" : "duckdb_like(";
		result = function + str;
		for (idx_t i = 1; i < children.size(); i++) {
			result += ", " + WriteExpression(children[i].GetExpression());
		}
		result += ")";
	}
	return negated ? "(NOT " + result + ")" : result;
}

//! Escape a string for a GLOB pattern
static string SQLiteGlobEscape(const string &str) {
	string result;
	for (auto c : str) {
		if (c == '*' || c == '?' || c == '[') {
			result += '[';
			result += c;
			result += ']';
		} else {
			result += c;
		}
	}
	return result;
}

bool SQLiteSQLWriter::TryWriteScalarFunction(const FunctionExpression &func, const string &name,
                                             const vector<FunctionArgument> &children, string &result) {
	auto arg = [&](idx_t i) -> const ParsedExpression & {
		return children[i].GetExpression();
	};
	auto require_types = [&](const std::function<bool(const LogicalType &)> &check) {
		for (auto &child : children) {
			if (!check(RequireType(child.GetExpression()))) {
				throw SQLitePushdownNotSupported("unsupported argument type for " + name);
			}
		}
	};
	auto is_string = [](const LogicalType &type) {
		return SQLiteIsStringType(type);
	};
	auto is_integer = [](const LogicalType &type) {
		return SQLiteIsIntegerType(type) || type.id() == LogicalTypeId::SQLNULL;
	};
	auto constant_string = [&](idx_t i, string &value) {
		Value constant;
		if (!SQLiteCapabilities::TryGetConstantValue(arg(i), constant) || constant.IsNull() ||
		    constant.type().id() != LogicalTypeId::VARCHAR) {
			return false;
		}
		value = StringValue::Get(constant);
		return true;
	};
	static const case_insensitive_set_t OPERATORS = {"+", "-", "*",   "/",  "//", "%", "||",
	                                                 "&", "|", "xor", "<<", ">>", "~"};
	if (OPERATORS.count(name) > 0) {
		result = WriteArithmetic(func, name, children);
		return true;
	}
	static const case_insensitive_set_t LIKE_FUNCTIONS = {"~~",           "!~~",
	                                                      "~~*",          "!~~*",
	                                                      "~~~",          "glob",
	                                                      "like_escape",  "not_like_escape",
	                                                      "ilike_escape", "not_ilike_escape"};
	if (LIKE_FUNCTIONS.count(name) > 0) {
		result = WriteLike(name, children);
		return true;
	}
	if (name == "length" || name == "len" || name == "char_length" || name == "character_length") {
		require_types(is_string);
		result = "length(" + WriteExpression(arg(0)) + ")";
		return true;
	}
	if (name == "strlen" || name == "octet_length") {
		auto type = RequireType(arg(0));
		if (!is_string(type) && type.id() != LogicalTypeId::BLOB) {
			throw SQLitePushdownNotSupported("unsupported argument type for " + name);
		}
		result = "length(CAST(" + WriteExpression(arg(0)) + " AS BLOB))";
		return true;
	}
	if (name == "lower" || name == "lcase" || name == "upper" || name == "ucase") {
		require_types(is_string);
		// SQLite only maps ASCII characters
		RequireExtensionFunctions(name);
		bool upper = name == "upper" || name == "ucase";
		result = string(upper ? "duckdb_upper(" : "duckdb_lower(") + WriteExpression(arg(0)) + ")";
		return true;
	}
	if (name == "trim" || name == "ltrim" || name == "rtrim" || name == "replace") {
		require_types(is_string);
		result = name + "(";
		for (idx_t i = 0; i < children.size(); i++) {
			result += (i > 0 ? ", " : "") + WriteExpression(arg(i));
		}
		result += ")";
		return true;
	}
	if (name == "instr" || name == "strpos" || name == "position") {
		require_types(is_string);
		result = "instr(" + WriteExpression(arg(0)) + ", " + WriteExpression(arg(1)) + ")";
		return true;
	}
	if (name == "contains") {
		// DuckDB's contains also checks list / map membership
		require_types(is_string);
		result = "(instr(" + WriteExpression(arg(0)) + ", " + WriteExpression(arg(1)) + ") > 0)";
		return true;
	}
	if (name == "starts_with" || name == "prefix" || name == "^@" || name == "ends_with" || name == "suffix") {
		require_types(is_string);
		bool prefix = name == "starts_with" || name == "prefix" || name == "^@";
		auto str = WriteExpression(arg(0));
		string constant;
		if (constant_string(1, constant)) {
			auto pattern = SQLiteGlobEscape(constant);
			pattern = prefix ? pattern + "*" : "*" + pattern;
			result = "(" + str + " GLOB " + WriteConstant(Value(pattern)) + ")";
			return true;
		}
		auto affix = WriteExpression(arg(1));
		if (prefix) {
			result = "(substr(" + str + ", 1, length(" + affix + ")) = " + affix + ")";
		} else {
			result = "(CASE WHEN length(" + affix + ") = 0 THEN " + str + " = " + str + " ELSE substr(" + str +
			         ", -length(" + affix + ")) = " + affix + " END)";
		}
		return true;
	}
	if (name == "substring" || name == "substr") {
		if (children.size() < 2 || children.size() > 3 || !is_string(RequireType(arg(0)))) {
			throw SQLitePushdownNotSupported("unsupported substring");
		}
		for (idx_t i = 1; i < children.size(); i++) {
			if (!is_integer(RequireType(arg(i)))) {
				throw SQLitePushdownNotSupported("unsupported substring arguments");
			}
		}
		int64_t start, length = 0;
		if (SQLiteCapabilities::TryGetIntegerLiteral(arg(1), start) && start >= 1 &&
		    (children.size() == 2 || (SQLiteCapabilities::TryGetIntegerLiteral(arg(2), length) && length >= 0))) {
			// SQLite's substr matches DuckDB's substring for positive offsets and non-negative lengths
			result = "substr(" + WriteExpression(arg(0)) + ", " + to_string(start);
			if (children.size() == 3) {
				result += ", " + to_string(length);
			}
			result += ")";
			return true;
		}
		// DuckDB's negative offsets and lengths differ from SQLite's
		RequireExtensionFunctions(name);
		result = "duckdb_substring(";
		for (idx_t i = 0; i < children.size(); i++) {
			result += (i > 0 ? ", " : "") + WriteExpression(arg(i));
		}
		result += ")";
		return true;
	}
	if (name == "left" || name == "right") {
		if (!is_string(RequireType(arg(0))) || !is_integer(RequireType(arg(1)))) {
			throw SQLitePushdownNotSupported("unsupported " + name + " arguments");
		}
		auto str = WriteExpression(arg(0));
		int64_t count;
		if (SQLiteCapabilities::TryGetIntegerLiteral(arg(1), count) && count >= 0) {
			if (count == 0) {
				result = "substr(" + str + ", 1, 0)";
			} else if (name == "left") {
				result = "substr(" + str + ", 1, " + to_string(count) + ")";
			} else {
				result = "substr(" + str + ", " + to_string(-count) + ")";
			}
			return true;
		}
		// negative counts remove characters in DuckDB
		RequireExtensionFunctions(name);
		result = "duckdb_" + name + "(" + str + ", " + WriteExpression(arg(1)) + ")";
		return true;
	}
	if (name == "lpad" || name == "rpad") {
		if (children.size() != 3 || !is_string(RequireType(arg(0))) || !is_integer(RequireType(arg(1))) ||
		    !is_string(RequireType(arg(2)))) {
			throw SQLitePushdownNotSupported("unsupported " + name + " arguments");
		}
		RequireExtensionFunctions(name);
		result = "duckdb_" + name + "(" + WriteExpression(arg(0)) + ", " + WriteExpression(arg(1)) + ", " +
		         WriteExpression(arg(2)) + ")";
		return true;
	}
	if (name == "reverse") {
		require_types(is_string);
		RequireExtensionFunctions(name);
		result = "duckdb_reverse(" + WriteExpression(arg(0)) + ")";
		return true;
	}
	if (name == "repeat") {
		if (!is_string(RequireType(arg(0))) || !is_integer(RequireType(arg(1)))) {
			throw SQLitePushdownNotSupported("unsupported repeat arguments");
		}
		// zeroblob(n) has n bytes - its hex representation n times "00"
		result = "replace(hex(zeroblob(" + WriteExpression(arg(1)) + ")), '00', " + WriteExpression(arg(0)) + ")";
		return true;
	}
	if (name == "concat" || name == "concat_ws") {
		// DuckDB skips NULL arguments
		vector<string> args;
		idx_t first = name == "concat_ws" ? 1 : 0;
		for (idx_t i = first; i < children.size(); i++) {
			if (RequireType(arg(i)).id() == LogicalTypeId::BLOB) {
				throw SQLitePushdownNotSupported("unsupported " + name + " argument");
			}
			args.push_back(WriteAs(arg(i), LogicalType::VARCHAR));
		}
		if (name == "concat") {
			result = "(";
			for (idx_t i = 0; i < args.size(); i++) {
				result += (i > 0 ? " || " : "") + string("COALESCE(") + args[i] + ", '')";
			}
			result += ")";
			return true;
		}
		// prefix every argument with the separator, and remove the first separator
		if (!is_string(RequireType(arg(0)))) {
			throw SQLitePushdownNotSupported("unsupported concat_ws separator");
		}
		auto separator = WriteExpression(arg(0));
		result = "substr(";
		for (idx_t i = 0; i < args.size(); i++) {
			result += (i > 0 ? " || " : "") + string("COALESCE(") + separator + " || " + args[i] + ", '')";
		}
		result += ", length(" + separator + ") + 1)";
		return true;
	}
	if (name == "coalesce" || name == "ifnull") {
		vector<reference<const ParsedExpression>> args;
		for (auto &child : children) {
			args.push_back(child.GetExpression());
		}
		auto type = CommonType(args);
		result = "COALESCE(";
		for (idx_t i = 0; i < children.size(); i++) {
			result += (i > 0 ? ", " : "") + WriteAs(arg(i), type);
		}
		if (children.size() == 1) {
			result += ", NULL";
		}
		result += ")";
		return true;
	}
	if (name == "nullif") {
		vector<reference<const ParsedExpression>> operands {arg(0), arg(1)};
		auto written = WriteComparisonOperands(operands);
		result =
		    "(CASE WHEN " + written[0] + " = " + written[1] + " THEN NULL ELSE " + WriteExpression(arg(0)) + " END)";
		return true;
	}
	if (name == "greatest" || name == "least") {
		// SQLite's multi-argument min / max yield NULL if any argument is NULL, DuckDB skips NULL arguments -
		// every argument is replaced with any non-NULL argument when it is NULL
		vector<reference<const ParsedExpression>> args;
		for (auto &child : children) {
			args.push_back(child.GetExpression());
		}
		auto type = CommonType(args);
		if (SQLiteGetRepresentation(type) == SQLiteRepresentation::OTHER) {
			throw SQLitePushdownNotSupported("unsupported " + name + " argument");
		}
		vector<string> written;
		for (auto &child : children) {
			written.push_back(WriteAs(child.GetExpression(), type));
		}
		if (written.size() == 1) {
			result = written[0];
			return true;
		}
		result = name == "greatest" ? "max(" : "min(";
		for (idx_t i = 0; i < written.size(); i++) {
			result += (i > 0 ? ", " : "") + string("COALESCE(") + written[i];
			for (idx_t j = 0; j < written.size(); j++) {
				if (j != i) {
					result += ", " + written[j];
				}
			}
			result += ")";
		}
		result += ")";
		return true;
	}
	if (name == "abs") {
		auto type = RequireType(arg(0));
		if (!SQLiteIsNumericType(type)) {
			throw SQLitePushdownNotSupported("unsupported abs argument");
		}
		result = "abs(" + WriteExpression(arg(0)) + ")";
		return true;
	}
	if (name == "sign") {
		auto type = RequireType(arg(0));
		if (!SQLiteIsNumericType(type)) {
			throw SQLitePushdownNotSupported("unsupported sign argument");
		}
		auto x = WriteExpression(arg(0));
		result = "(CASE WHEN " + x + " > 0 THEN 1 WHEN " + x + " < 0 THEN -1 WHEN " + x + " = 0 THEN 0 END)";
		return true;
	}
	if (name == "unicode" || name == "ascii") {
		require_types(is_string);
		auto x = WriteExpression(arg(0));
		// DuckDB returns -1 (unicode) / 0 (ascii) for an empty string
		result = "(CASE WHEN " + x + " = '' THEN " + (name == "unicode" ? "-1" : "0") + " ELSE unicode(" + x + ") END)";
		return true;
	}
	if (name == "chr") {
		require_types(is_integer);
		result = "char(" + WriteExpression(arg(0)) + ")";
		return true;
	}
	if (TryWriteMathFunction(func, name, children, result)) {
		return true;
	}
	if (TryWriteDateFunction(name, children, result)) {
		return true;
	}
	if (name == "regexp_matches" || name == "regexp_full_match" || name == "regexp_replace") {
		bool replace = name == "regexp_replace";
		idx_t options_index = replace ? 3 : 2;
		if (children.size() < options_index || children.size() > options_index + 1) {
			throw SQLitePushdownNotSupported("unsupported " + name + " arguments");
		}
		for (idx_t i = 0; i < options_index; i++) {
			if (!is_string(RequireType(arg(i)))) {
				throw SQLitePushdownNotSupported("unsupported argument type for " + name);
			}
		}
		string options;
		if (children.size() > options_index && !constant_string(options_index, options)) {
			throw SQLitePushdownNotSupported("non-constant regular expression options");
		}
		RequireExtensionFunctions(name);
		result = "duckdb_" + name + "(";
		for (idx_t i = 0; i < options_index; i++) {
			result += WriteExpression(arg(i)) + ", ";
		}
		result += WriteConstant(Value(options)) + ")";
		return true;
	}
	if (name == "typeof") {
		// the type of the argument is known - it is still referenced when it is an aggregate (or contains one), so
		// that the query remains an aggregate query
		auto type_name = WriteConstant(Value(RequireType(arg(0)).ToString()));
		if (SQLiteContainsAggregateOrWindow(arg(0))) {
			auto x = WriteExpression(arg(0));
			result = "(CASE WHEN " + x + " IS NULL THEN " + type_name + " ELSE " + type_name + " END)";
		} else {
			result = type_name;
		}
		return true;
	}
	if (name == "encode") {
		// the bytes of the string
		require_types(is_string);
		result = "CAST(" + WriteExpression(arg(0)) + " AS BLOB)";
		return true;
	}
	if (name == "regexp_extract") {
		if (children.size() < 2 || children.size() > 4 || !is_string(RequireType(arg(0))) ||
		    !is_string(RequireType(arg(1)))) {
			throw SQLitePushdownNotSupported("unsupported regexp_extract arguments");
		}
		int64_t group = 0;
		string options;
		if (children.size() >= 3) {
			Value group_value;
			if (!SQLiteCapabilities::TryGetConstantValue(arg(2), group_value) || group_value.IsNull()) {
				throw SQLitePushdownNotSupported("non-constant regexp_extract group");
			}
			if (group_value.type().id() == LogicalTypeId::VARCHAR && children.size() == 3) {
				options = StringValue::Get(group_value);
			} else if (!SQLiteCapabilities::TryGetIntegerLiteral(arg(2), group) || group < 0 || group > 9) {
				// DuckDB raises an error, or returns a STRUCT for a list of group names
				throw SQLitePushdownNotSupported("unsupported regexp_extract group");
			}
		}
		if (children.size() == 4 && !constant_string(3, options)) {
			throw SQLitePushdownNotSupported("non-constant regular expression options");
		}
		RequireExtensionFunctions(name);
		result = "duckdb_regexp_extract(" + WriteExpression(arg(0)) + ", " + WriteExpression(arg(1)) + ", " +
		         to_string(group) + ", " + WriteConstant(Value(options)) + ")";
		return true;
	}
	if (name == "hex" || name == "to_hex") {
		auto type = RequireType(arg(0));
		if (!is_string(type) && type.id() != LogicalTypeId::BLOB) {
			// DuckDB formats the value of integers in hexadecimal
			throw SQLitePushdownNotSupported("unsupported argument type for " + name);
		}
		// SQLite's hex of NULL is an empty string
		auto x = WriteExpression(arg(0));
		result = "(CASE WHEN " + x + " IS NULL THEN NULL ELSE hex(" + x + ") END)";
		return true;
	}
	if (name == "bit_length") {
		require_types(is_string);
		result = "(length(CAST(" + WriteExpression(arg(0)) + " AS BLOB)) * 8)";
		return true;
	}
	if (name == "to_base64" || name == "base64" || name == "from_base64") {
		auto type = RequireType(arg(0));
		bool to_base64 = name != "from_base64";
		if (to_base64 ? type.id() != LogicalTypeId::BLOB : !is_string(type)) {
			throw SQLitePushdownNotSupported("unsupported argument type for " + name);
		}
		RequireExtensionFunctions(name);
		result = string(to_base64 ? "duckdb_to_base64(" : "duckdb_from_base64(") + WriteExpression(arg(0)) + ")";
		return true;
	}
	if (name == "md5" || name == "sha256") {
		auto type = RequireType(arg(0));
		if (!is_string(type) && type.id() != LogicalTypeId::BLOB) {
			throw SQLitePushdownNotSupported("unsupported argument type for " + name);
		}
		RequireExtensionFunctions(name);
		result = "duckdb_" + name + "(" + WriteExpression(arg(0)) + ")";
		return true;
	}
	return false;
}

bool SQLiteSQLWriter::TryWriteMathFunction(const FunctionExpression &func, const string &name,
                                           const vector<FunctionArgument> &children, string &result) {
	static const case_insensitive_map_t<string> UNARY_FUNCTIONS = {
	    {"sqrt", "sqrt"},       {"cbrt", "cbrt"},   {"ln", "ln"},        {"log10", "log10"}, {"log2", "log2"},
	    {"exp", "exp"},         {"sin", "sin"},     {"cos", "cos"},      {"tan", "tan"},     {"cot", "cot"},
	    {"asin", "asin"},       {"acos", "acos"},   {"atan", "atan"},    {"sinh", "sinh"},   {"cosh", "cosh"},
	    {"tanh", "tanh"},       {"asinh", "asinh"}, {"acosh", "acosh"},  {"atanh", "atanh"}, {"degrees", "degrees"},
	    {"radians", "radians"}, {"ceil", "ceil"},   {"ceiling", "ceil"}, {"floor", "floor"}, {"trunc", "trunc"},
	    {"even", "even"}};
	static const case_insensitive_set_t BINARY_FUNCTIONS = {"pow", "power", "^", "**", "atan2"};
	static const case_insensitive_set_t ROUND_FUNCTIONS = {"round", "round_even", "roundbankers"};
	auto unary = UNARY_FUNCTIONS.find(name);
	bool is_log = name == "log";
	bool is_check = name == "isnan" || name == "isinf" || name == "isfinite";
	if (unary == UNARY_FUNCTIONS.end() && BINARY_FUNCTIONS.count(name) == 0 && ROUND_FUNCTIONS.count(name) == 0 &&
	    !is_log && !is_check) {
		return false;
	}
	auto arg = [&](idx_t i) -> const ParsedExpression & {
		return children[i].GetExpression();
	};
	for (auto &child : children) {
		if (!SQLiteIsNumericType(RequireType(child.GetExpression()))) {
			throw SQLitePushdownNotSupported("unsupported argument type for " + name);
		}
	}
	if (is_check) {
		// SQLite has no NaN - infinity is the only value that is not finite
		auto x = WriteExpression(arg(0));
		if (name == "isnan") {
			result = "(CASE WHEN " + x + " IS NULL THEN NULL ELSE 0 END)";
		} else if (name == "isinf") {
			result = "(abs(" + x + ") = 9e999)";
		} else {
			result = "(abs(" + x + ") < 9e999)";
		}
		return true;
	}
	Value ieee_floating_point_ops;
	if (context.TryGetCurrentSetting("ieee_floating_point_ops", ieee_floating_point_ops) &&
	    !ieee_floating_point_ops.IsNull() && !BooleanValue::Get(ieee_floating_point_ops)) {
		// DuckDB raises errors where the functions yield NaN / inf
		throw SQLitePushdownNotSupported("non-default floating point semantics");
	}
	auto result_type = RequireType(func);
	if (ROUND_FUNCTIONS.count(name) > 0 || (name == "trunc" && children.size() == 2)) {
		int64_t precision = 0;
		if (children.size() == 2 && !SQLiteCapabilities::TryGetIntegerLiteral(arg(1), precision)) {
			throw SQLitePushdownNotSupported("non-constant " + name + " precision");
		}
		auto x = WriteExpression(arg(0));
		bool integer = SQLiteIsIntegerType(RequireType(arg(0)));
		if (integer && precision >= 0) {
			// rounding integers to a non-negative precision does not change them
			result = x;
			return true;
		}
		RequireExtensionFunctions(name);
		string half_even = name == "round" ? "0" : "1";
		if (name == "trunc") {
			if (integer) {
				throw SQLitePushdownNotSupported("unsupported trunc of an integer");
			}
			result = "duckdb_trunc_precision(" + x + ", " + to_string(precision) + ")";
		} else if (integer) {
			result = "duckdb_round_integer(" + x + ", " + to_string(precision) + ", " + half_even + ")";
		} else {
			result = "duckdb_round(" + x + ", " + to_string(precision) + ", " + half_even + ")";
		}
		return true;
	}
	if (SQLiteIsIntegerType(result_type) && children.size() == 1) {
		// e.g. trunc of an integer
		result = WriteExpression(arg(0));
		return true;
	}
	if (result_type.id() != LogicalTypeId::DOUBLE) {
		throw SQLitePushdownNotSupported("unsupported result type for " + name);
	}
	RequireExtensionFunctions(name);
	if (is_log) {
		if (children.size() == 1) {
			result = "duckdb_log10(" + WriteExpression(arg(0)) + ")";
		} else {
			result = "duckdb_log(" + WriteExpression(arg(0)) + ", " + WriteExpression(arg(1)) + ")";
		}
		return true;
	}
	if (unary != UNARY_FUNCTIONS.end()) {
		result = "duckdb_" + unary->second + "(" + WriteExpression(arg(0)) + ")";
		return true;
	}
	result = string(name == "atan2" ? "duckdb_atan2(" : "duckdb_pow(") + WriteExpression(arg(0)) + ", " +
	         WriteExpression(arg(1)) + ")";
	return true;
}

//! Normalize a DuckDB date part name, returns an empty string for unsupported parts
static string SQLiteNormalizeDatePart(const string &part) {
	static const case_insensitive_map_t<string> DATE_PARTS = {
	    {"year", "year"},
	    {"years", "year"},
	    {"y", "year"},
	    {"yr", "year"},
	    {"yrs", "year"},
	    {"month", "month"},
	    {"months", "month"},
	    {"mon", "month"},
	    {"mons", "month"},
	    {"day", "day"},
	    {"days", "day"},
	    {"d", "day"},
	    {"dayofmonth", "day"},
	    {"hour", "hour"},
	    {"hours", "hour"},
	    {"h", "hour"},
	    {"hr", "hour"},
	    {"hrs", "hour"},
	    {"minute", "minute"},
	    {"minutes", "minute"},
	    {"m", "minute"},
	    {"min", "minute"},
	    {"mins", "minute"},
	    {"second", "second"},
	    {"seconds", "second"},
	    {"s", "second"},
	    {"sec", "second"},
	    {"secs", "second"},
	    {"millisecond", "millisecond"},
	    {"milliseconds", "millisecond"},
	    {"ms", "millisecond"},
	    {"msec", "millisecond"},
	    {"msecs", "millisecond"},
	    {"microsecond", "microsecond"},
	    {"microseconds", "microsecond"},
	    {"us", "microsecond"},
	    {"usec", "microsecond"},
	    {"usecs", "microsecond"},
	    {"quarter", "quarter"},
	    {"quarters", "quarter"},
	    {"week", "week"},
	    {"weeks", "week"},
	    {"w", "week"},
	    {"weekofyear", "week"},
	    {"dow", "dow"},
	    {"dayofweek", "dow"},
	    {"weekday", "dow"},
	    {"isodow", "isodow"},
	    {"doy", "doy"},
	    {"dayofyear", "doy"},
	    {"isoyear", "isoyear"},
	    {"yearweek", "yearweek"},
	    {"decade", "decade"},
	    {"decades", "decade"},
	    {"century", "century"},
	    {"centuries", "century"},
	    {"millennium", "millennium"},
	    {"millennia", "millennium"},
	    {"millenniums", "millennium"},
	    {"era", "era"},
	    {"epoch", "epoch"},
	    {"julian", "julian"},
	};
	auto entry = DATE_PARTS.find(part);
	return entry == DATE_PARTS.end() ? string() : entry->second;
}

bool SQLiteSQLWriter::TryWriteDateFunction(const string &name, const vector<FunctionArgument> &children,
                                           string &result) {
	static const case_insensitive_map_t<string> PART_FUNCTIONS = {{"year", "year"},
	                                                              {"month", "month"},
	                                                              {"day", "day"},
	                                                              {"dayofmonth", "day"},
	                                                              {"hour", "hour"},
	                                                              {"minute", "minute"},
	                                                              {"second", "second"},
	                                                              {"millisecond", "millisecond"},
	                                                              {"microsecond", "microsecond"},
	                                                              {"quarter", "quarter"},
	                                                              {"dayofweek", "dow"},
	                                                              {"isodow", "isodow"},
	                                                              {"dayofyear", "doy"},
	                                                              {"week", "week"},
	                                                              {"weekofyear", "week"},
	                                                              {"isoyear", "isoyear"},
	                                                              {"yearweek", "yearweek"},
	                                                              {"decade", "decade"},
	                                                              {"century", "century"},
	                                                              {"millennium", "millennium"},
	                                                              {"era", "era"},
	                                                              {"epoch", "epoch"},
	                                                              {"epoch_ms", "epoch_ms"},
	                                                              {"epoch_us", "epoch_us"},
	                                                              {"epoch_ns", "epoch_ns"},
	                                                              {"julian", "julian"},
	                                                              {"dayname", "dayname"},
	                                                              {"monthname", "monthname"}};
	auto arg = [&](idx_t i) -> const ParsedExpression & {
		return children[i].GetExpression();
	};
	auto literal_part = [&](idx_t i) {
		Value value;
		if (!SQLiteCapabilities::TryGetConstantValue(arg(i), value) || value.IsNull() ||
		    value.type().id() != LogicalTypeId::VARCHAR) {
			throw SQLitePushdownNotSupported("non-constant date part");
		}
		auto part = SQLiteNormalizeDatePart(StringValue::Get(value));
		if (part.empty()) {
			throw SQLitePushdownNotSupported("unsupported date part " + StringValue::Get(value));
		}
		return part;
	};
	auto require_timestamp = [&](idx_t i) {
		if (!SQLiteIsTimestampType(RequireType(arg(i)))) {
			throw SQLitePushdownNotSupported("unsupported argument type for " + name);
		}
	};
	auto part_entry = PART_FUNCTIONS.find(name);
	if (part_entry != PART_FUNCTIONS.end() || name == "date_part" || name == "datepart") {
		string part;
		idx_t value_index = 0;
		if (part_entry != PART_FUNCTIONS.end()) {
			if (children.size() != 1) {
				throw SQLitePushdownNotSupported("unsupported " + name + " arguments");
			}
			part = part_entry->second;
		} else {
			if (children.size() != 2) {
				throw SQLitePushdownNotSupported("unsupported " + name + " arguments");
			}
			part = literal_part(0);
			value_index = 1;
		}
		// e.g. epoch_ms(BIGINT) converts milliseconds to a timestamp
		require_timestamp(value_index);
		RequireExtensionFunctions(name);
		result = "duckdb_date_part('" + part + "', " + WriteExpression(arg(value_index)) + ")";
		return true;
	}
	if (name == "date_trunc" || name == "datetrunc") {
		if (children.size() != 2) {
			throw SQLitePushdownNotSupported("unsupported " + name + " arguments");
		}
		auto part = literal_part(0);
		static const case_insensitive_set_t TRUNC_PARTS = {
		    "millennium", "century", "decade", "year",   "quarter", "month",       "week",
		    "isoyear",    "day",     "hour",   "minute", "second",  "millisecond", "microsecond"};
		if (TRUNC_PARTS.count(part) == 0) {
			throw SQLitePushdownNotSupported("unsupported date_trunc part " + part);
		}
		require_timestamp(1);
		RequireExtensionFunctions(name);
		result = "duckdb_date_trunc('" + part + "', " + WriteExpression(arg(1)) + ")";
		return true;
	}
	if (name == "date_diff" || name == "datediff") {
		if (children.size() != 3) {
			throw SQLitePushdownNotSupported("unsupported " + name + " arguments");
		}
		auto part = literal_part(0);
		static const case_insensitive_set_t DIFF_PARTS = {
		    "year", "month",   "day",  "decade", "century", "millennium",  "quarter",
		    "week", "isoyear", "hour", "minute", "second",  "millisecond", "microsecond"};
		if (DIFF_PARTS.count(part) == 0) {
			throw SQLitePushdownNotSupported("unsupported date_diff part " + part);
		}
		require_timestamp(1);
		require_timestamp(2);
		RequireExtensionFunctions(name);
		result = "duckdb_date_diff('" + part + "', " + WriteExpression(arg(1)) + ", " + WriteExpression(arg(2)) + ")";
		return true;
	}
	if (name == "last_day") {
		require_timestamp(0);
		// the first day of the next month of December 9999 cannot be represented by SQLite
		auto value = WriteExpression(arg(0));
		result = "(CASE WHEN strftime('%m', " + value + ") = '12' THEN strftime('%Y-12-31', " + value + ") ELSE date(" +
		         value + ", 'start of month', '+1 month', '-1 day') END)";
		return true;
	}
	if (name == "make_date") {
		if (children.size() != 3) {
			throw SQLitePushdownNotSupported("unsupported make_date arguments");
		}
		for (idx_t i = 0; i < 3; i++) {
			if (!SQLiteIsIntegerType(RequireType(arg(i)))) {
				throw SQLitePushdownNotSupported("unsupported argument type for make_date");
			}
		}
		RequireExtensionFunctions(name);
		result = "duckdb_make_date(" + WriteExpression(arg(0)) + ", " + WriteExpression(arg(1)) + ", " +
		         WriteExpression(arg(2)) + ")";
		return true;
	}
	if (name == "strftime") {
		Value format;
		if (children.size() != 2 || !SQLiteCapabilities::TryGetConstantValue(arg(1), format) || format.IsNull() ||
		    format.type().id() != LogicalTypeId::VARCHAR) {
			throw SQLitePushdownNotSupported("unsupported strftime arguments");
		}
		require_timestamp(0);
		RequireExtensionFunctions(name);
		result = "duckdb_strftime(" + WriteExpression(arg(0)) + ", " + WriteConstant(format) + ")";
		return true;
	}
	if (name == "date_add") {
		if (children.size() != 2) {
			throw SQLitePushdownNotSupported("unsupported date_add arguments");
		}
		result = WriteIntervalArithmetic(arg(0), arg(1), false);
		return true;
	}
	return false;
}

string SQLiteSQLWriter::WriteIntervalArithmetic(const ParsedExpression &value_expr,
                                                const ParsedExpression &interval_expr, bool subtract) {
	interval_t interval;
	Value value;
	if (SQLiteCapabilities::TryGetConstantValue(interval_expr, value) && !value.IsNull() &&
	    value.type().id() == LogicalTypeId::INTERVAL) {
		interval = IntervalValue::Get(value);
	} else if (!SQLiteCapabilities::TryGetIntervalLiteral(interval_expr, interval)) {
		throw SQLitePushdownNotSupported("non-constant interval");
	}
	if (!SQLiteIsTimestampType(RequireType(value_expr))) {
		throw SQLitePushdownNotSupported("unsupported interval arithmetic");
	}
	if (subtract) {
		if (interval.months == NumericLimits<int32_t>::Minimum() ||
		    interval.days == NumericLimits<int32_t>::Minimum() ||
		    interval.micros == NumericLimits<int64_t>::Minimum()) {
			throw SQLitePushdownNotSupported("interval out of range");
		}
		interval.months = -interval.months;
		interval.days = -interval.days;
		interval.micros = -interval.micros;
	}
	RequireExtensionFunctions("interval arithmetic");
	// the result is a TIMESTAMP, also for dates
	return "duckdb_add_interval(" + WriteExpression(value_expr) + ", " + to_string(interval.months) + ", " +
	       to_string(interval.days) + ", " + to_string(interval.micros) + ")";
}

string SQLiteSQLWriter::WriteAggregateCall(const string &name, const vector<FunctionArgument> &children,
                                           bool distinct) {
	string distinct_str = distinct ? "DISTINCT " : "";
	if ((name == "count_star" || name == "count") && children.empty()) {
		return "count(*)";
	}
	auto arg = [&](idx_t i) -> const ParsedExpression & {
		return children[i].GetExpression();
	};
	if (name == "count") {
		return "count(" + distinct_str + WriteExpression(arg(0)) + ")";
	}
	if (name == "sum" || name == "avg" || name == "mean") {
		auto type = RequireType(arg(0));
		if (!SQLiteIsNumericType(type)) {
			throw SQLitePushdownNotSupported("unsupported argument type for " + name);
		}
		if (name != "sum" && SQLiteIsIntegerType(type)) {
			// DuckDB sums the integers exactly - SQLite as floating point numbers once the sum overflows
			RequireExtensionFunctions("avg of integers");
			return "duckdb_avg(" + distinct_str + WriteExpression(arg(0)) + ")";
		}
		return string(name == "sum" ? "sum(" : "avg(") + distinct_str + WriteExpression(arg(0)) + ")";
	}
	if (name == "min" || name == "max") {
		auto type = RequireType(arg(0));
		if (SQLiteGetRepresentation(type) == SQLiteRepresentation::OTHER || type.id() == LogicalTypeId::DECIMAL ||
		    type.id() == LogicalTypeId::FLOAT) {
			throw SQLitePushdownNotSupported("unsupported argument type for " + name);
		}
		return name + "(" + distinct_str + WriteExpression(arg(0)) + ")";
	}
	if (name == "bool_and" || name == "bool_or") {
		if (RequireType(arg(0)).id() != LogicalTypeId::BOOLEAN) {
			throw SQLitePushdownNotSupported("unsupported argument type for " + name);
		}
		// booleans are represented as 0 / 1
		return string(name == "bool_and" ? "min(" : "max(") + WriteExpression(arg(0)) + ")";
	}
	if (name == "string_agg" || name == "group_concat" || name == "listagg") {
		auto value = WriteAs(arg(0), LogicalType::VARCHAR);
		if (distinct || children.size() == 1) {
			// the default separator of both is a comma
			return "group_concat(" + distinct_str + value + ")";
		}
		return "group_concat(" + value + ", " + WriteExpression(arg(1)) + ")";
	}
	static const case_insensitive_map_t<string> STATISTICS_FUNCTIONS = {
	    {"stddev", "stddev_samp"}, {"stddev_samp", "stddev_samp"}, {"stddev_pop", "stddev_pop"},
	    {"variance", "var_samp"},  {"var_samp", "var_samp"},       {"var_pop", "var_pop"},
	    {"product", "product"},    {"bit_and", "bit_and"},         {"bit_or", "bit_or"},
	    {"bit_xor", "bit_xor"}};
	auto statistics = STATISTICS_FUNCTIONS.find(name);
	if (statistics != STATISTICS_FUNCTIONS.end()) {
		auto type = RequireType(arg(0));
		bool bitwise = StringUtil::StartsWith(name, "bit_");
		if (bitwise ? !SQLiteIsIntegerType(type) : !SQLiteIsNumericType(type)) {
			throw SQLitePushdownNotSupported("unsupported argument type for " + name);
		}
		RequireExtensionFunctions(name);
		return "duckdb_" + statistics->second + "(" + WriteExpression(arg(0)) + ")";
	}
	if (name == "arg_min" || name == "arg_max" || name == "min_by" || name == "max_by" || name == "argmin" ||
	    name == "argmax") {
		for (idx_t i = 0; i < 2; i++) {
			auto type = RequireType(arg(i));
			if (SQLiteGetRepresentation(type) == SQLiteRepresentation::OTHER || type.id() == LogicalTypeId::DECIMAL ||
			    type.id() == LogicalTypeId::FLOAT) {
				throw SQLitePushdownNotSupported("unsupported argument type for " + name);
			}
		}
		RequireExtensionFunctions(name);
		bool is_max = name == "arg_max" || name == "max_by" || name == "argmax";
		return string(is_max ? "duckdb_arg_max(" : "duckdb_arg_min(") + WriteExpression(arg(0)) + ", " +
		       WriteExpression(arg(1)) + ")";
	}
	if (name == "median" || name == "quantile" || name == "quantile_cont" || name == "quantile_disc") {
		auto type = RequireType(arg(0));
		bool continuous = name == "median" || name == "quantile_cont";
		if (name == "median" && SQLiteIsStringType(type)) {
			// DuckDB's median of strings is discrete
			continuous = false;
		} else if (!SQLiteIsNumericType(type) && (continuous || !SQLiteIsStringType(type))) {
			throw SQLitePushdownNotSupported("unsupported argument type for " + name);
		}
		RequireExtensionFunctions(name);
		string quantile = "0.5";
		if (name != "median") {
			Value value;
			if (!SQLiteCapabilities::TryGetConstantValue(arg(1), value)) {
				throw InternalException("SQLiteSQLWriter: the quantile must be a constant");
			}
			quantile = SQLiteWriteDouble(value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>());
		}
		return string(continuous ? "duckdb_quantile_cont(" : "duckdb_quantile_disc(") + WriteExpression(arg(0)) + ", " +
		       quantile + ")";
	}
	throw InternalException("SQLiteSQLWriter: unsupported aggregate %s - should have been blocked by SupportsPushdown",
	                        name);
}

string SQLiteSQLWriter::WriteAggregate(const FunctionExpression &func, const string &name) {
	auto &children = func.GetArguments();
	string result;
	if (func.OrderBy() && !func.OrderBy()->orders.empty()) {
		auto &orders = func.OrderBy()->orders;
		if (name == "string_agg" || name == "group_concat" || name == "listagg") {
			RequireExtensionFunctions("an ordered " + name);
			auto &config = DBConfig::GetConfig(context);
			string order;
			string keys;
			for (auto &entry : orders) {
				auto order_type = config.ResolveOrder(context, entry.type);
				auto null_order = config.ResolveNullOrder(context, order_type, entry.null_order);
				order += order_type == OrderType::DESCENDING ? "D" : "A";
				order += null_order == OrderByNullType::NULLS_FIRST ? "F" : "L";
				auto key_type = RequireType(*entry.expression);
				if (SQLiteGetRepresentation(key_type) == SQLiteRepresentation::OTHER ||
				    key_type.id() == LogicalTypeId::DECIMAL || key_type.id() == LogicalTypeId::FLOAT) {
					throw SQLitePushdownNotSupported("unsupported ORDER BY key of " + name);
				}
				keys += ", " + WriteExpression(*entry.expression);
			}
			auto separator = children.size() > 1 ? WriteExpression(children[1].GetExpression()) : string("','");
			result = "duckdb_string_agg(" + WriteAs(children[0].GetExpression(), LogicalType::VARCHAR) + ", " +
			         separator + ", '" + order + "'" + keys + ")";
		} else {
			if ((name == "sum" || name == "avg" || name == "mean") &&
			    !SQLiteIsIntegerType(RequireType(children[0].GetExpression()))) {
				// floating point sums depend on the order of the values
				throw SQLitePushdownNotSupported("ordered " + name + " of floating point numbers");
			}
			// the result does not depend on the order
			result = WriteAggregateCall(name, children, func.Distinct());
		}
		if (func.Filter()) {
			result += " FILTER (WHERE " + WriteCondition(*func.Filter()) + ")";
		}
		return result;
	}
	if (name == "count_if" || name == "countif") {
		result = "count(CASE WHEN " + WriteExpression(children[0].GetExpression()) + " THEN 1 END)";
	} else {
		result = WriteAggregateCall(name, children, func.Distinct());
	}
	if (func.Filter()) {
		result += " FILTER (WHERE " + WriteCondition(*func.Filter()) + ")";
	}
	return result;
}

string SQLiteSQLWriter::WriteFunction(const FunctionExpression &func) {
	auto &children = func.GetArguments();
	auto name = StringUtil::Lower(func.FunctionName().GetIdentifierName());
	if (SQLiteCapabilities::IsAggregateFunction(name)) {
		return WriteAggregate(func, name);
	}
	string result;
	if (TryWriteScalarFunction(func, name, children, result)) {
		return result;
	}
	throw InternalException("SQLiteSQLWriter: unsupported function %s - should have been blocked by SupportsPushdown",
	                        name);
}

static string SQLiteWriteWindowBoundary(WindowBoundary boundary, const string &boundary_expr) {
	switch (boundary) {
	case WindowBoundary::UNBOUNDED_PRECEDING:
		return "UNBOUNDED PRECEDING";
	case WindowBoundary::UNBOUNDED_FOLLOWING:
		return "UNBOUNDED FOLLOWING";
	case WindowBoundary::CURRENT_ROW_ROWS:
	case WindowBoundary::CURRENT_ROW_RANGE:
	case WindowBoundary::CURRENT_ROW_GROUPS:
		return "CURRENT ROW";
	case WindowBoundary::EXPR_PRECEDING_ROWS:
	case WindowBoundary::EXPR_PRECEDING_RANGE:
	case WindowBoundary::EXPR_PRECEDING_GROUPS:
		return boundary_expr + " PRECEDING";
	case WindowBoundary::EXPR_FOLLOWING_ROWS:
	case WindowBoundary::EXPR_FOLLOWING_RANGE:
	case WindowBoundary::EXPR_FOLLOWING_GROUPS:
		return boundary_expr + " FOLLOWING";
	default:
		throw InternalException("SQLiteSQLWriter: unsupported window boundary - should have been blocked by "
		                        "SupportsPushdown");
	}
}

static bool SQLiteIsRowsBoundary(WindowBoundary boundary) {
	return boundary == WindowBoundary::CURRENT_ROW_ROWS || boundary == WindowBoundary::EXPR_PRECEDING_ROWS ||
	       boundary == WindowBoundary::EXPR_FOLLOWING_ROWS;
}

static bool SQLiteIsGroupsBoundary(WindowBoundary boundary) {
	return boundary == WindowBoundary::CURRENT_ROW_GROUPS || boundary == WindowBoundary::EXPR_PRECEDING_GROUPS ||
	       boundary == WindowBoundary::EXPR_FOLLOWING_GROUPS;
}

string SQLiteSQLWriter::WriteWindow(const WindowExpression &window) {
	auto name = StringUtil::Lower(window.FunctionName().GetIdentifierName());
	auto &children = window.GetArguments();
	string result;
	bool sliding_frame = window.WindowStart() != WindowBoundary::UNBOUNDED_PRECEDING ||
	                     window.WindowExclude() != WindowExcludeMode::NO_OTHER;
	if (sliding_frame && (name == "sum" || name == "avg" || name == "mean") &&
	    RequireType(children[0].GetExpression()).id() == LogicalTypeId::DOUBLE) {
		// SQLite removes the values that leave the frame from the floating point sum - which can lose all precision
		RequireExtensionFunctions("a floating point " + name + " over a sliding window frame");
		result = string(name == "sum" ? "duckdb_frame_sum(" : "duckdb_frame_avg(") +
		         WriteExpression(children[0].GetExpression()) + ")";
	} else if (SQLiteCapabilities::IsAggregateFunction(name)) {
		result = WriteAggregateCall(name, children, false);
	} else {
		if (name == "rank_dense") {
			name = "dense_rank";
		}
		result = name + "(";
		for (idx_t i = 0; i < children.size(); i++) {
			auto &child = children[i].GetExpression();
			if ((name == "lead" || name == "lag") && i == 2) {
				// the default value is converted to the type of the value
				result += ", " + WriteAs(child, RequireType(children[0].GetExpression()));
				continue;
			}
			result += (i > 0 ? ", " : "") + WriteExpression(child);
		}
		result += ")";
	}
	if (window.Filter()) {
		result += " FILTER (WHERE " + WriteCondition(*window.Filter()) + ")";
	}
	result += " OVER (";
	string sep;
	if (!window.Partitions().empty()) {
		result += "PARTITION BY " + WriteExpressionList(window.Partitions());
		sep = " ";
	}
	if (!window.OrderBy().empty()) {
		result += sep + "ORDER BY " + WriteOrderList(window.OrderBy());
		sep = " ";
	}
	auto window_start = window.WindowStart();
	auto window_end = window.WindowEnd();
	if (!(window_start == WindowBoundary::UNBOUNDED_PRECEDING && window_end == WindowBoundary::CURRENT_ROW_RANGE &&
	      window.WindowExclude() == WindowExcludeMode::NO_OTHER)) {
		// non-default frame
		string mode = "RANGE";
		if (SQLiteIsRowsBoundary(window_start) || SQLiteIsRowsBoundary(window_end)) {
			mode = "ROWS";
		} else if (SQLiteIsGroupsBoundary(window_start) || SQLiteIsGroupsBoundary(window_end)) {
			mode = "GROUPS";
		}
		if (mode == "RANGE" && (window.StartExpr() || window.EndExpr())) {
			// SQLite requires a single numeric ORDER BY key for RANGE frames with an offset
			if (window.OrderBy().size() != 1 || !SQLiteIsNumericType(RequireType(*window.OrderBy()[0].expression))) {
				throw SQLitePushdownNotSupported("unsupported RANGE frame");
			}
		}
		string start_expr = window.StartExpr() ? WriteExpression(*window.StartExpr()) : string();
		string end_expr = window.EndExpr() ? WriteExpression(*window.EndExpr()) : string();
		result += sep + mode;
		result += " BETWEEN " + SQLiteWriteWindowBoundary(window_start, start_expr);
		result += " AND " + SQLiteWriteWindowBoundary(window_end, end_expr);
		switch (window.WindowExclude()) {
		case WindowExcludeMode::NO_OTHER:
			break;
		case WindowExcludeMode::CURRENT_ROW:
			result += " EXCLUDE CURRENT ROW";
			break;
		case WindowExcludeMode::GROUP:
			result += " EXCLUDE GROUP";
			break;
		case WindowExcludeMode::TIES:
			result += " EXCLUDE TIES";
			break;
		}
	}
	result += ")";
	return result;
}

//===--------------------------------------------------------------------===//
// Table references
//===--------------------------------------------------------------------===//
//! How DuckDB reads a column of a SQLite table, as a SQLite expression - empty if the stored value is read as is
static string SQLiteColumnRead(const SQLiteTableEntry &table, const ColumnDefinition &column, const string &name) {
	auto index = column.Logical().index;
	bool text_affinity = index < table.utf8_text_affinity.size() && table.utf8_text_affinity[index];
	bool binary_collation = index >= table.binary_collation.size() || table.binary_collation[index];
	switch (column.Type().id()) {
	case LogicalTypeId::BIGINT:
		// values of other storage classes cannot be read
		return string();
	case LogicalTypeId::DOUBLE:
		if (index < table.real_affinity.size() && !table.real_affinity[index]) {
			// e.g. a NUMERIC column, which stores integral values as integers - they are compared, summed, ... as
			// doubles in DuckDB
			return "CAST(" + name + " AS REAL)";
		}
		return string();
	case LogicalTypeId::DATE:
		// dates are stored as text or as unix timestamps
		return "duckdb_date(" + name + ")";
	case LogicalTypeId::TIMESTAMP:
		return "duckdb_timestamp(" + name + ")";
	case LogicalTypeId::BLOB:
		// any value is read as its bytes
		return "CAST(" + name + " AS BLOB)";
	case LogicalTypeId::VARCHAR: {
		string result;
		if (table.all_varchar || !text_affinity) {
			// numbers are read as their text - and a string constant would be converted to a number when compared
			// with a column that does not have TEXT affinity
			result = "CAST(" + name + " AS TEXT)";
		}
		if (!binary_collation) {
			// DuckDB compares strings byte by byte
			result = (result.empty() ? name : result) + " COLLATE BINARY";
		}
		return result;
	}
	default:
		throw SQLitePushdownNotSupported("unsupported column type " + column.Type().ToString());
	}
}

string SQLiteSQLWriter::WriteColumnAliases(const string &relation, const vector<Identifier> &aliases,
                                           const vector<string> &names, const string &alias) {
	if (aliases.size() > names.size()) {
		throw SQLitePushdownNotSupported("too many column aliases");
	}
	// a CTE with a column list renames the columns of the relation
	string result = "(WITH " + alias + "(";
	for (idx_t i = 0; i < names.size(); i++) {
		result += (i > 0 ? ", " : "") + WriteIdentifier(i < aliases.size() ? aliases[i].GetIdentifierName() : names[i]);
	}
	result += ") AS (SELECT * FROM " + relation + ") SELECT * FROM " + alias + ") AS " + alias;
	return result;
}

string SQLiteSQLWriter::WriteBaseTableRef(const BaseTableRef &ref) {
	if (IsCTEReference(ref)) {
		auto name = WriteIdentifier(ref.Table());
		auto alias = ref.alias.empty() ? name : WriteIdentifier(ref.alias);
		if (!ref.column_name_alias.empty()) {
			vector<string> names;
			auto probe = make_uniq<SelectNode>();
			probe->select_list.push_back(make_uniq<StarExpression>());
			auto cte_ref = make_uniq<BaseTableRef>();
			cte_ref->SetQualifiedName(ref.GetQualifiedName());
			probe->from_table = std::move(cte_ref);
			InferQueryTypes(*probe, &names);
			if (names.empty()) {
				throw SQLitePushdownNotSupported("cannot determine the columns of a CTE");
			}
			return WriteColumnAliases(name, ref.column_name_alias, names, alias);
		}
		return ref.alias.empty() ? name : name + " AS " + alias;
	}
	auto table = LookupTable(ref);
	if (!table) {
		// e.g. a view - which is executed with DuckDB's semantics
		throw SQLitePushdownNotSupported("table " + ref.Table().GetIdentifierName() + " not found");
	}
	auto table_name = WriteIdentifier(table->name);
	auto binding = WriteIdentifier(ref.alias.empty() ? table->name : ref.alias);
	auto &columns = table->GetColumns();
	vector<string> reads;
	bool requires_conversion = !ref.column_name_alias.empty();
	for (auto &column : columns.Logical()) {
		auto column_name = WriteIdentifier(column.Name());
		auto read = SQLiteColumnRead(*table, column, column_name);
		if (!all_columns_referenced && referenced_columns.count(column.Name().GetIdentifierName()) == 0) {
			// the column is not used by the query
			read = string();
		}
		if (!read.empty()) {
			requires_conversion = true;
			if (statement_mode) {
				// the conversions use SQL that DuckDB cannot read back (e.g. a COLLATE BINARY)
				throw SQLitePushdownNotSupported("a view of a column that requires a conversion");
			}
		}
		reads.push_back(read.empty() ? column_name : read);
	}
	if (!requires_conversion) {
		return ref.alias.empty() ? table_name : table_name + " AS " + binding;
	}
	// read the columns the way the table scan reads them
	if (ref.column_name_alias.size() > reads.size()) {
		throw SQLitePushdownNotSupported("too many column aliases");
	}
	string result = "(SELECT ";
	idx_t i = 0;
	for (auto &column : columns.Logical()) {
		auto &name = i < ref.column_name_alias.size() ? ref.column_name_alias[i] : column.Name();
		result += (i > 0 ? ", " : "") + reads[i] + " AS " + WriteIdentifier(name);
		i++;
	}
	if (!all_columns_referenced && referenced_columns.count("rowid") > 0 && !table->ColumnExists(Identifier("rowid"))) {
		// the row id of the table (which is not part of a star expansion)
		result += ", \"rowid\" AS \"rowid\"";
	}
	return result + " FROM " + table_name + ") AS " + binding;
}

string SQLiteSQLWriter::WriteTableRef(const TableRef &ref) {
	switch (ref.type) {
	case TableReferenceType::BASE_TABLE:
		return WriteBaseTableRef(ref.Cast<BaseTableRef>());
	case TableReferenceType::JOIN: {
		auto &join = ref.Cast<JoinRef>();
		if (!join.alias.empty() || !join.column_name_alias.empty()) {
			throw SQLitePushdownNotSupported("aliased join");
		}
		if (join.type == JoinType::SEMI || join.type == JoinType::ANTI) {
			// a SEMI / ANTI join filters the left side - which keeps the name it is bound to
			auto &left = *join.left;
			if (!left.column_name_alias.empty()) {
				throw SQLitePushdownNotSupported("SEMI / ANTI join with column aliases");
			}
			Identifier binding_name = left.alias.empty() && left.type == TableReferenceType::BASE_TABLE
			                              ? left.Cast<BaseTableRef>().Table()
			                              : left.alias;
			string result = "(SELECT * FROM " + WriteTableRef(left) + " WHERE ";
			result += join.type == JoinType::SEMI ? "EXISTS" : "NOT EXISTS";
			result += " (SELECT 1 FROM " + WriteTableRef(*join.right) + " WHERE " + WriteCondition(*join.condition) +
			          ")) AS " + WriteIdentifier(binding_name);
			return result;
		}
		if (join.type == JoinType::RIGHT) {
			if (unqualified_star) {
				throw SQLitePushdownNotSupported("RIGHT JOIN with an unqualified *");
			}
			// SQLite 3.38 has no RIGHT JOIN - the sides are swapped
			return "(" + WriteTableRef(*join.right) + " LEFT JOIN " + WriteTableRef(*join.left) + " ON " +
			       WriteCondition(*join.condition) + ")";
		}
		string result = "(" + WriteTableRef(*join.left) + " ";
		if (join.ref_type == JoinRefType::CROSS) {
			// a CROSS JOIN determines the join order in SQLite - an inner join without a condition does not
			result += "JOIN ";
		} else {
			if (join.ref_type == JoinRefType::NATURAL) {
				result += "NATURAL ";
			}
			switch (join.type) {
			case JoinType::INNER:
				result += "INNER JOIN ";
				break;
			case JoinType::LEFT:
				result += "LEFT JOIN ";
				break;
			default:
				throw InternalException("SQLiteSQLWriter: unsupported join type - should have been blocked by "
				                        "SupportsPushdown");
			}
		}
		result += WriteTableRef(*join.right);
		if (join.condition) {
			result += " ON " + WriteCondition(*join.condition);
		} else if (!join.using_columns.empty()) {
			result += " USING (";
			for (idx_t i = 0; i < join.using_columns.size(); i++) {
				result += (i > 0 ? ", " : "") + WriteIdentifier(join.using_columns[i]);
			}
			result += ")";
		} else if (join.ref_type == JoinRefType::REGULAR) {
			// an INNER JOIN without a condition
			result += " ON 1";
		}
		return result + ")";
	}
	case TableReferenceType::SUBQUERY: {
		auto &subquery = ref.Cast<SubqueryRef>();
		auto query = "(" + WriteQueryNode(*subquery.subquery->node) + ")";
		string alias;
		if (subquery.alias.empty()) {
			alias = "unnamed_subquery";
			if (unnamed_subquery_index > 0) {
				alias += to_string(unnamed_subquery_index);
			}
			unnamed_subquery_index++;
			alias = WriteIdentifier(alias);
		} else {
			alias = WriteIdentifier(subquery.alias);
		}
		if (!subquery.column_name_alias.empty()) {
			vector<string> names;
			InferQueryTypes(*subquery.subquery->node, &names);
			if (names.empty()) {
				throw SQLitePushdownNotSupported("cannot determine the columns of a subquery");
			}
			return WriteColumnAliases(query, subquery.column_name_alias, names, alias);
		}
		return query + " AS " + alias;
	}
	case TableReferenceType::EXPRESSION_LIST: {
		auto &expression_list = ref.Cast<ExpressionListRef>();
		// the columns of a VALUES list are named column1, ... in SQLite, and valueslist.col0, ... in DuckDB
		auto column_count = expression_list.values[0].size();
		string result = "(SELECT ";
		for (idx_t col = 0; col < column_count; col++) {
			string name;
			if (col < ref.column_name_alias.size()) {
				name = ref.column_name_alias[col].GetIdentifierName();
			} else if (col < expression_list.expected_names.size()) {
				name = expression_list.expected_names[col].GetIdentifierName();
			} else {
				name = "col" + to_string(col);
			}
			result += (col > 0 ? ", " : "") + string("column") + to_string(col + 1) + " AS " + WriteIdentifier(name);
		}
		result += " FROM (VALUES ";
		for (idx_t row = 0; row < expression_list.values.size(); row++) {
			if (expression_list.values[row].size() != column_count) {
				throw SQLitePushdownNotSupported("VALUES rows with a different number of columns");
			}
			result += (row > 0 ? ", " : "") + string("(") + WriteExpressionList(expression_list.values[row]) + ")";
		}
		result += ")) AS " + WriteIdentifier(ref.alias.empty() ? Identifier("valueslist") : ref.alias);
		return result;
	}
	default:
		throw InternalException("SQLiteSQLWriter: unsupported table ref type - should have been blocked by "
		                        "SupportsPushdown");
	}
}

//===--------------------------------------------------------------------===//
// Result modifiers
//===--------------------------------------------------------------------===//
//! Integer literals in ORDER BY / GROUP BY are positional references - other constants (e.g. a cast of an integer
//! literal) are written so that SQLite does not interpret them as positions either
string SQLiteSQLWriter::WriteOrderingKey(const ParsedExpression &expr) {
	Value value;
	if (expr.GetExpressionClass() != ExpressionClass::CONSTANT &&
	    SQLiteCapabilities::TryGetConstantValue(expr, value)) {
		// SQLite's unary plus does not convert its operand
		return "(+" + WriteConstant(value) + ")";
	}
	return WriteExpression(expr);
}

//! The leftmost SELECT of a set operation, which determines the names of its result columns
static optional_ptr<const SelectNode> SQLiteLeftmostSelect(const QueryNode &node) {
	switch (node.type) {
	case QueryNodeType::SELECT_NODE:
		return &node.Cast<SelectNode>();
	case QueryNodeType::SET_OPERATION_NODE: {
		auto &setop = node.Cast<SetOperationNode>();
		return setop.children.empty() ? nullptr : SQLiteLeftmostSelect(*setop.children[0]);
	}
	case QueryNodeType::RECURSIVE_CTE_NODE:
		return SQLiteLeftmostSelect(*node.Cast<RecursiveCTENode>().left);
	default:
		return nullptr;
	}
}

string SQLiteSQLWriter::WriteOrderList(const vector<OrderByNode> &orders, optional_ptr<const QueryNode> node) {
	auto &config = DBConfig::GetConfig(context);
	string result;
	for (auto &order : orders) {
		auto order_type = config.ResolveOrder(context, order.type);
		auto null_order = config.ResolveNullOrder(context, order_type, order.null_order);
		vector<string> keys;
		if (order.expression->GetExpressionClass() == ExpressionClass::STAR) {
			// ORDER BY ALL - order by all select-list positions
			auto leftmost = node ? SQLiteLeftmostSelect(*node) : nullptr;
			if (!leftmost) {
				throw SQLitePushdownNotSupported("ORDER BY ALL without a select list");
			}
			for (idx_t i = 0; i < leftmost->select_list.size(); i++) {
				keys.push_back(to_string(i + 1));
			}
		} else {
			keys.push_back(WriteOrderingKey(*order.expression));
		}
		for (auto &key : keys) {
			if (!result.empty()) {
				result += ", ";
			}
			result += key;
			result += order_type == OrderType::DESCENDING ? " DESC" : " ASC";
			// SQLite sorts NULLs as smaller than any value, DuckDB sorts them last by default
			result += null_order == OrderByNullType::NULLS_FIRST ? " NULLS FIRST" : " NULLS LAST";
		}
	}
	return result;
}

string SQLiteSQLWriter::WriteResultModifiers(const QueryNode &node) {
	string result;
	for (auto &modifier : node.modifiers) {
		switch (modifier->type) {
		case ResultModifierType::ORDER_MODIFIER: {
			auto &order_modifier = modifier->Cast<OrderModifier>();
			result += " ORDER BY " + WriteOrderList(order_modifier.orders, &node);
			break;
		}
		case ResultModifierType::LIMIT_MODIFIER: {
			auto &limit_modifier = modifier->Cast<LimitModifier>();
			// SQLite requires a LIMIT for an OFFSET - a negative LIMIT is no limit
			result += " LIMIT " + (limit_modifier.limit ? WriteExpression(*limit_modifier.limit) : string("-1"));
			if (limit_modifier.offset) {
				result += " OFFSET " + WriteExpression(*limit_modifier.offset);
			}
			break;
		}
		case ResultModifierType::DISTINCT_MODIFIER:
			// handled in WriteSelectNode
			break;
		default:
			throw InternalException("SQLiteSQLWriter: unsupported result modifier - should have been blocked by "
			                        "SupportsPushdown");
		}
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Query nodes
//===--------------------------------------------------------------------===//
string SQLiteSQLWriter::WriteCTEMap(const CommonTableExpressionMap &cte_map) {
	if (cte_map.map.empty()) {
		return string();
	}
	bool has_recursive = false;
	for (auto &kv : cte_map.map) {
		if (kv.second->query_node && kv.second->query_node->type == QueryNodeType::RECURSIVE_CTE_NODE) {
			has_recursive = true;
			break;
		}
	}
	string result = "WITH ";
	if (has_recursive) {
		result += "RECURSIVE ";
	}
	bool first_cte = true;
	for (auto &kv : cte_map.map) {
		if (!first_cte) {
			result += ", ";
		}
		auto &cte = *kv.second;
		result += WriteIdentifier(kv.first);
		if (!cte.aliases.empty()) {
			result += " (";
			for (idx_t k = 0; k < cte.aliases.size(); k++) {
				result += (k > 0 ? ", " : "") + WriteIdentifier(cte.aliases[k]);
			}
			result += ")";
		}
		result += " AS ";
		switch (cte.materialized) {
		case CTEMaterialize::CTE_MATERIALIZE_ALWAYS:
			result += "MATERIALIZED ";
			break;
		case CTEMaterialize::CTE_MATERIALIZE_NEVER:
			result += "NOT MATERIALIZED ";
			break;
		default:
			break;
		}
		result += "(" + WriteQueryNode(*cte.query_node) + ")";
		first_cte = false;
	}
	result += " ";
	return result;
}

//! The name of a result column of a SELECT
static string SQLiteSelectEntryName(const ParsedExpression &expr) {
	if (!expr.GetAlias().empty()) {
		return expr.GetAlias().GetIdentifierName();
	}
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		return expr.Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName();
	}
	return expr.ToString();
}

//! A condition that is moved into the select list is converted to BOOLEAN
static unique_ptr<ParsedExpression> SQLiteBooleanCondition(unique_ptr<ParsedExpression> condition) {
	if (SQLiteIsBooleanExpression(*condition)) {
		return condition;
	}
	return make_uniq<CastExpression>(LogicalType::BOOLEAN, std::move(condition));
}

string SQLiteSQLWriter::WriteFilteredSelectNode(const SelectNode &node) {
	// SQLite has no QUALIFY / DISTINCT ON - the rows are filtered by a query over the SELECT, which computes the
	// filter (the QUALIFY condition, or the first row of each DISTINCT ON group)
	vector<string> names;
	case_insensitive_set_t unique_names;
	for (auto &expr : node.select_list) {
		auto name = SQLiteSelectEntryName(*expr);
		if (expr->GetExpressionClass() == ExpressionClass::STAR || !unique_names.insert(name).second) {
			throw SQLitePushdownNotSupported("QUALIFY / DISTINCT ON / HAVING without uniquely named result columns");
		}
		names.push_back(name);
	}
	owned_nodes.push_back(node.Copy());
	auto &inner = owned_nodes.back()->Cast<SelectNode>();
	inner.modifiers.clear();
	inner.cte_map.map.clear();
	string filter;
	bool distinct = false;
	optional_ptr<const OrderModifier> order_modifier;
	optional_ptr<const LimitModifier> limit_modifier;
	for (auto &modifier : node.modifiers) {
		switch (modifier->type) {
		case ResultModifierType::ORDER_MODIFIER:
			order_modifier = &modifier->Cast<OrderModifier>();
			break;
		case ResultModifierType::LIMIT_MODIFIER:
			limit_modifier = &modifier->Cast<LimitModifier>();
			break;
		case ResultModifierType::DISTINCT_MODIFIER: {
			auto &distinct_modifier = modifier->Cast<DistinctModifier>();
			if (distinct_modifier.distinct_on_targets.empty()) {
				distinct = true;
				break;
			}
			// the first row (by the ORDER BY) of each group
			auto row_number = make_uniq<WindowExpression>(string(), string(), "row_number");
			row_number->WindowStartMutable() = WindowBoundary::UNBOUNDED_PRECEDING;
			row_number->WindowEndMutable() = WindowBoundary::CURRENT_ROW_RANGE;
			for (auto &target : distinct_modifier.distinct_on_targets) {
				row_number->PartitionsMutable().push_back(target->Copy());
			}
			for (auto &mod : node.modifiers) {
				if (mod->type == ResultModifierType::ORDER_MODIFIER) {
					for (auto &order : mod->Cast<OrderModifier>().orders) {
						row_number->OrderByMutable().emplace_back(order.type, order.null_order,
						                                          order.expression->Copy());
					}
				}
			}
			row_number->SetAlias(Identifier("__distinct_on"));
			inner.select_list.push_back(std::move(row_number));
			filter = WriteIdentifier(string("__distinct_on")) + " = 1";
			break;
		}
		default:
			throw InternalException("SQLiteSQLWriter: unsupported result modifier");
		}
	}
	if (inner.having && inner.groups.grouping_sets.empty()) {
		if (!filter.empty() || node.qualify) {
			throw SQLitePushdownNotSupported("HAVING without GROUP BY with QUALIFY / DISTINCT ON");
		}
		// the HAVING condition of an aggregate without groups is an aggregate itself
		auto having = SQLiteBooleanCondition(std::move(inner.having));
		having->SetAlias(Identifier("__having"));
		inner.select_list.push_back(std::move(having));
		filter = WriteIdentifier(string("__having"));
	}
	if (node.qualify) {
		if (!filter.empty()) {
			throw SQLitePushdownNotSupported("QUALIFY with DISTINCT ON");
		}
		auto qualify = SQLiteBooleanCondition(node.qualify->Copy());
		qualify->SetAlias(Identifier("__qualify"));
		inner.select_list.push_back(std::move(qualify));
		inner.qualify.reset();
		filter = WriteIdentifier(string("__qualify"));
	}
	// the ORDER BY of the outer query can only refer to the result columns
	vector<OrderByNode> orders;
	if (order_modifier) {
		for (auto &order : order_modifier->orders) {
			auto &expr = *order.expression;
			unique_ptr<ParsedExpression> key;
			if (SQLiteCapabilities::IsIntegerLiteral(expr) || expr.GetExpressionClass() == ExpressionClass::STAR) {
				key = expr.Copy();
			} else {
				for (idx_t i = 0; i < node.select_list.size() && !key; i++) {
					auto &entry = *node.select_list[i];
					bool is_name = expr.GetExpressionClass() == ExpressionClass::COLUMN_REF &&
					               !expr.Cast<ColumnRefExpression>().IsQualified() &&
					               StringUtil::CIEquals(
					                   expr.Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName(), names[i]);
					if (is_name || entry.Equals(expr)) {
						key = make_uniq<ColumnRefExpression>(Identifier(names[i]));
					}
				}
			}
			if (!key) {
				throw SQLitePushdownNotSupported("ORDER BY of an expression that is not a result column with QUALIFY / "
				                                 "DISTINCT ON");
			}
			orders.emplace_back(order.type, order.null_order, std::move(key));
		}
	}
	string result = WriteCTEMap(node.cte_map);
	result += distinct ? "SELECT DISTINCT " : "SELECT ";
	for (idx_t i = 0; i < names.size(); i++) {
		result += (i > 0 ? ", " : "") + WriteIdentifier(names[i]);
	}
	result += " FROM (" + WriteExpandedSelectNode(inner) + ") WHERE " + filter;
	if (!orders.empty()) {
		result += " ORDER BY " + WriteOrderList(orders, &node);
	}
	if (limit_modifier) {
		result += " LIMIT " + (limit_modifier->limit ? WriteExpression(*limit_modifier->limit) : string("-1"));
		if (limit_modifier->offset) {
			result += " OFFSET " + WriteExpression(*limit_modifier->offset);
		}
	}
	return result;
}

unique_ptr<QueryNode> SQLiteSQLWriter::ExpandSelectAliases(const SelectNode &node) {
	case_insensitive_map_t<idx_t> aliases;
	for (idx_t i = 0; i < node.select_list.size(); i++) {
		auto &alias = node.select_list[i]->GetAlias();
		if (!alias.empty() && aliases.find(alias.GetIdentifierName()) == aliases.end()) {
			aliases[alias.GetIdentifierName()] = i;
		}
	}
	if (aliases.empty() || !node.from_table) {
		return nullptr;
	}
	auto result = node.Copy();
	auto &select = result->Cast<SelectNode>();
	// DuckDB resolves a name as an alias when it is not a column of the FROM clause - except in the HAVING clause,
	// where an alias takes precedence over the columns that are not grouped by
	case_insensitive_map_t<bool> is_column;
	auto find_alias = [&](const string &name, bool alias_first) -> optional_idx {
		auto entry = aliases.find(name);
		if (entry == aliases.end()) {
			return optional_idx();
		}
		if (alias_first) {
			return entry->second;
		}
		auto column_entry = is_column.find(name);
		if (column_entry == is_column.end()) {
			column_entry = is_column.emplace(name, ProvidesColumn(*node.from_table, Identifier(name))).first;
		}
		return column_entry->second ? optional_idx() : optional_idx(entry->second);
	};
	bool changed = false;
	std::function<void(unique_ptr<ParsedExpression> &, idx_t, bool)> expand = [&](unique_ptr<ParsedExpression> &expr,
	                                                                              idx_t limit, bool alias_first) {
		if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &column_ref = expr->Cast<ColumnRefExpression>();
			if (column_ref.IsQualified()) {
				return;
			}
			auto index = find_alias(column_ref.GetColumnName().GetIdentifierName(), alias_first);
			if (!index.IsValid() || index.GetIndex() >= limit) {
				return;
			}
			auto alias = expr->GetAlias();
			// the aliased expression has been expanded already
			expr = select.select_list[index.GetIndex()]->Copy();
			expr->SetAlias(alias);
			changed = true;
			return;
		}
		if (expr->GetExpressionClass() == ExpressionClass::SUBQUERY) {
			// subqueries do not resolve the aliases of the enclosing select list
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(
		    *expr, [&](unique_ptr<ParsedExpression> &child) { expand(child, limit, alias_first); });
	};
	for (idx_t i = 0; i < select.select_list.size(); i++) {
		auto &entry = select.select_list[i];
		if (entry->GetExpressionClass() == ExpressionClass::STAR) {
			continue;
		}
		auto name = SQLiteSelectEntryName(*entry);
		auto was_changed = changed;
		changed = false;
		expand(entry, i, false);
		if (changed && entry->GetAlias().empty()) {
			// the result column keeps its name
			entry->SetAlias(Identifier(name));
		}
		changed = changed || was_changed;
	}
	auto all_items = select.select_list.size();
	if (select.where_clause) {
		expand(select.where_clause, all_items, false);
	}
	for (auto &group : select.groups.group_expressions) {
		if (group->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			expand(group, all_items, false);
			continue;
		}
		// DuckDB does not resolve aliases within GROUP BY expressions
		auto copy = group->Copy();
		auto was_changed = changed;
		changed = false;
		expand(copy, all_items, false);
		if (changed) {
			throw SQLitePushdownNotSupported("alias reference within a GROUP BY expression");
		}
		changed = was_changed;
	}
	if (select.having) {
		// a GROUP BY column takes precedence over an alias
		case_insensitive_set_t group_columns;
		for (auto &group : select.groups.group_expressions) {
			optional_ptr<ParsedExpression> group_expr = group.get();
			int64_t position;
			if (SQLiteCapabilities::TryGetIntegerLiteral(*group, position) && position >= 1 &&
			    idx_t(position) <= select.select_list.size()) {
				// GROUP BY 1
				group_expr = select.select_list[NumericCast<idx_t>(position - 1)].get();
			}
			if (group_expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
				group_columns.insert(group_expr->Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName());
			}
		}
		for (auto &name : group_columns) {
			aliases.erase(name);
		}
		expand(select.having, all_items, true);
	}
	if (select.qualify) {
		expand(select.qualify, all_items, false);
	}
	if (!changed) {
		return nullptr;
	}
	return result;
}

string SQLiteSQLWriter::WriteSelectNode(const SelectNode &node) {
	auto expanded = ExpandSelectAliases(node);
	if (expanded) {
		owned_nodes.push_back(std::move(expanded));
		return WriteExpandedSelectNode(owned_nodes.back()->Cast<SelectNode>());
	}
	return WriteExpandedSelectNode(node);
}

string SQLiteSQLWriter::WriteExpandedSelectNode(const SelectNode &node) {
	if (node.qualify || (node.having && node.groups.grouping_sets.empty())) {
		// SQLite 3.38 requires a GROUP BY for a HAVING
		return WriteFilteredSelectNode(node);
	}
	for (auto &modifier : node.modifiers) {
		if (modifier->type == ResultModifierType::DISTINCT_MODIFIER &&
		    !modifier->Cast<DistinctModifier>().distinct_on_targets.empty()) {
			return WriteFilteredSelectNode(node);
		}
	}
	string result = WriteCTEMap(node.cte_map);
	// whether the select list expands the columns of the FROM clause in their order
	auto previous_unqualified_star = unqualified_star;
	unqualified_star = false;
	for (auto &expr : node.select_list) {
		if (expr->GetExpressionClass() == ExpressionClass::STAR &&
		    expr->Cast<StarExpression>().RelationName().empty()) {
			unqualified_star = true;
		}
	}
	table_scopes.emplace_back();
	if (node.from_table) {
		table_scopes.back().push_back(node.from_table);
	}
	result += "SELECT ";
	for (auto &modifier : node.modifiers) {
		if (modifier->type == ResultModifierType::DISTINCT_MODIFIER) {
			result += "DISTINCT ";
			break;
		}
	}
	for (idx_t i = 0; i < node.select_list.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		auto &select_expr = *node.select_list[i];
		result += WriteExpression(select_expr);
		if (!select_expr.GetAlias().empty()) {
			result += " AS " + WriteIdentifier(select_expr.GetAlias());
		} else if (select_expr.GetExpressionClass() != ExpressionClass::STAR &&
		           select_expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			// name the column the way DuckDB does - so that references to the column by name work
			result += " AS " + WriteIdentifier(select_expr.ToString());
		}
	}
	if (node.from_table && node.from_table->type != TableReferenceType::EMPTY_FROM) {
		result += " FROM " + WriteTableRef(*node.from_table);
	}
	if (node.where_clause) {
		result += " WHERE " + WriteCondition(*node.where_clause);
	}
	if (!node.groups.grouping_sets.empty()) {
		auto &grouping_set = node.groups.grouping_sets[0];
		if (grouping_set.empty()) {
			throw SQLitePushdownNotSupported("GROUP BY ()");
		}
		result += " GROUP BY ";
		bool first = true;
		for (auto &group_index : grouping_set) {
			if (!first) {
				result += ", ";
			}
			result += WriteOrderingKey(*node.groups.group_expressions[group_index]);
			first = false;
		}
	}
	if (node.having) {
		result += " HAVING " + WriteCondition(*node.having);
	}
	if (node.sample) {
		throw InternalException("SQLiteSQLWriter: SAMPLE should have been blocked by SupportsPushdown");
	}
	result += WriteResultModifiers(node);
	table_scopes.pop_back();
	unqualified_star = previous_unqualified_star;
	return result;
}

string SQLiteSQLWriter::WriteSetOperationChild(const QueryNode &node) {
	bool inline_child = node.type == QueryNodeType::SELECT_NODE && node.cte_map.map.empty();
	for (auto &modifier : node.modifiers) {
		if (modifier->type != ResultModifierType::DISTINCT_MODIFIER) {
			inline_child = false;
		}
	}
	if (inline_child) {
		return WriteQueryNode(node);
	}
	// the members of a compound SELECT cannot have an ORDER BY / LIMIT, and nested compound SELECTs would be
	// evaluated left to right
	return "SELECT * FROM (" + WriteQueryNode(node) + ")";
}

unique_ptr<QueryNode> SQLiteSQLWriter::CastSetOperationChild(const QueryNode &node, const vector<LogicalType> &types) {
	vector<string> names;
	auto child_types = InferQueryTypes(node, &names);
	if (child_types.size() != types.size()) {
		throw SQLitePushdownNotSupported("cannot determine the types of a set operation");
	}
	if (child_types == types) {
		return nullptr;
	}
	// SQLite does not convert the values of the members of a set operation to a common type (e.g. the integer 42 and
	// the string '42' are different values)
	auto result = node.Copy();
	switch (result->type) {
	case QueryNodeType::SELECT_NODE: {
		auto &select = result->Cast<SelectNode>();
		if (select.select_list.size() != types.size()) {
			throw SQLitePushdownNotSupported("set operation member with a star whose types are converted");
		}
		for (idx_t i = 0; i < types.size(); i++) {
			if (child_types[i] == types[i]) {
				continue;
			}
			auto &entry = select.select_list[i];
			if (entry->GetExpressionClass() == ExpressionClass::STAR) {
				throw SQLitePushdownNotSupported("set operation member with a star whose types are converted");
			}
			auto alias = entry->GetAlias().empty() ? Identifier(names[i]) : entry->GetAlias();
			entry = make_uniq_base<ParsedExpression, CastExpression>(types[i], std::move(entry));
			entry->SetAlias(alias);
		}
		return result;
	}
	case QueryNodeType::SET_OPERATION_NODE: {
		auto &setop = result->Cast<SetOperationNode>();
		for (auto &child : setop.children) {
			auto cast_child = CastSetOperationChild(*child, types);
			if (cast_child) {
				child = std::move(cast_child);
			}
		}
		return result;
	}
	default:
		throw SQLitePushdownNotSupported("set operation member whose types are converted");
	}
}

string SQLiteSQLWriter::WriteSetOperationNode(const SetOperationNode &node) {
	auto types = InferQueryTypes(node);
	if (types.empty()) {
		throw SQLitePushdownNotSupported("cannot determine the types of a set operation");
	}
	string result = WriteCTEMap(node.cte_map);
	for (idx_t i = 0; i < node.children.size(); i++) {
		if (i > 0) {
			switch (node.setop_type) {
			case SetOperationType::UNION:
				result += node.setop_all ? " UNION ALL " : " UNION ";
				break;
			case SetOperationType::EXCEPT:
				result += " EXCEPT ";
				break;
			case SetOperationType::INTERSECT:
				result += " INTERSECT ";
				break;
			default:
				throw InternalException("SQLiteSQLWriter: unsupported set operation - should have been blocked by "
				                        "SupportsPushdown");
			}
		}
		auto cast_child = CastSetOperationChild(*node.children[i], types);
		if (cast_child) {
			owned_nodes.push_back(std::move(cast_child));
			result += WriteSetOperationChild(*owned_nodes.back());
		} else {
			result += WriteSetOperationChild(*node.children[i]);
		}
	}
	result += WriteResultModifiers(node);
	return result;
}

string SQLiteSQLWriter::WriteRecursiveCTENode(const RecursiveCTENode &node) {
	string result = WriteCTEMap(node.cte_map);
	result += WriteSetOperationChild(*node.left);
	result += node.union_all ? " UNION ALL " : " UNION ";
	// the rows of the recursive member are converted to the types of the non-recursive member
	auto types = InferQueryTypes(*node.left);
	if (types.empty()) {
		throw SQLitePushdownNotSupported("cannot determine the types of a recursive CTE");
	}
	auto cast_right = CastSetOperationChild(*node.right, types);
	if (cast_right) {
		owned_nodes.push_back(std::move(cast_right));
		result += WriteSetOperationChild(*owned_nodes.back());
	} else {
		result += WriteSetOperationChild(*node.right);
	}
	return result;
}

string SQLiteSQLWriter::WriteQueryNode(const QueryNode &node) {
	bool has_ctes = !node.cte_map.map.empty();
	if (has_ctes) {
		cte_scopes.push_back(node.cte_map);
	}
	string result;
	switch (node.type) {
	case QueryNodeType::SELECT_NODE:
		result = WriteSelectNode(node.Cast<SelectNode>());
		break;
	case QueryNodeType::SET_OPERATION_NODE:
		result = WriteSetOperationNode(node.Cast<SetOperationNode>());
		break;
	case QueryNodeType::RECURSIVE_CTE_NODE:
		result = WriteRecursiveCTENode(node.Cast<RecursiveCTENode>());
		break;
	case QueryNodeType::UPDATE_QUERY_NODE:
		result = WriteUpdateNode(node.Cast<UpdateQueryNode>());
		break;
	case QueryNodeType::DELETE_QUERY_NODE:
		result = WriteDeleteNode(node.Cast<DeleteQueryNode>());
		break;
	case QueryNodeType::INSERT_QUERY_NODE:
		result = WriteInsertNode(node.Cast<InsertQueryNode>());
		break;
	default:
		throw InternalException("SQLiteSQLWriter: unsupported query node type - should have been blocked by "
		                        "SupportsPushdown");
	}
	if (has_ctes) {
		cte_scopes.pop_back();
	}
	return result;
}

//===--------------------------------------------------------------------===//
// DML
//===--------------------------------------------------------------------===//
SQLiteTableEntry &SQLiteSQLWriter::GetTargetTable(const TableRef &ref) {
	if (ref.type != TableReferenceType::BASE_TABLE || IsCTEReference(ref.Cast<BaseTableRef>())) {
		throw SQLitePushdownNotSupported("unsupported DML target");
	}
	auto table = LookupTable(ref.Cast<BaseTableRef>());
	if (!table) {
		throw SQLitePushdownNotSupported("DML target table not found");
	}
	return *table;
}

string SQLiteSQLWriter::WriteAssignment(const ParsedExpression &expr, const ColumnDefinition &column) {
	if (expr.GetExpressionClass() == ExpressionClass::DEFAULT) {
		throw SQLitePushdownNotSupported("SQLite has no DEFAULT values in assignments");
	}
	// DuckDB converts the value to the type of the column - and stores it as the SQLite representation of that type
	return WriteAs(expr, column.Type());
}

string SQLiteSQLWriter::WriteSetClause(const UpdateSetInfo &set_info, SQLiteTableEntry &table) {
	string result;
	for (idx_t i = 0; i < set_info.columns.size(); i++) {
		auto &column_name = set_info.columns[i];
		if (!table.ColumnExists(column_name)) {
			throw SQLitePushdownNotSupported("column not found");
		}
		auto &column = table.GetColumn(column_name);
		result += (i > 0 ? ", " : "") + WriteIdentifier(column.Name()) + " = " +
		          WriteAssignment(*set_info.expressions[i], column);
	}
	return result;
}

string SQLiteSQLWriter::WriteReturningList(const vector<unique_ptr<ParsedExpression>> &returning_list,
                                           SQLiteTableEntry &table) {
	if (returning_list.empty()) {
		return string();
	}
	// SQLite's RETURNING can only refer to the columns of the target table, without qualification
	unqualified_columns = true;
	string result = " RETURNING ";
	for (idx_t i = 0; i < returning_list.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		auto &expr = *returning_list[i];
		if (expr.GetExpressionClass() == ExpressionClass::STAR) {
			auto &star = expr.Cast<StarExpression>();
			if (!star.RelationName().empty() || !star.ExcludeList().empty() || !star.ReplaceList().empty() ||
			    !star.RenameList().empty() || star.Expression()) {
				throw SQLitePushdownNotSupported("unsupported RETURNING star");
			}
			// the columns are converted the way the table scan converts them
			idx_t col = 0;
			for (auto &column : table.GetColumns().Logical()) {
				auto column_name = WriteIdentifier(column.Name());
				auto read = SQLiteColumnRead(table, column, column_name);
				if (!read.empty() &&
				    (column.Type().id() == LogicalTypeId::DATE || column.Type().id() == LogicalTypeId::TIMESTAMP)) {
					RequireExtensionFunctions("reading a " + column.Type().ToString() + " column");
				}
				result += (col > 0 ? ", " : "") + (read.empty() ? column_name : read + " AS " + column_name);
				col++;
			}
			continue;
		}
		result += WriteExpression(expr);
		if (!expr.GetAlias().empty()) {
			result += " AS " + WriteIdentifier(expr.GetAlias());
		} else if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			result += " AS " + WriteIdentifier(expr.ToString());
		}
	}
	unqualified_columns = false;
	return result;
}

static string SQLiteWriteTarget(SQLiteTableEntry &table, const Identifier &alias) {
	auto result = SQLiteUtils::WriteQuotedAndEscaped(table.name.GetIdentifierName(), '"');
	if (!alias.empty()) {
		result += " AS " + SQLiteUtils::WriteQuotedAndEscaped(alias.GetIdentifierName(), '"');
	}
	return result;
}

string SQLiteSQLWriter::WriteUpdateNode(const UpdateQueryNode &node) {
	auto &table = GetTargetTable(*node.table);
	string result = WriteCTEMap(node.cte_map);
	table_scopes.emplace_back();
	table_scopes.back().push_back(node.table);
	if (node.from_table) {
		table_scopes.back().push_back(node.from_table);
	}
	dml_target = &table;
	dml_bindings = {node.table->alias.empty() ? table.name : node.table->alias};
	dml_scope = table_scopes.size();
	result += "UPDATE " + SQLiteWriteTarget(table, node.table->alias);
	result += " SET " + WriteSetClause(*node.set_info, table);
	if (node.from_table) {
		result += " FROM " + WriteTableRef(*node.from_table);
	}
	if (node.set_info->condition) {
		result += " WHERE " + WriteCondition(*node.set_info->condition);
	}
	if (!node.returning_list.empty() && node.from_table) {
		throw SQLitePushdownNotSupported("RETURNING in an UPDATE with a FROM clause");
	}
	result += WriteReturningList(node.returning_list, table);
	dml_target = nullptr;
	table_scopes.pop_back();
	return result;
}

string SQLiteSQLWriter::WriteDeleteNode(const DeleteQueryNode &node) {
	auto &table = GetTargetTable(*node.table);
	string result = WriteCTEMap(node.cte_map);
	table_scopes.emplace_back();
	table_scopes.back().push_back(node.table);
	dml_target = &table;
	dml_bindings = {node.table->alias.empty() ? table.name : node.table->alias};
	dml_scope = table_scopes.size();
	result += "DELETE FROM " + SQLiteWriteTarget(table, node.table->alias);
	if (!node.using_clauses.empty()) {
		// SQLite has no DELETE ... USING - a row is deleted if there is a row in the USING tables that it matches
		if (!node.returning_list.empty()) {
			throw SQLitePushdownNotSupported("RETURNING in a DELETE with a USING clause");
		}
		table_scopes.emplace_back();
		for (auto &using_clause : node.using_clauses) {
			table_scopes.back().push_back(using_clause);
		}
		result += " WHERE EXISTS (SELECT 1 FROM ";
		for (idx_t i = 0; i < node.using_clauses.size(); i++) {
			result += (i > 0 ? ", " : "") + WriteTableRef(*node.using_clauses[i]);
		}
		if (node.condition) {
			result += " WHERE " + WriteCondition(*node.condition);
		}
		result += ")";
		table_scopes.pop_back();
	} else if (node.condition) {
		result += " WHERE " + WriteCondition(*node.condition);
	}
	result += WriteReturningList(node.returning_list, table);
	dml_target = nullptr;
	table_scopes.pop_back();
	return result;
}

string SQLiteSQLWriter::WriteInsertNode(const InsertQueryNode &node) {
	auto table_ptr = LookupTable(node.qualified_name);
	if (!table_ptr) {
		throw SQLitePushdownNotSupported("INSERT target table not found");
	}
	auto &table = *table_ptr;
	vector<reference<const ColumnDefinition>> columns;
	if (node.columns.empty()) {
		for (auto &column : table.GetColumns().Physical()) {
			columns.push_back(column);
		}
	} else {
		for (auto &column_name : node.columns) {
			if (!table.ColumnExists(column_name)) {
				throw SQLitePushdownNotSupported("INSERT column not found");
			}
			columns.push_back(table.GetColumn(column_name));
		}
	}
	auto values_list = node.GetValuesList();
	// SQLite has no DEFAULT in a VALUES list - columns that are DEFAULT in every row are not inserted into
	vector<bool> insert_column(columns.size(), true);
	if (values_list) {
		for (idx_t col = 0; col < columns.size(); col++) {
			bool all_default = true;
			bool any_default = false;
			for (auto &row : values_list->values) {
				if (row.size() != columns.size()) {
					throw SQLitePushdownNotSupported("INSERT column count mismatch");
				}
				bool is_default = row[col]->GetExpressionClass() == ExpressionClass::DEFAULT;
				all_default = all_default && is_default;
				any_default = any_default || is_default;
			}
			if (any_default && !all_default) {
				throw SQLitePushdownNotSupported("INSERT of DEFAULT values for some rows only");
			}
			insert_column[col] = !all_default;
		}
	}
	string ctes = WriteCTEMap(node.cte_map);
	string target = "INSERT INTO " + SQLiteWriteTarget(table, node.table_ref ? node.table_ref->alias : Identifier());
	string column_list;
	for (idx_t i = 0; i < columns.size(); i++) {
		if (insert_column[i]) {
			column_list += (column_list.empty() ? "" : ", ") + WriteIdentifier(columns[i].get().Name());
		}
	}
	string source;
	if (node.default_values || (values_list && column_list.empty())) {
		if (values_list && values_list->values.size() != 1) {
			throw SQLitePushdownNotSupported("INSERT of multiple rows of default values");
		}
		source = " DEFAULT VALUES";
	} else if (values_list) {
		// every value is converted to the type of its column
		table_scopes.emplace_back();
		vector<string> rows;
		for (auto &row : values_list->values) {
			string row_str;
			for (idx_t col = 0; col < row.size(); col++) {
				if (!insert_column[col]) {
					continue;
				}
				row_str += (row_str.empty() ? "" : ", ") + WriteAssignment(*row[col], columns[col]);
			}
			rows.push_back("(" + row_str + ")");
		}
		table_scopes.pop_back();
		source = " (" + column_list + ") VALUES " + StringUtil::Join(rows, ", ");
	} else {
		auto &select = *node.select_statement->node;
		auto source_types = InferQueryTypes(select);
		if (source_types.size() != columns.size()) {
			throw SQLitePushdownNotSupported("cannot determine the types of the inserted values");
		}
		bool requires_conversion = false;
		for (idx_t i = 0; i < columns.size(); i++) {
			requires_conversion = requires_conversion || source_types[i] != columns[i].get().Type();
		}
		auto select_str = WriteQueryNode(select);
		if (!requires_conversion) {
			source = " (" + column_list + ") SELECT * FROM (" + select_str + ")";
		} else {
			// DuckDB converts the inserted values to the types of the columns
			string select_list;
			string aliases;
			for (idx_t i = 0; i < columns.size(); i++) {
				auto name = WriteIdentifier("c" + to_string(i));
				select_list += (i > 0 ? ", " : "") + WriteCast(name, source_types[i], columns[i].get().Type());
				aliases += (i > 0 ? ", " : "") + name;
			}
			auto values_name = WriteIdentifier(string("insert_values"));
			source = " (" + column_list + ") SELECT " + select_list + " FROM (WITH " + values_name + "(" + aliases +
			         ") AS (" + select_str + ") SELECT * FROM " + values_name + ")";
		}
		if (node.on_conflict_info) {
			// "INSERT INTO ... SELECT ... ON CONFLICT" is ambiguous in SQLite without a WHERE clause
			source += " WHERE 1";
		}
	}
	string result = ctes + target + source;
	if (node.on_conflict_info) {
		result += WriteOnConflict(*node.on_conflict_info, table, node.table_ref ? node.table_ref->alias : Identifier(),
		                          columns, values_list.get());
	}
	auto target_ref = make_uniq<BaseTableRef>();
	target_ref->SetQualifiedName(QualifiedName(Identifier(), table.ParentSchema().name, table.name));
	if (node.table_ref) {
		target_ref->alias = node.table_ref->alias;
	}
	unique_ptr<TableRef> target_table = std::move(target_ref);
	table_scopes.emplace_back();
	table_scopes.back().push_back(target_table);
	dml_target = &table;
	dml_bindings = {node.table_ref && !node.table_ref->alias.empty() ? node.table_ref->alias : table.name};
	dml_scope = table_scopes.size();
	result += WriteReturningList(node.returning_list, table);
	dml_target = nullptr;
	table_scopes.pop_back();
	return result;
}

string SQLiteSQLWriter::WriteOnConflict(const OnConflictInfo &info, SQLiteTableEntry &table,
                                        const Identifier &target_binding,
                                        const vector<reference<const ColumnDefinition>> &columns,
                                        optional_ptr<const ExpressionListRef> values_list) {
	if (info.action_type == OnConflictAction::THROW) {
		return string();
	}
	// the conflict target - DuckDB defaults to the primary key
	vector<string> conflict_columns;
	for (auto &column_name : info.indexed_columns) {
		if (!table.ColumnExists(column_name)) {
			throw SQLitePushdownNotSupported("ON CONFLICT column not found");
		}
		conflict_columns.push_back(WriteIdentifier(table.GetColumn(column_name).Name()));
	}
	if (conflict_columns.empty() && info.action_type != OnConflictAction::NOTHING) {
		for (auto &constraint : table.GetConstraints()) {
			if (constraint->type != ConstraintType::UNIQUE) {
				continue;
			}
			auto &unique = constraint->Cast<UniqueConstraint>();
			if (!unique.IsPrimaryKey()) {
				continue;
			}
			if (unique.HasIndex()) {
				conflict_columns.push_back(WriteIdentifier(table.GetColumn(unique.GetIndex()).Name()));
			} else {
				for (auto &column_name : unique.GetColumnNames()) {
					conflict_columns.push_back(WriteIdentifier(table.GetColumn(column_name).Name()));
				}
			}
		}
		if (conflict_columns.empty()) {
			throw SQLitePushdownNotSupported("ON CONFLICT DO UPDATE without a primary key");
		}
	}
	string result = " ON CONFLICT";
	if (!conflict_columns.empty()) {
		result += " (" + StringUtil::Join(conflict_columns, ", ") + ")";
	}
	if (info.action_type == OnConflictAction::NOTHING) {
		return result + " DO NOTHING";
	}
	// DuckDB raises an error when a row is updated twice by the same statement, SQLite updates it twice - only push
	// down VALUES lists where the values of the conflict target are constants that are distinct
	if (!values_list) {
		throw SQLitePushdownNotSupported("ON CONFLICT DO UPDATE with a query");
	}
	vector<idx_t> key_indexes;
	for (auto &conflict_column : conflict_columns) {
		bool found = false;
		for (idx_t i = 0; i < columns.size(); i++) {
			if (WriteIdentifier(columns[i].get().Name()) == conflict_column) {
				key_indexes.push_back(i);
				found = true;
			}
		}
		if (!found) {
			throw SQLitePushdownNotSupported("ON CONFLICT column is not inserted");
		}
	}
	vector<vector<Value>> keys;
	for (auto &row : values_list->values) {
		vector<Value> key;
		for (auto index : key_indexes) {
			Value value;
			if (index >= row.size() || !SQLiteCapabilities::TryGetConstantValue(*row[index], value)) {
				throw SQLitePushdownNotSupported("ON CONFLICT DO UPDATE with non-constant keys");
			}
			auto cast_value = value.TryCastAs(context, columns[index].get().Type());
			if (!cast_value) {
				throw SQLitePushdownNotSupported("ON CONFLICT DO UPDATE with invalid keys");
			}
			key.push_back(std::move(*cast_value));
		}
		for (auto &other : keys) {
			bool equal = true;
			for (idx_t k = 0; k < key.size(); k++) {
				equal = equal && !key[k].IsNull() && Value::NotDistinctFrom(key[k], other[k]);
			}
			if (equal) {
				throw SQLitePushdownNotSupported("ON CONFLICT DO UPDATE with duplicate keys");
			}
		}
		keys.push_back(std::move(key));
	}
	if (info.action_type == OnConflictAction::REPLACE) {
		// INSERT OR REPLACE updates all inserted columns that are not part of the conflict target
		string set_clause;
		for (auto &column : columns) {
			auto name = WriteIdentifier(column.get().Name());
			bool is_conflict_column = false;
			for (auto &conflict_column : conflict_columns) {
				is_conflict_column = is_conflict_column || conflict_column == name;
			}
			if (is_conflict_column) {
				continue;
			}
			set_clause +=
			    (set_clause.empty() ? "" : ", ") + name + " = " + WriteIdentifier(string("excluded")) + "." + name;
		}
		if (set_clause.empty()) {
			return result + " DO NOTHING";
		}
		return result + " DO UPDATE SET " + set_clause;
	}
	// DO UPDATE - the SET expressions can reference the target table and the excluded values
	auto target_ref = make_uniq<BaseTableRef>();
	target_ref->SetQualifiedName(QualifiedName(Identifier(), table.ParentSchema().name, table.name));
	target_ref->alias = target_binding;
	auto excluded_ref = make_uniq<BaseTableRef>();
	excluded_ref->SetQualifiedName(QualifiedName(Identifier(), table.ParentSchema().name, table.name));
	excluded_ref->alias = Identifier("excluded");
	unique_ptr<TableRef> excluded_table = std::move(excluded_ref);
	unique_ptr<TableRef> target_table = std::move(target_ref);
	table_scopes.emplace_back();
	table_scopes.back().push_back(target_table);
	table_scopes.back().push_back(excluded_table);
	dml_target = &table;
	dml_bindings = {target_binding.empty() ? table.name : target_binding, Identifier("excluded")};
	dml_scope = table_scopes.size();
	result += " DO UPDATE SET " + WriteSetClause(*info.set_info, table);
	if (info.set_info->condition) {
		result += " WHERE " + WriteCondition(*info.set_info->condition);
	}
	dml_target = nullptr;
	table_scopes.pop_back();
	return result;
}

//===--------------------------------------------------------------------===//
// Statements
//===--------------------------------------------------------------------===//
SQLiteWriterResult SQLiteSQLWriter::WriteStatement(ClientContext &context, SQLiteCatalog &catalog,
                                                   SQLStatement &statement) {
	SQLiteWriterResult result;
	// all statements modify the catalog
	result.clear_catalog_cache = true;
	switch (statement.type) {
	case StatementType::CREATE_STATEMENT: {
		auto &create = statement.Cast<CreateStatement>();
		switch (create.info->type) {
		case CatalogType::TABLE_ENTRY: {
			SQLiteSQLWriter writer(context, catalog, false);
			writer.WriteCreateTable(create, result);
			break;
		}
		case CatalogType::VIEW_ENTRY: {
			SQLiteSQLWriter writer(context, catalog, true);
			writer.WriteCreateView(create.info->Cast<CreateViewInfo>(), result);
			break;
		}
		default:
			throw InternalException("SQLiteSQLWriter: unsupported CREATE statement - should have been blocked by "
			                        "SupportsPushdown");
		}
		break;
	}
	case StatementType::DROP_STATEMENT: {
		SQLiteSQLWriter writer(context, catalog, true);
		result.sql = writer.WriteDrop(*statement.Cast<DropStatement>().info);
		break;
	}
	case StatementType::ALTER_STATEMENT: {
		SQLiteSQLWriter writer(context, catalog, true);
		result.sql = writer.WriteAlter(*statement.Cast<AlterStatement>().info);
		break;
	}
	default:
		throw InternalException("SQLiteSQLWriter: unsupported statement type - should have been blocked by "
		                        "SupportsPushdown");
	}
	return result;
}

static optional_ptr<LogicalCreateTable> SQLiteFindCreateTable(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_CREATE_TABLE) {
		return op.Cast<LogicalCreateTable>();
	}
	for (auto &child : op.children) {
		auto result = SQLiteFindCreateTable(*child);
		if (result) {
			return result;
		}
	}
	return nullptr;
}

static QualifiedName SQLiteQualifyName(const QualifiedName &name, const Identifier &catalog_name) {
	vector<Identifier> schema_path;
	if (!name.Schema().empty()) {
		schema_path.push_back(name.Schema());
	}
	return QualifiedName::FromCatalogSchema(catalog_name, std::move(schema_path), name.Name());
}

void SQLiteSQLWriter::WriteCreateTable(CreateStatement &statement, SQLiteWriterResult &result) {
	auto &info = statement.info->Cast<CreateTableInfo>();
	if (!info.query) {
		throw InternalException("SQLiteSQLWriter: CREATE TABLE without a query should have been blocked by "
		                        "SupportsPushdown");
	}
	// bind the statement to resolve the column types of the query
	auto bind_statement = statement.Copy();
	auto &bind_info = bind_statement->Cast<CreateStatement>().info->Cast<CreateTableInfo>();
	bind_info.SetQualifiedName(SQLiteQualifyName(info.GetQualifiedName(), catalog.GetName()));
	identifier_set_t cte_names;
	CollectCTENames(*bind_info.query->node, cte_names);
	QualifyTables(*bind_info.query->node, catalog.GetName(), cte_names);
	auto binder = Binder::CreateBinder(context);
	auto bound = binder->Bind(*bind_statement);
	auto create_table = SQLiteFindCreateTable(*bound.plan);
	if (!create_table) {
		throw InternalException("SQLiteSQLWriter: CREATE TABLE statement was not bound to a LogicalCreateTable");
	}
	auto &bound_info = create_table->info->Base();
	vector<LogicalType> source_types;
	for (auto &column : bound_info.columns.Logical()) {
		source_types.push_back(column.Type());
	}
	auto table_name = WriteIdentifier(bound_info.GetTableName());
	if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		result.preliminary_queries.push_back("DROP TABLE IF EXISTS " + table_name);
	}
	// the same CREATE TABLE statement as a regular CREATE TABLE AS - which converts the column types
	bound_info.on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
	auto create_sql = GetCreateTableSQL(bound_info);
	if (StringUtil::EndsWith(create_sql, ";")) {
		create_sql = create_sql.substr(0, create_sql.size() - 1);
	}
	result.preliminary_queries.push_back(std::move(create_sql));
	// fill the table with the result of the query - converted to the column types
	CollectColumnReferences(*info.query->node);
	auto query_str = WriteQueryNode(*info.query->node);
	{
		// verify that SQLite accepts the query - the table does not exist yet, so the statement cannot be prepared
		auto &db = SQLiteTransaction::Get(context, catalog).GetDB();
		string error;
		if (!db.TryPrepareStrict(query_str, error)) {
			throw SQLitePushdownNotSupported("SQLite cannot prepare the query: " + error);
		}
	}
	string columns;
	string select_list;
	string aliases;
	idx_t index = 0;
	for (auto &column : bound_info.columns.Logical()) {
		auto alias = WriteIdentifier("c" + to_string(index));
		columns += (index > 0 ? ", " : "") + WriteIdentifier(column.Name());
		select_list += (index > 0 ? ", " : "") + WriteCast(alias, source_types[index], column.Type());
		aliases += (index > 0 ? ", " : "") + alias;
		index++;
	}
	auto values_name = WriteIdentifier(string("insert_values"));
	result.sql = "INSERT INTO " + table_name + " (" + columns + ") SELECT " + select_list + " FROM (WITH " +
	             values_name + "(" + aliases + ") AS (" + query_str + ") SELECT * FROM " + values_name + ")";
	result.changed_rows = true;
}

void SQLiteSQLWriter::WriteCreateView(CreateViewInfo &info, SQLiteWriterResult &result) {
	auto view_name = WriteIdentifier(info.GetViewName());
	string query_sql;
	try {
		// the view is read back by DuckDB - it must bind its SQL to the same result types
		vector<LogicalType> view_types;
		{
			auto probe = info.query->node->Copy();
			identifier_set_t cte_names;
			CollectCTENames(*probe, cte_names);
			QualifyTables(*probe, catalog.GetName(), cte_names);
			auto binder = Binder::CreateBinder(context);
			view_types = binder->Bind(*probe).types;
		}
		CollectColumnReferences(*info.query->node);
		query_sql = WriteQueryNode(*info.query->node);
		Parser parser(context);
		parser.ParseQuery(query_sql);
		if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
			throw SQLitePushdownNotSupported("the view cannot be read back");
		}
		auto &read_back = *parser.statements[0]->Cast<SelectStatement>().node;
		identifier_set_t cte_names;
		CollectCTENames(read_back, cte_names);
		QualifyTables(read_back, catalog.GetName(), cte_names);
		auto binder = Binder::CreateBinder(context);
		if (binder->Bind(read_back).types != view_types) {
			throw SQLitePushdownNotSupported("the view is read back with different types");
		}
	} catch (std::exception &ex) {
		// store the view like a regular CREATE VIEW does
		if (SQLiteDB::DebugPrintQueries()) {
			Printer::Print("Remote pushdown: storing the SQL of the view as is, " + string(ex.what()));
		}
		query_sql = string();
	}
	if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		result.preliminary_queries.push_back("DROP VIEW IF EXISTS " + view_name);
	}
	if (query_sql.empty()) {
		auto native_info = unique_ptr_cast<CreateInfo, CreateViewInfo>(info.Copy());
		native_info->on_conflict = info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT
		                               ? OnCreateConflict::IGNORE_ON_CONFLICT
		                               : OnCreateConflict::ERROR_ON_CONFLICT;
		result.sql = GetCreateViewSQL(*native_info);
		return;
	}
	result.sql = "CREATE VIEW ";
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		result.sql += "IF NOT EXISTS ";
	}
	result.sql += view_name;
	if (!info.aliases.empty()) {
		result.sql += " (";
		for (idx_t i = 0; i < info.aliases.size(); i++) {
			result.sql += (i > 0 ? ", " : "") + WriteIdentifier(info.aliases[i]);
		}
		result.sql += ")";
	}
	result.sql += " AS " + query_sql;
}

void SQLiteSQLWriter::VerifyEntryExists(CatalogType type, const QualifiedName &name) {
	// raise the same error as the regular statement when the entry does not exist
	Identifier schema_name = name.Schema().empty() ? Identifier(DEFAULT_SCHEMA) : name.Schema();
	EntryLookupInfo lookup(type, QualifiedName(catalog.GetName(), schema_name, name.Name()));
	Catalog::GetEntry(context, lookup, OnEntryNotFound::THROW_EXCEPTION);
}

string SQLiteSQLWriter::WriteDrop(const DropInfo &info) {
	if (info.if_not_found != OnEntryNotFound::RETURN_NULL) {
		VerifyEntryExists(info.type, info.GetQualifiedName());
	}
	string result = "DROP ";
	switch (info.type) {
	case CatalogType::TABLE_ENTRY:
		result += "TABLE ";
		break;
	case CatalogType::VIEW_ENTRY:
		result += "VIEW ";
		break;
	case CatalogType::INDEX_ENTRY:
		result += "INDEX ";
		break;
	default:
		throw InternalException("SQLiteSQLWriter: unsupported DROP statement - should have been blocked by "
		                        "SupportsPushdown");
	}
	if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	return result + SQLiteUtils::WriteOptionallyQuoted(info.GetQualifiedName().Name().GetIdentifierName());
}

string SQLiteSQLWriter::WriteAlter(const AlterInfo &info) {
	auto &alter_info = info.Cast<AlterTableInfo>();
	auto table = LookupTable(alter_info.GetQualifiedName());
	if (!table) {
		if (alter_info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			// nothing to do
			return "SELECT 1";
		}
		VerifyEntryExists(CatalogType::TABLE_ENTRY, alter_info.GetQualifiedName());
		throw SQLitePushdownNotSupported("ALTER of a view");
	}
	string result = "ALTER TABLE " + SQLiteUtils::WriteOptionallyQuoted(table->name.GetIdentifierName());
	switch (alter_info.alter_table_type) {
	case AlterTableType::RENAME_TABLE:
		return result + " RENAME TO " +
		       SQLiteUtils::WriteOptionallyQuoted(
		           alter_info.Cast<RenameTableInfo>().new_table_name.GetIdentifierName());
	case AlterTableType::RENAME_COLUMN: {
		// like for a regular ALTER TABLE, SQLite raises the error for a column that does not exist
		auto &rename_info = alter_info.Cast<RenameColumnInfo>();
		return result + " RENAME COLUMN " +
		       SQLiteUtils::WriteOptionallyQuoted(rename_info.old_name.GetIdentifierName()) + " TO " +
		       SQLiteUtils::WriteOptionallyQuoted(rename_info.new_name.GetIdentifierName());
	}
	case AlterTableType::ADD_COLUMN: {
		auto &add_info = alter_info.Cast<AddColumnInfo>();
		auto &column = add_info.new_column;
		if (add_info.if_column_not_exists && table->ColumnExists(column.Name())) {
			return "SELECT 1";
		}
		LogicalType type = column.Type();
		if (type.id() == LogicalTypeId::UNBOUND) {
			type = SQLiteCapabilities::ResolveType(UnboundType::GetTypeExpression(type)->Cast<TypeExpression>());
		}
		// the column type is written like a regular ALTER TABLE writes it
		return result + " ADD COLUMN " + SQLiteUtils::WriteOptionallyQuoted(column.Name().GetIdentifierName()) + " " +
		       type.ToString();
	}
	case AlterTableType::REMOVE_COLUMN: {
		auto &remove_info = alter_info.Cast<RemoveColumnInfo>();
		if (remove_info.if_column_exists && !table->ColumnExists(remove_info.removed_column)) {
			return "SELECT 1";
		}
		return result + " DROP COLUMN " +
		       SQLiteUtils::WriteOptionallyQuoted(remove_info.removed_column.GetIdentifierName());
	}
	default:
		throw InternalException("SQLiteSQLWriter: unsupported ALTER statement - should have been blocked by "
		                        "SupportsPushdown");
	}
}

} // namespace duckdb
