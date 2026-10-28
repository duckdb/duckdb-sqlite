#include "storage/sqlite_options.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

static string ParseKeyword(const string &option, const string &value, const vector<string> &accepted) {
	string trimmed = value;
	StringUtil::Trim(trimmed);
	auto upper = StringUtil::Upper(trimmed);
	for (auto &candidate : accepted) {
		if (upper == candidate) {
			return candidate;
		}
	}
	// SQLite also accepts the numeric spellings of PRAGMA synchronous (0..3); map them to the keyword
	if (option == "synchronous" && upper.size() == 1 && upper[0] >= '0' && upper[0] <= '3') {
		return accepted[upper[0] - '0'];
	}
	throw BinderException("Invalid value \"%s\" for SQLite option %s: expected one of %s", value, option,
	                      StringUtil::Join(accepted, ", "));
}

string SQLiteOpenOptions::ParseJournalMode(const string &value) {
	return ParseKeyword("journal_mode", value, {"DELETE", "TRUNCATE", "PERSIST", "MEMORY", "WAL", "OFF"});
}

string SQLiteOpenOptions::ParseSynchronous(const string &value) {
	// order matters: index == the numeric PRAGMA value
	return ParseKeyword("synchronous", value, {"OFF", "NORMAL", "FULL", "EXTRA"});
}

} // namespace duckdb
