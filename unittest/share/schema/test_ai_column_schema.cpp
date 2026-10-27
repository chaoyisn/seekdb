#include <gtest/gtest.h>
#include <vector>
#include <algorithm>
#include "share/schema/ob_ai_column_schema.h"

using namespace oceanbase::common;
using namespace oceanbase::share::schema;

namespace {
void init(AiColumnSchema &schema)
{
  schema.set_column_id(17);
  schema.set_state_column_id(18);
  schema.set_request_column_id(19);
  schema.set_result_spec_column_id(20);
  schema.set_definition_version(42);
  schema.set_max_retries(3);
  ASSERT_EQ(OB_SUCCESS, schema.set_expression(ObString::make_string("AI_COMPLETE('m',content)")));
  ASSERT_EQ(OB_SUCCESS, schema.set_normalized_expr(ObString::make_string("AI_COMPLETE('m',content,'Return exactly one valid JSON value')")));
  ObSEArray<uint64_t, 1> deps;
  ASSERT_EQ(OB_SUCCESS, deps.push_back(16));
  ASSERT_EQ(OB_SUCCESS, schema.set_dependencies(deps));
}

TEST(AiColumnSchemaTest, valid_definition)
{
  AiColumnSchema schema;
  EXPECT_FALSE(schema.is_valid());
  init(schema);
  EXPECT_TRUE(schema.is_valid());
  EXPECT_EQ(3, schema.get_max_retries());
  EXPECT_EQ(42, schema.get_definition_version());
  EXPECT_EQ(16U, schema.get_dependencies().at(0));
}

TEST(AiColumnSchemaTest, copy_owns_strings)
{
  AiColumnSchema copy;
  {
    AiColumnSchema original;
    init(original);
    ASSERT_EQ(OB_SUCCESS, copy.assign(original));
    EXPECT_NE(copy.get_expression().ptr(), original.get_expression().ptr());
    EXPECT_NE(copy.get_normalized_expr().ptr(), original.get_normalized_expr().ptr());
  }
  EXPECT_TRUE(copy.is_valid());
  EXPECT_EQ(0, copy.get_expression().compare("AI_COMPLETE('m',content)"));
}

TEST(AiColumnSchemaTest, serialization_owns_decoded_strings)
{
  AiColumnSchema original;
  init(original);
  std::vector<char> buffer(original.get_serialize_size());
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, original.serialize(buffer.data(), buffer.size(), pos));
  ASSERT_EQ(buffer.size(), static_cast<size_t>(pos));
  AiColumnSchema decoded;
  pos = 0;
  ASSERT_EQ(OB_SUCCESS, decoded.deserialize(buffer.data(), buffer.size(), pos));
  std::fill(buffer.begin(), buffer.end(), 0);
  EXPECT_TRUE(decoded.is_valid());
  EXPECT_EQ(0, decoded.get_expression().compare(original.get_expression()));
  EXPECT_EQ(0, decoded.get_normalized_expr().compare(original.get_normalized_expr()));
  EXPECT_EQ(20U, decoded.get_result_spec_column_id());
  EXPECT_EQ(16U, decoded.get_dependencies().at(0));
}

TEST(AiColumnSchemaTest, truncated_serialization_is_not_a_valid_definition)
{
  AiColumnSchema original;
  init(original);
  std::vector<char> buffer(original.get_serialize_size());
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, original.serialize(buffer.data(), buffer.size(), pos));
  AiColumnSchema decoded;
  pos = 0;
  EXPECT_NE(OB_SUCCESS, decoded.deserialize(buffer.data(), buffer.size()-1, pos));
  EXPECT_FALSE(decoded.is_valid());
}

TEST(AiColumnSchemaTest, remap_all_column_references)
{
  AiColumnSchema schema;
  init(schema);
  hash::ObHashMap<uint64_t, uint64_t> mapping;
  ASSERT_EQ(OB_SUCCESS, mapping.create(8, "AiColTest"));
  for (uint64_t id = 16; id <= 20; ++id) {
    ASSERT_EQ(OB_SUCCESS, mapping.set_refactored(id,id+100));
  }
  ASSERT_EQ(OB_SUCCESS, schema.convert_column_ids(mapping));
  EXPECT_TRUE(schema.is_valid());
  EXPECT_EQ(117U,schema.get_column_id());
  EXPECT_EQ(118U,schema.get_state_column_id());
  EXPECT_EQ(119U,schema.get_request_column_id());
  EXPECT_EQ(120U,schema.get_result_spec_column_id());
  EXPECT_EQ(116U,schema.get_dependencies().at(0));
}

TEST(AiColumnSchemaTest, missing_mapping_leaves_original_ids)
{
  AiColumnSchema schema;
  init(schema);
  hash::ObHashMap<uint64_t, uint64_t> mapping;
  ASSERT_EQ(OB_SUCCESS, mapping.create(8, "AiColTest"));
  for (uint64_t id = 17; id <= 20; ++id) {
    ASSERT_EQ(OB_SUCCESS, mapping.set_refactored(id,id+100));
  }
  EXPECT_NE(OB_SUCCESS,schema.convert_column_ids(mapping));
  EXPECT_EQ(17U,schema.get_column_id());
  EXPECT_EQ(18U,schema.get_state_column_id());
  EXPECT_EQ(19U,schema.get_request_column_id());
  EXPECT_EQ(20U,schema.get_result_spec_column_id());
  EXPECT_EQ(16U,schema.get_dependencies().at(0));
}

TEST(AiColumnSchemaTest, mapping_collision_rejected)
{
  AiColumnSchema schema;
  init(schema);
  hash::ObHashMap<uint64_t, uint64_t> mapping;
  ASSERT_EQ(OB_SUCCESS, mapping.create(8, "AiColTest"));
  for (uint64_t id = 16; id <= 20; ++id) {
    ASSERT_EQ(OB_SUCCESS, mapping.set_refactored(id,117));
  }
  EXPECT_EQ(OB_INVALID_ARGUMENT,schema.convert_column_ids(mapping));
  EXPECT_EQ(17U,schema.get_column_id());
}

TEST(AiColumnSchemaTest, rejects_hidden_column_aliasing_and_self_dependency)
{
  AiColumnSchema schema;
  init(schema);
  schema.set_state_column_id(17);
  EXPECT_FALSE(schema.is_valid());
  schema.set_state_column_id(18);
  ObSEArray<uint64_t, 1> deps;
  ASSERT_EQ(OB_SUCCESS,deps.push_back(17));
  ASSERT_EQ(OB_SUCCESS,schema.set_dependencies(deps));
  EXPECT_FALSE(schema.is_valid());
}
} // namespace

#include "share/schema/ob_column_schema.h"

TEST(AiColumnSchemaIntegration, metadata_keeps_stored_result_column_semantics)
{
  AiColumnSchema definition;
  init(definition);
  ObColumnSchemaV2 column;
  column.set_column_id(17);
  column.set_data_type(ObJsonType);
  ASSERT_EQ(OB_SUCCESS, column.set_ai_column_schema(definition));
  EXPECT_TRUE(column.is_ai_column());
  EXPECT_FALSE(column.is_generated_column());
  EXPECT_FALSE(column.is_hidden());
  EXPECT_TRUE(column.get_cur_default_value().is_null());
  EXPECT_TRUE(column.get_orig_default_value().is_null());
}

TEST(AiColumnSchemaIntegration, column_assign_deep_copies_metadata)
{
  AiColumnSchema definition;
  init(definition);
  ObColumnSchemaV2 original;
  original.set_column_id(17);
  ASSERT_EQ(OB_SUCCESS, original.set_ai_column_schema(definition));
  ObColumnSchemaV2 copy;
  ASSERT_EQ(OB_SUCCESS, copy.assign(original));
  EXPECT_NE(original.get_ai_column_schema(), copy.get_ai_column_schema());
  EXPECT_NE(original.get_ai_column_schema()->get_expression().ptr(),
            copy.get_ai_column_schema()->get_expression().ptr());
  original.reset();
  ASSERT_TRUE(copy.is_ai_column());
  EXPECT_EQ(42, copy.get_ai_column_schema()->get_definition_version());
  EXPECT_TRUE(copy.get_ai_column_schema()->is_valid());
}

TEST(AiColumnSchemaIntegration, column_serialization_retains_definition)
{
  AiColumnSchema definition;
  init(definition);
  ObColumnSchemaV2 original;
  original.set_column_id(17);
  ASSERT_EQ(OB_SUCCESS, original.set_ai_column_schema(definition));
  std::vector<char> buffer(original.get_serialize_size());
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, original.serialize(buffer.data(), buffer.size(), pos));
  EXPECT_EQ(buffer.size(), static_cast<size_t>(pos));
  ObColumnSchemaV2 decoded;
  pos = 0;
  ASSERT_EQ(OB_SUCCESS, decoded.deserialize(buffer.data(), buffer.size(), pos));
  std::fill(buffer.begin(), buffer.end(), 0);
  ASSERT_TRUE(decoded.is_ai_column());
  EXPECT_EQ(0, definition.get_expression().compare(decoded.get_ai_column_schema()->get_expression()));
}

TEST(AiColumnSchemaIntegration, non_ai_column_clears_definition_on_reuse)
{
  AiColumnSchema definition;
  init(definition);
  ObColumnSchemaV2 decoded;
  decoded.set_column_id(17);
  ASSERT_EQ(OB_SUCCESS, decoded.set_ai_column_schema(definition));
  ObColumnSchemaV2 ordinary;
  ordinary.set_column_id(16);
  std::vector<char> buffer(ordinary.get_serialize_size());
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, ordinary.serialize(buffer.data(), buffer.size(), pos));
  pos = 0;
  ASSERT_EQ(OB_SUCCESS, decoded.deserialize(buffer.data(), buffer.size(), pos));
  EXPECT_FALSE(decoded.is_ai_column());
  EXPECT_EQ(16U, decoded.get_column_id());
}

TEST(AiColumnSchemaIntegration, column_id_remapping_includes_definition_references)
{
  AiColumnSchema definition;
  init(definition);
  ObColumnSchemaV2 column;
  column.set_column_id(17);
  ASSERT_EQ(OB_SUCCESS, column.set_ai_column_schema(definition));
  hash::ObHashMap<uint64_t, uint64_t> mapping;
  ASSERT_EQ(OB_SUCCESS, mapping.create(8, "AiColTest"));
  for (uint64_t id = 16; id <= 20; ++id) {
    ASSERT_EQ(OB_SUCCESS, mapping.set_refactored(id,id+100));
  }
  ASSERT_EQ(OB_SUCCESS, column.convert_column_id(mapping));
  EXPECT_EQ(117U,column.get_column_id());
  ASSERT_TRUE(column.is_ai_column());
  EXPECT_EQ(117U,column.get_ai_column_schema()->get_column_id());
  EXPECT_EQ(118U,column.get_ai_column_schema()->get_state_column_id());
  EXPECT_EQ(119U,column.get_ai_column_schema()->get_request_column_id());
  EXPECT_EQ(120U,column.get_ai_column_schema()->get_result_spec_column_id());
  EXPECT_EQ(116U,column.get_ai_column_schema()->get_dependencies().at(0));
}

TEST(AiColumnSchemaIntegration, mismatched_definition_does_not_replace_metadata)
{
  AiColumnSchema definition;
  init(definition);
  ObColumnSchemaV2 column;
  column.set_column_id(17);
  ASSERT_EQ(OB_SUCCESS, column.set_ai_column_schema(definition));
  definition.set_column_id(21);
  EXPECT_EQ(OB_INVALID_ARGUMENT,column.set_ai_column_schema(definition));
  ASSERT_TRUE(column.is_ai_column());
  EXPECT_EQ(17U,column.get_ai_column_schema()->get_column_id());
}
