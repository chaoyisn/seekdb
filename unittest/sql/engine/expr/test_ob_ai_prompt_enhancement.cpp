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


// Supplemental regression tests; not the C++ tests mentioned in the published task.
#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

TEST(AIPromptEnhancement, replaces_every_repeated_long_argument)
{
  ObArenaAllocator allocator;
  std::string argument(8192, 'x');
  ObString text("{0}/{0}/{0}");
  ObString value(argument.length(), argument.data());
  ObJsonString node(value);
  ObJsonArray args(&allocator);
  ObJsonObject *prompt = nullptr;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, args.append(&node));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(allocator, text, &args, prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  EXPECT_EQ(argument + "/" + argument + "/" + argument, std::string(result.ptr(), result.length()));
}

TEST(AIPromptEnhancement, expands_nested_prompt_and_serializes_json_arguments)
{
  ObArenaAllocator allocator;
  ObString source(R"({"template":"{0}|{1}|{2}","args":[{"template":"你好 {0}","args":["\"雪\"\n\\"]},[1,true],{"k":2}]})");
  ObJsonObject *prompt = nullptr;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, source, prompt));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  const std::string actual(result.ptr(), result.length());
  EXPECT_EQ(0, actual.find("你好 \"雪\"\n\\|"));
  EXPECT_NE(std::string::npos, actual.find("true"));
  EXPECT_NE(std::string::npos, actual.find("\"k\""));
}

TEST(AIPromptEnhancement, rejects_out_of_range_and_overflowing_indices)
{
  ObArenaAllocator allocator;
  ObString value("value");
  ObJsonString node(value);
  ObJsonArray args(&allocator);
  ASSERT_EQ(OB_SUCCESS, args.append(&node));
  for (const char *text : {"{1}", "{-1}", "{+0}", "{99999999999999999999999999999999999999999}"}) {
    ObString pattern(text);
    ObJsonObject *prompt = nullptr;
    ObString output;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(allocator, pattern, &args, prompt));
    EXPECT_EQ(OB_INVALID_ARGUMENT,
        ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, output));
  }
}

TEST(AIPromptEnhancement, preserves_literal_braces_and_rejects_invalid_prompt_shape)
{
  ObArenaAllocator allocator;
  ObString source(R"({"template":"literal {name} and {}","args":[]})");
  ObJsonObject *prompt = nullptr;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, source, prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  EXPECT_EQ("literal {name} and {}", std::string(result.ptr(), result.length()));
  ObString invalid(R"({"template":123,"args":[]})");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, invalid, prompt));
  EXPECT_EQ(OB_INVALID_ARGUMENT,
      ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
}
