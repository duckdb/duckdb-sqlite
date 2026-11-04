//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_ducklake_profile.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {
class DatabaseInstance;

//! Registers with DuckLake how it should use a SQLite database as its metadata catalog
struct SQLiteDuckLakeProfile {
	//! Registers the profile now if DuckLake is loaded, and otherwise when it loads
	static void Register(DatabaseInstance &db);
};

} // namespace duckdb
