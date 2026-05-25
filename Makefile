PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=sqlite_scanner
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile


# Setup the sqlite3 tpch database
data/db/tpch.db:
	command -v sqlite3 || (command -v brew && brew install sqlite) || (command -v choco && choco install sqlite -y) || (command -v apt-get && apt-get install -y sqlite3) || (command -v yum && yum install -y sqlite) || (command -v apk && apk add sqlite) || echo "no sqlite3"
	./build/release/duckdb < data/sql/tpch-export.duckdb || tree ./build/release || echo "neither tree not duck"
	sqlite3 data/db/tpch.db < data/sql/tpch-create.sqlite

export SQLITE_TPCH_GENERATED=1

test_release: data/db/tpch.db

test_debug: data/db/tpch.db

test_reldebug: data/db/tpch.db
