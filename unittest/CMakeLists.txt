# Focused AI COLUMN regressions; this competition baseline ships no CMake tests.
find_package(Threads REQUIRED)
add_executable(sql_ai_column_tests
  sql/parser/test_ai_column_parser.cpp
  sql/resolver/test_ai_column_resolver.cpp
  sql/resolver/test_ai_column_definition.cpp
  share/schema/test_ai_column_schema.cpp
  observer/virtual_table/test_ai_column_metadata_table.cpp
  observer/test_ai_column_result.cpp)
target_include_directories(sql_ai_column_tests PRIVATE "${DEP_DIR}/include")
target_link_libraries(sql_ai_column_tests PRIVATE
  oceanbase "${DEP_DIR}/lib/libgtest_main.a" "${DEP_DIR}/lib/libgtest.a" Threads::Threads)
add_test(NAME sql_ai_column_tests COMMAND sql_ai_column_tests)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(sql_ai_column_transport_tests observer/test_ai_column_transport.cpp)
target_include_directories(sql_ai_column_transport_tests PRIVATE "${DEP_DIR}/include")
target_link_libraries(sql_ai_column_transport_tests PRIVATE
  oceanbase "${DEP_DIR}/lib/libgtest_main.a" "${DEP_DIR}/lib/libgtest.a" Threads::Threads)
add_test(NAME sql_ai_column_transport_tests
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/observer/ai_column_transport_fault_service.py
    --binary $<TARGET_FILE:sql_ai_column_transport_tests>
    --report ${CMAKE_CURRENT_BINARY_DIR}/ai_column_transport_http.json)
add_custom_target(pretest DEPENDS sql_ai_column_tests sql_ai_column_transport_tests)
