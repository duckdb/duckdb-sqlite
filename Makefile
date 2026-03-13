PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=sqlite_scanner
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile


# Setup the sqlite3 tpch database
data/db/tpch.db:
	command -v sqlite3 || (command -v brew && brew install sqlite) || (command -v choco && choco install sqlite -y) || (command -v apt-get && apt-get install -y sqlite3) || (command -v yum && yum install -y sqlite) || (command -v apk && apk add sqlite) || echo "no sqlite3"
	./build/release/$(DUCKDB_PATH) < data/sql/tpch-export.duckdb || tree ./build/release || echo "neither tree not duck"
	sqlite3 data/db/tpch.db < data/sql/tpch-create.sqlite

# Override the test target implementations from the duckdb_extension.Makefile
test_release_internal: data/db/tpch.db
	SQLITE_TPCH_GENERATED=1 ./build/release/$(TEST_PATH) "$(PROJ_DIR)test/*"

test_debug_internal: data/db/tpch.db
	SQLITE_TPCH_GENERATED=1 ./build/debug/$(TEST_PATH) "$(PROJ_DIR)test/*"

test_reldebug_internal: data/db/tpch.db
	SQLITE_TPCH_GENERATED=1 ./build/reldebug/$(TEST_PATH) "$(PROJ_DIR)test/*"
