include_directories(include)

add_subdirectory(dbconnector)
add_subdirectory(sqlite)
add_subdirectory(storage)

add_library(
  sqlite_ext_library OBJECT
  sqlite_db.cpp sqlite_duckdb_vfs_cache.cpp sqlite_extension.cpp sqlite_filter_pushdown.cpp sqlite_scanner.cpp
  sqlite_functions.cpp sqlite_sql_writer.cpp sqlite_stmt.cpp sqlite_storage.cpp sqlite_utils.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:sqlite_ext_library>
    PARENT_SCOPE)
