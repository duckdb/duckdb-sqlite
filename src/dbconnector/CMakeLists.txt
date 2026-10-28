set(SQLITE_DBCONNECTOR_PATH "${CMAKE_CURRENT_LIST_DIR}/../../database-connector")

add_library(
  sqlite_ext_dbconnector OBJECT
  ${SQLITE_DBCONNECTOR_PATH}/src/attached/attached_catalog.cpp
  ${SQLITE_DBCONNECTOR_PATH}/src/attached/attached_table.cpp
  ${SQLITE_DBCONNECTOR_PATH}/src/functions/configure_pool.cpp
  ${SQLITE_DBCONNECTOR_PATH}/src/query/query_writer.cpp
  ${SQLITE_DBCONNECTOR_PATH}/src/table_scan/filter_pushdown.cpp
  ${SQLITE_DBCONNECTOR_PATH}/src/table_scan/filter_util.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:sqlite_ext_dbconnector>
    PARENT_SCOPE)
