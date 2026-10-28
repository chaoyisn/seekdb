# Focused AI regressions for the competition branch, which ships no C++ test modules.
find_package(Threads REQUIRED)
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

add_custom_target(pretest DEPENDS sql_ai_function_tests ob_expr_ai_prompt_test test_ob_openai_utils)
