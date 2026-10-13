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

class SQLiteUtils {
public:
	static void Check(int rc, sqlite3 *db);
	static string TypeToString(int sqlite_type);
	static LogicalType TypeToLogicalType(const string &sqlite_type);
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
};

struct RowIdInfo {
	optional_idx min_rowid;
	optional_idx max_rowid;
};

} // namespace duckdb
