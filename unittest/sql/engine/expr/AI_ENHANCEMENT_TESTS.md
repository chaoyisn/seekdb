# Supplemental AI enhancement regressions

These tests were added locally. They are not the missing C++ tests or the official Mock mentioned in the task documentation.
No model download, real model invocation or external implementation of AI SQL functions is used.

Build the pinned competition branch using its normal build.sh/CMake instructions, then:

```sh
make -C build_release -j1 sql_ai_function_tests
ctest --test-dir build_release -R '^sql_ai_function_tests$' --output-on-failure
```

Start `tools/deploy/mysql_test/test_suite/ai_function/include/mock_ai_protocol.py` beside an isolated database, after checking port 8000 is free.
The fixture sends reversed OpenAI embedding and rerank results and supports explicit fault paths.
Its log contains synthetic prompts/keys only; do not reuse it with real credentials or private data.

Run the integration script with Python and PyMySQL, providing an isolated SQL port and a report path:

```sh
python3 unittest/sql/engine/expr/test_ai_function_integration.py --port 12882 --report /path/to/integration.json
```

The runner refuses the original contest port 2881. It uses its own database/model/endpoint names and checks real request counts and input-to-SQL-row mapping.
Do not modify published mysqltest expected results to pass these tests.
