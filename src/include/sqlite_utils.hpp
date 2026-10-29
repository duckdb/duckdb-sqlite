//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "sqlite3.h"

namespace duckdb {

//! A value as it is stored in SQLite - data and size are set for TEXT and BLOB values
struct SQLiteValue {
	int type = SQLITE_NULL;
	int64_t integer = 0;
	double real = 0;
	const char *data = nullptr;
	idx_t size = 0;
};

class SQLiteUtils {
public:
	static void Check(int rc, sqlite3 *db);
	static string TypeToString(int sqlite_type);
	static LogicalType TypeToLogicalType(const string &sqlite_type);
	//! Whether a column with this (lower case) declared type has TEXT affinity
	static bool HasTextAffinity(const string &sqlite_type);
	static string SanitizeString(const string &table_name);
	static string SanitizeIdentifier(const string &table_name);
	static LogicalType ToSQLiteType(const LogicalType &input);
	//! DuckDB ToString() operator name -> SQLite keyword, or nullptr if unchanged.
	static const char *ToSQLiteOperator(const string &duckdb_operator);
	string ToSQLiteTypeAlias(const LogicalType &input);

	static string EscapeQuotes(const string &text, char quote);
	static string WriteQuotedAndEscaped(const string &text, char quote);
	static string WriteOptionallyQuoted(const string &text, char quote = '"', bool allow_caps = true);

	//! The value of sqlite_all_varchar, false when it is not set or NULL
	static bool AllVarchar(ClientContext &context);

	//! Convert a SQLite value to a DuckDB type - integers and floats are read as unix timestamps for DATE and
	//! TIMESTAMP (like the table scan reads them), other conversions use DuckDB's casts
	static Value ToDuckDBValue(const SQLiteValue &value, const LogicalType &type);
	//! Read the value of a result column of a statement
	static SQLiteValue ReadColumn(sqlite3_stmt *stmt, int col);
	//! Read the value of a function argument
	static SQLiteValue ReadValue(sqlite3_value *value);
};

struct RowIdInfo {
	optional_idx min_rowid;
	optional_idx max_rowid;
};

} // namespace duckdb
