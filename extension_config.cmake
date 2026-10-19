# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(sqlite_scanner
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
duckdb_extension_load(tpch)

# The remote-SQLite tests need httpfs, needs to be synchronized with the httpfs
# config in duckdb/.github/config/extensions/httpfs.cmake
if(NOT EMSCRIPTEN)
    duckdb_extension_load(httpfs
        GIT_URL https://github.com/duckdb/duckdb-httpfs
        GIT_TAG 740d974694058ab1ecd35a5f2f5dfca0d0977b5d # main 2026-10-18 pin
    )
endif()
