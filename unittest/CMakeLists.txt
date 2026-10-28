# Focused AI function and AI COLUMN regressions for competition integrated branch
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# AI Function tests
add_executable(sql_ai_function_tests
  sql/engine/expr/test_ob_ai_prompt_enhancement.cpp
  sql/engine/expr/test_ob_openai_utils_enhancement.cpp)
target_include_directories(sql_ai_function_tests PRIVATE "${DEP_DIR}/include")
target_link_libraries(sql_ai_function_tests PRIVATE
  oceanbase "${DEP_DIR}/lib/libgtest_main.a" "${DEP_DIR}/lib/libgtest.a" Threads::Threads)
add_test(NAME sql_ai_function_tests COMMAND sql_ai_function_tests)

add_executable(ob_expr_ai_prompt_test
  sql/engine/expr/test_ob_ai_prompt_enhancement.cpp)
target_include_directories(ob_expr_ai_prompt_test PRIVATE "${DEP_DIR}/include")
target_link_libraries(ob_expr_ai_prompt_test PRIVATE
  oceanbase "${DEP_DIR}/lib/libgtest_main.a" "${DEP_DIR}/lib/libgtest.a" Threads::Threads)
add_test(NAME ob_expr_ai_prompt_test COMMAND ob_expr_ai_prompt_test)

add_executable(test_ob_openai_utils
  sql/engine/expr/test_ob_openai_utils_enhancement.cpp)
target_include_directories(test_ob_openai_utils PRIVATE "${DEP_DIR}/include")
target_link_libraries(test_ob_openai_utils PRIVATE
  oceanbase "${DEP_DIR}/lib/libgtest_main.a" "${DEP_DIR}/lib/libgtest.a" Threads::Threads)
add_test(NAME test_ob_openai_utils COMMAND test_ob_openai_utils)

# AI Column tests
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

add_executable(sql_ai_column_transport_tests observer/test_ai_column_transport.cpp)
target_include_directories(sql_ai_column_transport_tests PRIVATE "${DEP_DIR}/include")
target_link_libraries(sql_ai_column_transport_tests PRIVATE
  oceanbase "${DEP_DIR}/lib/libgtest_main.a" "${DEP_DIR}/lib/libgtest.a" Threads::Threads)
add_test(NAME sql_ai_column_transport_tests
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/observer/ai_column_transport_fault_service.py
    --binary $<TARGET_FILE:sql_ai_column_transport_tests>
    --report ${CMAKE_CURRENT_BINARY_DIR}/ai_column_transport_http.json)

add_custom_target(pretest DEPENDS sql_ai_function_tests ob_expr_ai_prompt_test test_ob_openai_utils sql_ai_column_tests sql_ai_column_transport_tests)
