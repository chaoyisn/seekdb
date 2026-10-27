/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


// Supplemental protocol regressions. All endpoints and API keys are synthetic.
#include <gtest/gtest.h>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "share/ai_service/ob_ai_service_struct.h"

using namespace oceanbase::common;

TEST(OpenAIEnhancement, restores_embedding_indices)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObString source(R"({"data":[{"index":1,"embedding":[2,3]},{"index":0,"embedding":[0,1]}]})");
  ObJsonObject *response = nullptr;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, source, response));
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator, response, result));
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, result->json_type());
  ASSERT_EQ(2, result->element_count());
  ObJsonArray *first = static_cast<ObJsonArray *>(static_cast<ObJsonArray *>(result)->get_value(0));
  EXPECT_EQ(0, static_cast<ObJsonInt *>(first->get_value(0))->value());
}

TEST(OpenAIEnhancement, rejects_malformed_embedding_results)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  for (const char *text : {
      R"({"data":{}})", R"({"data":[null]})",
      R"({"data":[{"embedding":[1]}]})",
      R"({"data":[{"index":-1,"embedding":[1]}]})",
      R"({"data":[{"index":9,"embedding":[1]}]})",
      R"({"data":[{"index":0,"embedding":123}]})",
      R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})"}) {
    ObString source(text);
    ObJsonObject *response = nullptr;
    ObIJsonBase *result = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, source, response));
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator, response, result)) << text;
  }
}

TEST(OpenAIEnhancement, forwards_completion_options_and_requires_nonempty_content)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIComplete provider;
  ObString model("arbitrary-model"), prompt, text("text"), config_text(R"({"temperature":0.2,"max_tokens":32})");
  ObJsonObject *config = nullptr;
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, config_text, config));
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, prompt, text, config, body));
  EXPECT_NE(nullptr, body->get_value("temperature"));
  EXPECT_NE(nullptr, body->get_value("max_tokens"));
  for (const char *source : {R"({"choices":[]})", R"({"choices":[{"message":{"content":""}}]})",
      R"({"choices":[{"message":{"content":null}}]})", R"({"choices":[{"message":{"content":{}}}]})"}) {
    ObString response_text(source);
    ObJsonObject *response = nullptr;
    ObIJsonBase *result = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, response_text, response));
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator, response, result));
  }
}

TEST(OllamaEnhancement, uses_native_nonstreaming_request_and_optional_key)
{
  ObArenaAllocator allocator;
  ObOllamaUtils::ObOllamaComplete provider;
  ObString model("native-model"), system("system"), prompt("hello"), key;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_header(allocator, key, headers));
  ASSERT_EQ(1, headers.count());
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, system, prompt, nullptr, body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_EQ(ObJsonNodeType::J_BOOLEAN, body->get_value("stream")->json_type());
  EXPECT_FALSE(static_cast<ObJsonBoolean *>(body->get_value("stream"))->value());
  EXPECT_NE(nullptr, body->get_value("prompt"));
  EXPECT_NE(nullptr, body->get_value("system"));
  EXPECT_EQ(nullptr, body->get_value("messages"));
}

TEST(OllamaEnhancement, validates_endpoint_provider_without_weakening_openai_key_requirement)
{
  ObArenaAllocator allocator;
  oceanbase::share::ObAiModelEndpointInfo endpoint;
  ObString source(R"({"ai_model_name":"model","url":"http://127.0.0.1:8000/api/generate","access_key":"","provider":"oLlAmA"})");
  ObJsonObject *config = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, source, config));
  ASSERT_EQ(OB_SUCCESS, endpoint.parse_from_json_base(allocator, ObString("endpoint"), *config));
  oceanbase::share::ObAiModelEndpointInfo copied;
  ASSERT_EQ(OB_SUCCESS, copied.deep_copy(allocator, endpoint));
  ObString alter_ollama(R"({"provider":"Ollama"})");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, alter_ollama, config));
  ASSERT_EQ(OB_SUCCESS, copied.merge_delta_endpoint(allocator, *config));
  EXPECT_NE(nullptr, copied.get_encrypted_access_key().ptr());
  ObString change(R"({"provider":"OPENAI"})");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, change, config));
  EXPECT_EQ(OB_AI_FUNC_PARAM_EMPTY, endpoint.merge_delta_endpoint(allocator, *config));
}
