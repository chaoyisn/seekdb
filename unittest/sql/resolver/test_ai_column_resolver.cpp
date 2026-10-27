#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/ddl/ob_ai_column_options.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace {
const ParseNode *find_options(const ParseNode *node)
{
  if (node == nullptr || node->type_ == T_AI_COLUMN_DEFINITION) { return node; }
  const ParseNode *found = nullptr;
  for (int i = 0; found == nullptr && i < node->num_child_; ++i) {
    found = find_options(node->children_[i]);
  }
  return found;
}

class AiColumnOptionsTest : public ::testing::Test {
protected:
  ObArenaAllocator allocator_;
  ObParser parser_{allocator_, 0};
  ParseResult result_{};
  AiColumnOptions options_;
  int resolve(const char *sql)
  {
    int ret = parser_.parse(ObString::make_string(sql), result_);
    if (ret == OB_SUCCESS) {
      const ParseNode *def = find_options(result_.result_tree_);
      ret = def == nullptr ? OB_ERR_UNEXPECTED : AiColumnOptions::resolve(def->children_[1], options_);
    }
    return ret;
  }
  void TearDown() override { parser_.free_result(result_); }
};

TEST_F(AiColumnOptionsTest, defaults)
{
  EXPECT_EQ(OB_SUCCESS, resolve("CREATE TABLE t(a INT AI COLUMN(AI_COMPLETE('m','x')))"));
  EXPECT_EQ(2, options_.max_retries_);
}

TEST_F(AiColumnOptionsTest, zero_retries_and_case_insensitive_mode)
{
  EXPECT_EQ(OB_SUCCESS, resolve("CREATE TABLE t(a INT AI COLUMN(AI_COMPLETE('m','x')) WITH(SYNC_MODE='ASYNC',max_retries=0))"));
  EXPECT_EQ(0, options_.max_retries_);
}

TEST_F(AiColumnOptionsTest, duplicate_mode)
{
  EXPECT_EQ(OB_INVALID_ARGUMENT, resolve("CREATE TABLE t(a INT AI COLUMN(AI_COMPLETE('m','x')) WITH(sync_mode='async',SYNC_MODE='async'))"));
}

TEST_F(AiColumnOptionsTest, duplicate_retries_does_not_modify_output)
{
  options_.max_retries_ = 9;
  EXPECT_EQ(OB_INVALID_ARGUMENT, resolve("CREATE TABLE t(a INT AI COLUMN(AI_COMPLETE('m','x')) WITH(max_retries=1,MAX_RETRIES=2))"));
  EXPECT_EQ(9, options_.max_retries_);
}

TEST_F(AiColumnOptionsTest, synchronous_mode_rejected)
{
  EXPECT_EQ(OB_INVALID_ARGUMENT, resolve("CREATE TABLE t(a INT AI COLUMN(AI_COMPLETE('m','x')) WITH(sync_mode='sync'))"));
}

TEST_F(AiColumnOptionsTest, negative_retries)
{
  EXPECT_EQ(OB_INVALID_ARGUMENT, resolve("CREATE TABLE t(a INT AI COLUMN(AI_COMPLETE('m','x')) WITH(max_retries=-1))"));
}

TEST_F(AiColumnOptionsTest, noninteger_retries)
{
  EXPECT_EQ(OB_INVALID_ARGUMENT, resolve("CREATE TABLE t(a INT AI COLUMN(AI_COMPLETE('m','x')) WITH(max_retries=1.5))"));
}

TEST_F(AiColumnOptionsTest, null_mode)
{
  EXPECT_EQ(OB_INVALID_ARGUMENT, resolve("CREATE TABLE t(a INT AI COLUMN(AI_COMPLETE('m','x')) WITH(sync_mode=NULL))"));
}

TEST_F(AiColumnOptionsTest, unknown_option)
{
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("CREATE TABLE t(a INT AI COLUMN(AI_COMPLETE('m','x')) WITH(unknown_option=1))"));
}

TEST_F(AiColumnOptionsTest, unsigned_overflow_retries)
{
  EXPECT_EQ(OB_INVALID_ARGUMENT, resolve("CREATE TABLE t(a INT AI COLUMN(AI_COMPLETE('m','x')) WITH(max_retries=18446744073709551615))"));
}
} // namespace
