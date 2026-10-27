#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "sql/resolver/ddl/ob_ai_column_resolver.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;
using namespace oceanbase::share::schema;

namespace {
void valid_definition(AiColumnSchema &definition)
{
  definition.set_column_id(17);
  definition.set_state_column_id(18);
  definition.set_request_column_id(19);
  definition.set_result_spec_column_id(20);
  ASSERT_EQ(OB_SUCCESS,definition.set_expression(ObString::make_string("AI_COMPLETE('m',content)")));
  ASSERT_EQ(OB_SUCCESS,definition.set_normalized_expr(ObString::make_string("AI_COMPLETE('m',content,NULL)")));
}

TEST(AiColumnDefinition, auxiliary_columns_are_real_hidden_stored_columns)
{
  AiColumnSchema definition;
  valid_definition(definition);
  ObSEArray<ObColumnSchemaV2,3> columns;
  ASSERT_EQ(OB_SUCCESS,AiColumnResolver::make_hidden_columns(definition,columns));
  ASSERT_EQ(3,columns.count());
  for (int i=0;i<3;++i) {
    EXPECT_TRUE(columns.at(i).is_hidden());
    EXPECT_FALSE(columns.at(i).is_generated_column());
    EXPECT_FALSE(columns.at(i).is_ai_column());
    EXPECT_EQ(18U+i,columns.at(i).get_column_id());
  }
  EXPECT_EQ(0,columns.at(0).get_cur_default_value().get_int());
  EXPECT_EQ(0,columns.at(0).get_orig_default_value().get_int());
  EXPECT_FALSE(columns.at(0).is_nullable());
  EXPECT_EQ(64,columns.at(1).get_data_length());
  EXPECT_TRUE(columns.at(1).get_cur_default_value().is_null());
  EXPECT_TRUE(columns.at(2).get_cur_default_value().is_null());
  EXPECT_EQ(0,columns.at(2).get_column_name_str().compare("__ai_result_spec_17"));
}

TEST(AiColumnDefinition, invalid_definition_allocates_no_auxiliary_columns)
{
  AiColumnSchema definition;
  ObSEArray<ObColumnSchemaV2,3> columns;
  EXPECT_EQ(OB_INVALID_ARGUMENT,AiColumnResolver::make_hidden_columns(definition,columns));
  EXPECT_EQ(0,columns.count());
}

TEST(AiColumnDefinition, final_schema_version_updates_definition_version)
{
  AiColumnSchema definition;
  valid_definition(definition);
  ObColumnSchemaV2 column;
  column.set_column_id(17);
  ASSERT_EQ(OB_SUCCESS,column.set_ai_column_schema(definition));
  column.set_schema_version(1234);
  ASSERT_TRUE(column.is_ai_column());
  EXPECT_EQ(1234,column.get_ai_column_schema()->get_definition_version());
}

class AiColumnCompletionConfig : public ::testing::Test {
protected:
  ObArenaAllocator allocator_;
  ObJsonObject *config_ = nullptr;
  void parse(const char *text) {
    ObString input=ObString::make_string(text);
    ASSERT_EQ(OB_SUCCESS,ObAIFuncJsonUtils::get_json_object_form_str(allocator_,input,config_));
  }
};

TEST_F(AiColumnCompletionConfig, null_config_is_unchanged)
{
  ObString system;
  ObJsonObject *provider = nullptr;
  EXPECT_EQ(OB_SUCCESS,ObAIFuncUtils::prepare_ai_column_config(allocator_,nullptr,system,provider));
  EXPECT_TRUE(system.empty());
  EXPECT_EQ(nullptr,provider);
}

TEST_F(AiColumnCompletionConfig, ordinary_config_keeps_existing_behavior)
{
  parse("{\"temperature\":0.2,\"messages\":[]}");
  ObString system;
  ObJsonObject *provider = nullptr;
  ASSERT_EQ(OB_SUCCESS,ObAIFuncUtils::prepare_ai_column_config(allocator_,config_,system,provider));
  EXPECT_TRUE(system.empty());
  EXPECT_EQ(config_,provider);
  EXPECT_NE(nullptr,provider->get_value("messages"));
}

TEST_F(AiColumnCompletionConfig, typed_hint_becomes_system_message_and_preserves_input)
{
  parse("{\"__ai_column_system_prompt\":\"Return exactly one valid JSON value\",\"temperature\":0.2,\"max_tokens\":64}");
  ObString system;
  ObJsonObject *provider = nullptr;
  ASSERT_EQ(OB_SUCCESS,ObAIFuncUtils::prepare_ai_column_config(allocator_,config_,system,provider));
  ASSERT_NE(nullptr,provider);
  EXPECT_EQ(nullptr,provider->get_value("__ai_column_system_prompt"));
  EXPECT_NE(nullptr,config_->get_value("__ai_column_system_prompt"));
  EXPECT_NE(nullptr,provider->get_value("temperature"));
  EXPECT_NE(nullptr,provider->get_value("max_tokens"));
  ObOpenAIUtils::ObOpenAIComplete completion;
  ObString model=ObString::make_string("completion-model");
  ObString content=ObString::make_string("unchanged input");
  ObJsonObject *body=nullptr;
  ASSERT_EQ(OB_SUCCESS,completion.get_body(allocator_,model,system,content,provider,body));
  ASSERT_NE(nullptr,body);
  auto *messages=static_cast<ObJsonArray *>(body->get_value("messages"));
  ASSERT_NE(nullptr,messages);
  ASSERT_EQ(2,messages->element_count());
  auto *first=static_cast<ObJsonObject *>(messages->get_value(0));
  auto *second=static_cast<ObJsonObject *>(messages->get_value(1));
  ASSERT_NE(nullptr,first); ASSERT_NE(nullptr,second);
  EXPECT_EQ(0,static_cast<ObJsonString *>(first->get_value("role"))->value().compare("system"));
  EXPECT_EQ(0,static_cast<ObJsonString *>(first->get_value("content"))->value().compare(system));
  EXPECT_EQ(0,static_cast<ObJsonString *>(second->get_value("role"))->value().compare("user"));
  EXPECT_EQ(0,static_cast<ObJsonString *>(second->get_value("content"))->value().compare(content));
  EXPECT_EQ(nullptr,body->get_value("__ai_column_system_prompt"));
}

TEST_F(AiColumnCompletionConfig, typed_config_cannot_replace_the_message_pair)
{
  parse("{\"__ai_column_system_prompt\":\"Return one integer\",\"messages\":[]}");
  ObString system;
  ObJsonObject *provider = nullptr;
  EXPECT_EQ(OB_INVALID_ARGUMENT,ObAIFuncUtils::prepare_ai_column_config(allocator_,config_,system,provider));
}

TEST_F(AiColumnCompletionConfig, invalid_hint_rejected)
{
  parse("{\"__ai_column_system_prompt\":7}");
  ObString system;
  ObJsonObject *provider = nullptr;
  EXPECT_EQ(OB_INVALID_ARGUMENT,ObAIFuncUtils::prepare_ai_column_config(allocator_,config_,system,provider));
  EXPECT_TRUE(system.empty());
}
} // namespace
