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
	string ToSQLiteTypeAlias(const LogicalType &input);

	static string EscapeQuotes(const string &text, char quote);
	static string WriteQuotedAndEscaped(const string &text, char quote);
	static string WriteOptionallyQuoted(const string &text, char quote = '"', bool allow_caps = true);
};

struct RowIdInfo {
	optional_idx min_rowid;
	optional_idx max_rowid;
};

} // namespace duckdb
