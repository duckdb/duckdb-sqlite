//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_stmt.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "sqlite_utils.hpp"

#include <cstddef>

namespace duckdb {
struct SqliteBindData;

class SQLiteStatement {
public:
	SQLiteStatement();
	SQLiteStatement(sqlite3 *db, sqlite3_stmt *stmt);
	~SQLiteStatement();
	// disable copy constructors
	SQLiteStatement(const SQLiteStatement &other) = delete;
	SQLiteStatement &operator=(const SQLiteStatement &) = delete;
	//! enable move constructors
	SQLiteStatement(SQLiteStatement &&other) noexcept;
	SQLiteStatement &operator=(SQLiteStatement &&) noexcept;

	sqlite3 *db = nullptr;
	sqlite3_stmt *stmt = nullptr;

public:
	int Step();
	template <class T>
	T GetValue(idx_t col) {
		throw InternalException("Unsupported type for SQLiteStatement::GetValue");
	}
	template <class T>
	void Bind(idx_t col, T value) {
		throw InternalException("Unsupported type for SQLiteStatement::Bind");
	}
	void BindText(idx_t col, const string_t &value);
	void BindText(idx_t col, const string &value);
	void BindBlob(idx_t col, const string_t &value);
	void BindBlob(idx_t col, const string &value);
	void BindValue(Vector &col, idx_t c, idx_t r);
	void BindParameter(const Value &param, idx_t param_idx);
	int GetType(idx_t col);
	string GetName(idx_t col);
	idx_t GetColumnCount();
	bool IsOpen();
	void Close();
	void CheckTypeMatches(const SqliteBindData &bind_data, sqlite3_value *val, int sqlite_column_type,
	                      int expected_type, idx_t col_idx);
	void CheckTypeIsFloatOrInteger(sqlite3_value *val, int sqlite_column_type, idx_t col_idx);
	void Reset();
	void ClearBindings();
};

template <>
string SQLiteStatement::GetValue(idx_t col);
template <>
int SQLiteStatement::GetValue(idx_t col);
template <>
int64_t SQLiteStatement::GetValue(idx_t col);
template <>
sqlite3_value *SQLiteStatement::GetValue(idx_t col);

template <>
void SQLiteStatement::Bind(idx_t col, int32_t value);
template <>
void SQLiteStatement::Bind(idx_t col, int64_t value);
template <>
void SQLiteStatement::Bind(idx_t col, double value);
template <>
void SQLiteStatement::Bind(idx_t col, std::nullptr_t value);

} // namespace duckdb
