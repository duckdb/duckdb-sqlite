//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "sqlite3.h"

namespace duckdb {

//! Functions that are registered on the SQLite connections of databases attached with remote pushdown - queries that
//! are pushed down into SQLite use them where SQLite has no built-in function with the semantics of the DuckDB
//! function. All of them are named duckdb_<name>, and they can only be used by top-level SQL (not by the views or
//! triggers stored in a database file).
class SQLiteFunctions {
public:
	static void Register(sqlite3 *db);

	//! Convert a DuckDB value to the value that represents it in SQLite - the inverse of SQLiteUtils::ToDuckDBValue
	//! for the types that have a representation in pushed down queries. Throws if the value cannot be represented.
	static void SetResult(sqlite3_context *context, const Value &value);
	//! Whether values of a type can be represented in pushed down queries
	static bool HasRepresentation(const LogicalType &type);
	//! The text representation of DATE and TIMESTAMP values in pushed down queries, which orders like the values -
	//! returns false if the value cannot be represented this way (e.g. infinite values or years beyond 9999)
	static bool TryGetCanonicalText(const Value &value, string &result);
};

} // namespace duckdb
