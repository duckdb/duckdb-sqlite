//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/sqlite_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/access_mode.hpp"

namespace duckdb {

struct SQLiteOpenOptions {
	// access mode
	AccessMode access_mode = AccessMode::READ_WRITE;
	// busy time-out in ms
	idx_t busy_timeout = 5000;
	// journal mode (PRAGMA journal_mode), validated against the SQLite keyword list
	string journal_mode;
	// synchronous setting (PRAGMA synchronous), validated against the SQLite keyword list
	string synchronous;
	// keep one handle open for the lifetime of an attached on-disk database (sqlite_keep_database_open)
	bool keep_database_open = false;
	// push down whole queries and statements into SQLite (sqlite_enable_remote_pushdown)
	bool remote_pushdown = false;

	//! Validate and normalise a journal_mode value; throws on anything SQLite would not accept
	static string ParseJournalMode(const string &value);
	//! Validate and normalise a synchronous value; throws on anything SQLite would not accept
	static string ParseSynchronous(const string &value);
};

} // namespace duckdb
