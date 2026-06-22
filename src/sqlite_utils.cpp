#include "sqlite_utils.hpp"

#include <chrono>
#include <random>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fileapi.h>
#include "duckdb/common/windows_util.hpp"
#endif // _WIN32

namespace duckdb {

void SQLiteUtils::Check(int rc, sqlite3 *db) {
	if (rc != SQLITE_OK) {
		throw std::runtime_error(string(sqlite3_errmsg(db)));
	}
}

string SQLiteUtils::TypeToString(int sqlite_type) {
	switch (sqlite_type) {
	case SQLITE_ANY:
		return "any";
	case SQLITE_INTEGER:
		return "integer";
	case SQLITE_TEXT:
		return "text";
	case SQLITE_BLOB:
		return "blob";
	case SQLITE_FLOAT:
		return "float";
	default:
		return "unknown";
	}
}

string SQLiteUtils::SanitizeString(const string &table_name) {
	return StringUtil::Replace(table_name, "'", "''");
}

string SQLiteUtils::SanitizeIdentifier(const string &table_name) {
	return StringUtil::Replace(table_name, "\"", "\"\"");
}

LogicalType SQLiteUtils::ToSQLiteType(const LogicalType &input) {
	switch (input.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		return LogicalType::BIGINT;
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return LogicalType::DOUBLE;
	case LogicalTypeId::BLOB:
		return LogicalType::BLOB;
	default:
		return LogicalType::VARCHAR;
	}
}

LogicalType SQLiteUtils::TypeToLogicalType(const string &sqlite_type) {
	// type affinity rules are taken from here:
	// https://www.sqlite.org/datatype3.html

	// If the declared type contains the string "INT" then it is assigned INTEGER
	// affinity.
	if (StringUtil::Contains(sqlite_type, "int")) {
		return LogicalType::BIGINT;
	}

	// boolean
	if (StringUtil::Contains(sqlite_type, "bool")) {
		return LogicalType::BIGINT;
	}

	// If the declared type of the column contains any of the strings "CHAR",
	// "CLOB", or "TEXT" then that column has TEXT affinity. Notice that the type
	// VARCHAR contains the string "CHAR" and is thus assigned TEXT affinity.
	if (StringUtil::Contains(sqlite_type, "char") || StringUtil::Contains(sqlite_type, "clob") ||
	    StringUtil::Contains(sqlite_type, "text")) {
		return LogicalType::VARCHAR;
	}

	// If the declared type for a column contains the string "BLOB" or if no type
	// is specified then the column has affinity BLOB.
	if (StringUtil::Contains(sqlite_type, "blob") || sqlite_type.empty()) {
		return LogicalType::BLOB;
	}

	// If the declared type for a column contains any of the strings "REAL",
	// "FLOA", or "DOUB" then the column has REAL affinity.
	if (StringUtil::Contains(sqlite_type, "real") || StringUtil::Contains(sqlite_type, "floa") ||
	    StringUtil::Contains(sqlite_type, "doub")) {
		return LogicalType::DOUBLE;
	}
	// Otherwise, the affinity is NUMERIC.
	// now numeric sounds simple, but it is rather complex:
	// A column with NUMERIC affinity may contain values using all five storage
	// classes.
	// ...
	// we add some more extra rules to try to be somewhat sane
	if (sqlite_type == "date") {
		return LogicalType::DATE;
	}

	// datetime, timestamp
	if (StringUtil::Contains(sqlite_type, "time")) {
		return LogicalType::TIMESTAMP;
	}

	// decimal, numeric
	if (StringUtil::Contains(sqlite_type, "dec") || StringUtil::Contains(sqlite_type, "num")) {
		return LogicalType::DOUBLE;
	}

	// alright, give up and fallback to varchar
	return LogicalType::VARCHAR;
}

static void AppendEnvVarValue(vector<string> &vec, const string &name) {
	const char *val = std::getenv(name.c_str());
	if (val != nullptr && *val != '\0') {
		string val_str(val);
		vec.emplace_back(std::move(val_str));
	}
}

static vector<string> TempDirCandidates() {
	vector<string> res;
#ifdef _WIN32
	vector<wchar_t> buf;
	buf.resize(MAX_PATH + 2);
	auto len = GetTempPath2W(static_cast<DWORD>(buf.size()), buf.data());
	if (len > 0) { // The maximum possible return value is MAX_PATH+1 (261).
		string dirname = WindowsUtil::UnicodeToUTF8(buf.data());
		res.emplace_back(std::move(dirname));
	} else {
		AppendEnvVarValue(res, "TEMP");
		AppendEnvVarValue(res, "TMP");
	}
#else  // !_WIN32
	AppendEnvVarValue(res, "TMPDIR");
	AppendEnvVarValue(res, "TMP");
	res.emplace_back("/tmp");
	res.emplace_back("/var/tmp");
	res.emplace_back(".");
#endif // _WIN32
	return res;
}

string SQLiteUtils::GetSystemTempDirectory(FileSystem &fs) {
	for (const auto &dir : TempDirCandidates()) {
		if (fs.DirectoryExists(dir)) {
			return fs.CanonicalizePath(dir);
		}
	}
	throw IOException("Unable to determine the OS temporary directory, setting "
										"TMP environment variable may resolve this");
}

string SQLiteUtils::GenerateRandomDirName(const string &prefix) {
	auto now = std::chrono::system_clock::now().time_since_epoch().count();
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> dis(0, 100000);
	return prefix + "_" + std::to_string(now) + "_" + std::to_string(dis(gen));
}

} // namespace duckdb
