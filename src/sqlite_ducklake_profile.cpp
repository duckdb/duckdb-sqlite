#include "sqlite_ducklake_profile.hpp"

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/extension_callback.hpp"

namespace duckdb {

//! See ducklake_register_metadata_profile in DuckLake for the fields
static constexpr const char *DUCKLAKE_PROFILE =
    R"({
	"types": {
		"natively_unsupported": ["STRUCT", "MAP", "LIST", "FLOAT", "DOUBLE", "TIMESTAMP WITH TIME ZONE",
		                         "TIMESTAMPTZ_NS", "VARIANT"],
		"column_type_overrides": {"FLOAT": "VARCHAR", "DOUBLE": "VARCHAR", "NULL": "INTEGER"}
	},
	"max_column_count": 2000,
	"system_column_names": ["rowid", "oid", "_rowid_"],
	"dropped_tables_stay_readable": false,
	"rename_inlined_columns_by_copy": true,
	"reset_settings": ["sqlite_all_varchar"],
	"commit_errors": [
		{
			"pattern": "failed to commit: failed to execute query: database is locked",
			"hint": "In its rollback journal mode, a SQLite catalog cannot commit while another transaction is )"
    R"(open on it. Attach the catalog with META_JOURNAL_MODE 'WAL' to let open transactions and a )"
    R"(commit run at the same time.\n",
			"hint_unless_parameter": {"journal_mode": "wal"}
		},
		{"pattern": "database is locked\\s*$", "retryable": true}
	]
})";

static void RegisterProfile(DatabaseInstance &db) {
	Connection con(db);
	// without replace, a profile that the user registered before SQLite was loaded is kept
	auto result = con.Query("CALL ducklake_register_metadata_profile('sqlite', " +
	                        SQLString::ToString(DUCKLAKE_PROFILE) + ", replace := false)");
	if (result->HasError() && result->GetErrorType() != ExceptionType::CATALOG) {
		// DuckLake then uses its own profile, or none if it has no registration function
		DUCKDB_LOG_WARNING(db, "Failed to register the SQLite profile with DuckLake: %s", result->GetError());
	}
}

class SQLiteDuckLakeCallback : public ExtensionCallback {
public:
	void OnExtensionLoaded(DatabaseInstance &db, const string &name) override {
		if (name == "ducklake") {
			RegisterProfile(db);
		}
	}
};

void SQLiteDuckLakeProfile::Register(DatabaseInstance &db) {
	ExtensionCallback::Register(DBConfig::GetConfig(db), make_shared_ptr<SQLiteDuckLakeCallback>());
	if (db.ExtensionIsLoaded("ducklake")) {
		RegisterProfile(db);
	}
}

} // namespace duckdb
