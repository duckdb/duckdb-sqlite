#include "sqlite_stmt.hpp"
#include "sqlite_db.hpp"
#include "sqlite_scanner.hpp"

namespace duckdb {

SQLiteStatement::SQLiteStatement() : db(nullptr), stmt(nullptr) {
}

SQLiteStatement::SQLiteStatement(sqlite3 *db, sqlite3_stmt *stmt) : db(db), stmt(stmt) {
	D_ASSERT(db);
}

SQLiteStatement::~SQLiteStatement() {
	Close();
}

SQLiteStatement::SQLiteStatement(SQLiteStatement &&other) noexcept {
	std::swap(db, other.db);
	std::swap(stmt, other.stmt);
}

SQLiteStatement &SQLiteStatement::operator=(SQLiteStatement &&other) noexcept {
	std::swap(db, other.db);
	std::swap(stmt, other.stmt);
	return *this;
}

int SQLiteStatement::Step() {
	D_ASSERT(db);
	D_ASSERT(stmt);
	auto rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		return true;
	}
	if (rc == SQLITE_DONE) {
		return false;
	}
	throw std::runtime_error(string(sqlite3_errmsg(db)));
}
int SQLiteStatement::GetType(idx_t col) {
	D_ASSERT(stmt);
	return sqlite3_column_type(stmt, col);
}

string SQLiteStatement::GetName(idx_t col) {
	D_ASSERT(stmt);
	return sqlite3_column_name(stmt, col);
}

idx_t SQLiteStatement::GetColumnCount() {
	D_ASSERT(stmt);
	return sqlite3_column_count(stmt);
}

bool SQLiteStatement::IsOpen() {
	return stmt;
}

void SQLiteStatement::Close() {
	if (!IsOpen()) {
		return;
	}
	sqlite3_finalize(stmt);
	db = nullptr;
	stmt = nullptr;
}

void SQLiteStatement::CheckTypeMatches(const SqliteBindData &bind_data, int sqlite_column_type,
                                       int expected_type, idx_t col_idx) {
	D_ASSERT(stmt);
	if (bind_data.all_varchar) {
		// no type check required
		return;
	}
	if (sqlite_column_type != expected_type) {
		auto column_name = string(sqlite3_column_name(stmt, int(col_idx)));
		auto text = sqlite3_column_text(stmt, int(col_idx));
		auto value_as_text = text ? string((const char *)text) : string();
		auto message = "Invalid type in column \"" + column_name + "\": column was declared as " +
		               SQLiteUtils::TypeToString(expected_type) + ", found \"" + value_as_text + "\" of type \"" +
		               SQLiteUtils::TypeToString(sqlite_column_type) + "\" instead.";
		message += "\n* SET sqlite_all_varchar=true to load all columns as VARCHAR "
		           "and skip type conversions";
		throw Exception(ExceptionType::MISMATCH_TYPE, message);
	}
}

void SQLiteStatement::CheckTypeIsFloatOrInteger(int sqlite_column_type, idx_t col_idx) {
	if (sqlite_column_type != SQLITE_FLOAT && sqlite_column_type != SQLITE_INTEGER) {
		auto column_name = string(sqlite3_column_name(stmt, int(col_idx)));
		auto text = sqlite3_column_text(stmt, int(col_idx));
		auto value_as_text = text ? string((const char *)text) : string();
		auto message = "Invalid type in column \"" + column_name + "\": expected float or integer, found \"" +
		               value_as_text + "\" of type \"" + SQLiteUtils::TypeToString(sqlite_column_type) + "\" instead.";
		message += "\n* SET sqlite_all_varchar=true to load all columns as VARCHAR "
		           "and skip type conversions";
		throw Exception(ExceptionType::MISMATCH_TYPE, message);
	}
}

void SQLiteStatement::Reset() {
	SQLiteUtils::Check(sqlite3_reset(stmt), db);
}

void SQLiteStatement::ClearBindings() {
	SQLiteUtils::Check(sqlite3_clear_bindings(stmt), db);
}

template <>
string SQLiteStatement::GetValue(idx_t col) {
	D_ASSERT(stmt);
	auto ptr = sqlite3_column_text(stmt, col);
	if (!ptr) {
		return string();
	}
	return string((char *)ptr);
}

template <>
int SQLiteStatement::GetValue(idx_t col) {
	D_ASSERT(stmt);
	return sqlite3_column_int(stmt, col);
}

template <>
int64_t SQLiteStatement::GetValue(idx_t col) {
	D_ASSERT(stmt);
	return sqlite3_column_int64(stmt, col);
}

template <>
sqlite3_value *SQLiteStatement::GetValue(idx_t col) {
	D_ASSERT(stmt);
	return sqlite3_column_value(stmt, col);
}

template <>
void SQLiteStatement::Bind(idx_t col, int32_t value) {
	SQLiteUtils::Check(sqlite3_bind_int(stmt, col + 1, value), db);
}

template <>
void SQLiteStatement::Bind(idx_t col, int64_t value) {
	SQLiteUtils::Check(sqlite3_bind_int64(stmt, col + 1, value), db);
}

template <>
void SQLiteStatement::Bind(idx_t col, double value) {
	SQLiteUtils::Check(sqlite3_bind_double(stmt, col + 1, value), db);
}

void SQLiteStatement::BindBlob(idx_t col, const string_t &value) {
	SQLiteUtils::Check(sqlite3_bind_blob(stmt, col + 1, value.GetDataUnsafe(), value.GetSize(), nullptr), db);
}

void SQLiteStatement::BindBlob(idx_t col, const string &value) {
	SQLiteUtils::Check(sqlite3_bind_blob(stmt, col + 1, value.c_str(), value.length(), nullptr), db);
}

void SQLiteStatement::BindText(idx_t col, const string_t &value) {
	SQLiteUtils::Check(sqlite3_bind_text(stmt, col + 1, value.GetDataUnsafe(), value.GetSize(), nullptr), db);
}

void SQLiteStatement::BindText(idx_t col, const string &value) {
	SQLiteUtils::Check(sqlite3_bind_text(stmt, col + 1, value.c_str(), value.length(), nullptr), db);
}

template <>
void SQLiteStatement::Bind(idx_t col, std::nullptr_t value) {
	SQLiteUtils::Check(sqlite3_bind_null(stmt, col + 1), db);
}

void SQLiteStatement::BindValue(Vector &col, idx_t c, idx_t r) {
	auto &mask = FlatVector::Validity(col);
	if (!mask.RowIsValid(r)) {
		Bind<std::nullptr_t>(c, nullptr);
	} else {
		switch (col.GetType().id()) {
		case LogicalTypeId::BIGINT:
			Bind<int64_t>(c, FlatVector::GetData<int64_t>(col)[r]);
			break;
		case LogicalTypeId::DOUBLE:
			Bind<double>(c, FlatVector::GetData<double>(col)[r]);
			break;
		case LogicalTypeId::BLOB:
			BindBlob(c, FlatVector::GetData<string_t>(col)[r]);
			break;
		case LogicalTypeId::VARCHAR:
			BindText(c, FlatVector::GetData<string_t>(col)[r]);
			break;
		default:
			throw InternalException("Unsupported type \"%s\" for SQLite::BindValue", col.GetType());
		}
	}
}

void SQLiteStatement::BindParameter(const Value &param, idx_t param_idx) {
	if (param.IsNull()) {
		Bind<std::nullptr_t>(param_idx, nullptr);
	} else {
		switch (param.type().id()) {
		case LogicalTypeId::BIGINT:
			Bind<int64_t>(param_idx, BigIntValue::Get(param));
			break;
		case LogicalTypeId::DOUBLE:
			Bind<double>(param_idx, DoubleValue::Get(param));
			break;
		case LogicalTypeId::BLOB:
			BindBlob(param_idx, StringValue::Get(param));
			break;
		case LogicalTypeId::VARCHAR:
			BindText(param_idx, StringValue::Get(param));
			break;
		default:
			throw InternalException("Unsupported parameter type \"%s\", index: %zu for SQLite::BindValue", param.type().ToString(), param_idx);
		}
	}
}

} // namespace duckdb
