# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(sqlite_scanner
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
duckdb_extension_load(tpch)

# The remote-SQLite tests need httpfs, pinned to the commit the duckdb engine coordinates with
# (APPLY_PATCHES applies the engine's bundled httpfs patches for the dev-engine API; engine and httpfs
# move independently and skew between releases). Skipped on WASM: the remote tests do not run there,
# and httpfs's OpenSSL dependency does not build for emscripten.
if(NOT EMSCRIPTEN)
    duckdb_extension_load(httpfs
        GIT_URL https://github.com/duckdb/duckdb-httpfs
        GIT_TAG fafb14f2c899ddfd1998f8adf2e07fbbfd28b3fd
        APPLY_PATCHES
    )
endif()
