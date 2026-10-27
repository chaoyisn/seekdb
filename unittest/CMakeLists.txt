# Focused AI COLUMN regressions; this competition baseline ships no CMake tests.
find_package(Threads REQUIRED)
add_executable(sql_ai_column_tests
  sql/parser/test_ai_column_parser.cpp
  sql/resolver/test_ai_column_resolver.cpp)
target_include_directories(sql_ai_column_tests PRIVATE "${DEP_DIR}/include")
target_link_libraries(sql_ai_column_tests PRIVATE
  oceanbase "${DEP_DIR}/lib/libgtest_main.a" "${DEP_DIR}/lib/libgtest.a" Threads::Threads)
add_test(NAME sql_ai_column_tests COMMAND sql_ai_column_tests)
add_custom_target(pretest DEPENDS sql_ai_column_tests)
