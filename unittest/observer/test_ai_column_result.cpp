#include <gtest/gtest.h>
#include "observer/ai_column_worker.h"
#include "share/schema/ob_column_schema.h"
#include "lib/allocator/page_arena.h"

using namespace oceanbase::common;
using namespace oceanbase::observer;
using namespace oceanbase::share::schema;

class AiColumnResultTest : public ::testing::Test {
protected:
  ObArenaAllocator allocator_;
  ObColumnSchemaV2 column_;
  int check(ObObjType type, const char *value, int64_t length = 1024) {
    column_.set_data_type(type);
    column_.set_data_length(length);
    return AiColumnWorker::validate_result(column_, false, ObString::make_string(value), allocator_);
  }
  int vector(const char *value) {
    column_.set_data_type(ObCollectionSQLType);
    ObSEArray<ObString,1> info;
    EXPECT_EQ(OB_SUCCESS,info.push_back(ObString::make_string("VECTOR(4)")));
    EXPECT_EQ(OB_SUCCESS,column_.set_extended_type_info(info));
    return AiColumnWorker::validate_result(column_,false,ObString::make_string(value),allocator_);
  }
};

TEST_F(AiColumnResultTest, signed_integer_boundaries) {
  EXPECT_EQ(OB_SUCCESS,check(ObIntType,"-9223372036854775808"));
  EXPECT_EQ(OB_SUCCESS,check(ObIntType,"9223372036854775807"));
  EXPECT_EQ(OB_SUCCESS,check(ObIntType," \t-7\r\n"));
  EXPECT_NE(OB_SUCCESS,check(ObIntType,"9223372036854775808"));
}
TEST_F(AiColumnResultTest, integer_requires_entire_response) {
  for (const char *value : {"", " ", "7oops", "7.0", "1e2", "```7```", "7 8"}) {
    EXPECT_NE(OB_SUCCESS,check(ObIntType,value)) << value;
  }
}
TEST_F(AiColumnResultTest, narrow_integer_ranges) {
  EXPECT_EQ(OB_SUCCESS,check(ObTinyIntType,"-128"));
  EXPECT_EQ(OB_SUCCESS,check(ObTinyIntType,"127"));
  EXPECT_NE(OB_SUCCESS,check(ObTinyIntType,"128"));
  EXPECT_NE(OB_SUCCESS,check(ObTinyIntType,"-129"));
  EXPECT_EQ(OB_SUCCESS,check(ObMediumIntType,"8388607"));
  EXPECT_NE(OB_SUCCESS,check(ObMediumIntType,"8388608"));
}
TEST_F(AiColumnResultTest, unsigned_integer_ranges) {
  EXPECT_EQ(OB_SUCCESS,check(ObUInt64Type,"18446744073709551615"));
  EXPECT_NE(OB_SUCCESS,check(ObUInt64Type,"18446744073709551616"));
  EXPECT_NE(OB_SUCCESS,check(ObUInt64Type,"-1"));
  EXPECT_EQ(OB_SUCCESS,check(ObUTinyIntType,"255"));
  EXPECT_NE(OB_SUCCESS,check(ObUTinyIntType,"256"));
}
TEST_F(AiColumnResultTest, floating_point_must_be_finite) {
  EXPECT_EQ(OB_SUCCESS,check(ObDoubleType,"-1.25e2"));
  for (const char *value : {"nan", "inf", "-inf", "1e999", "1.2x"}) {
    EXPECT_NE(OB_SUCCESS,check(ObDoubleType,value)) << value;
  }
  EXPECT_NE(OB_SUCCESS,check(ObFloatType,"1e39"));
  EXPECT_NE(OB_SUCCESS,check(ObUDoubleType,"-1"));
}
TEST_F(AiColumnResultTest, json_accepts_all_json_value_kinds) {
  for (const char *value : {"null", "7", "true", "\"text\"", "[1,2]", "{\"input\":\"x\"}"}) {
    EXPECT_EQ(OB_SUCCESS,check(ObJsonType,value)) << value;
  }
}
TEST_F(AiColumnResultTest, malformed_json_is_rejected) {
  for (const char *value : {"", "{", "{\"x\":}", "```json\n{}\n```", "{}tail"}) {
    EXPECT_NE(OB_SUCCESS,check(ObJsonType,value)) << value;
  }
}
TEST_F(AiColumnResultTest, text_preserves_empty_and_enforces_capacity) {
  EXPECT_EQ(OB_SUCCESS,check(ObVarcharType,"",4));
  EXPECT_EQ(OB_SUCCESS,check(ObVarcharType,"four",4));
  EXPECT_NE(OB_SUCCESS,check(ObVarcharType,"longer",4));
  EXPECT_EQ(OB_SUCCESS,check(ObLongTextType,"a longtext response"));
}
TEST_F(AiColumnResultTest, vector_checks_dimensions_and_numeric_elements) {
  EXPECT_EQ(OB_SUCCESS,vector("[1,2,3,4]"));
  EXPECT_EQ(OB_SUCCESS,vector("[0.1,-0.2,3e-2,4]"));
  EXPECT_NE(OB_SUCCESS,vector("[1,2,3]"));
  EXPECT_NE(OB_SUCCESS,vector("{\"x\":[1,2,3,4]}"));
  EXPECT_NE(OB_SUCCESS,vector("[1,2,3,null]"));
  EXPECT_NE(OB_SUCCESS,vector("[1,2,3,\"4\"]"));
  EXPECT_NE(OB_SUCCESS,vector("[1,2,3,1e999]"));
}
TEST_F(AiColumnResultTest, decimal_requires_numeric_json) {
  EXPECT_EQ(OB_SUCCESS,check(ObNumberType,"-123.456"));
  EXPECT_NE(OB_SUCCESS,check(ObNumberType,"1e"));
  EXPECT_NE(OB_SUCCESS,check(ObNumberType,"{\"number\":1}"));
}
TEST_F(AiColumnResultTest, sql_null_is_a_valid_result) {
  for (ObObjType type : {ObJsonType,ObIntType,ObVarcharType,ObCollectionSQLType}) {
    column_.set_data_type(type);
    EXPECT_EQ(OB_SUCCESS,AiColumnWorker::validate_result(column_,true,ObString(),allocator_));
  }
}
