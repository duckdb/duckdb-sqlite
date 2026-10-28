add_library(
  sqlite_ext_storage OBJECT
  sqlite_catalog.cpp
  sqlite_options.cpp
  sqlite_delete.cpp
  sqlite_index.cpp
  sqlite_index_entry.cpp
  sqlite_insert.cpp
  sqlite_query.cpp
  sqlite_table_entry.cpp
  sqlite_schema_entry.cpp
  sqlite_transaction.cpp
  sqlite_transaction_manager.cpp
  sqlite_update.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:sqlite_ext_storage>
    PARENT_SCOPE)
