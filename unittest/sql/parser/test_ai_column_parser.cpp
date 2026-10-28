#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "sql/parser/ob_parser.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace {
const ParseNode *find_node(const ParseNode *node, ObItemType type)
{
  if (node == nullptr || node->type_ == type) { return node; }
  const ParseNode *found = nullptr;
  for (int i = 0; found == nullptr && i < node->num_child_; ++i) {
    found = find_node(node->children_[i], type);
  }
  return found;
}

class AiColumnParser : public ::testing::Test {
protected:
  ObArenaAllocator allocator_;
  ObParser parser_{allocator_, 0};
  ParseResult result_{};
  int parse(const char *sql) { return parser_.parse(ObString::make_string(sql), result_); }
  void TearDown() override { parser_.free_result(result_); }
};

TEST_F(AiColumnParser, create_preserves_expression_and_options)
{
  ASSERT_EQ(OB_SUCCESS, parse("CREATE TABLE docs(id BIGINT PRIMARY KEY, content TEXT, tags JSON AI COLUMN (AI_COMPLETE('text_model', CONCAT('Classify: ', content))) WITH (sync_mode='async', max_retries=2), emb VECTOR(4) AI COLUMN (AI_EMBED('embed_model', content)))"));
  const ParseNode *definition = find_node(result_.result_tree_, T_AI_COLUMN_DEFINITION);
  ASSERT_NE(nullptr, definition);
  ASSERT_EQ(2, definition->num_child_);
  const ParseNode *expr = definition->children_[0];
  ASSERT_NE(nullptr, expr);
  EXPECT_EQ("AI_COMPLETE('text_model', CONCAT('Classify: ', content))", std::string(expr->str_value_, expr->str_len_));
  const ParseNode *options = definition->children_[1];
  ASSERT_NE(nullptr, options);
  ASSERT_EQ(T_AI_COLUMN_OPTIONS, options->type_);
  ASSERT_EQ(2, options->num_child_);
  EXPECT_EQ(T_AI_COLUMN_OPTION, options->children_[0]->type_);
  EXPECT_EQ(2, options->children_[1]->children_[1]->value_);
}

TEST_F(AiColumnParser, add_column)
{
  ASSERT_EQ(OB_SUCCESS, parse("ALTER TABLE docs ADD COLUMN tags JSON AI COLUMN (AI_COMPLETE('m',content)) WITH (sync_mode='async',max_retries=0) AFTER content"));
  EXPECT_NE(nullptr, find_node(result_.result_tree_, T_COLUMN_ADD));
  EXPECT_NE(nullptr, find_node(result_.result_tree_, T_AI_COLUMN_DEFINITION));
  EXPECT_NE(nullptr, find_node(result_.result_tree_, T_COLUMN_ADD_AFTER));
}

TEST_F(AiColumnParser, modify_column)
{
  ASSERT_EQ(OB_SUCCESS, parse("ALTER TABLE docs MODIFY COLUMN tags VARCHAR(400) AI COLUMN (AI_COMPLETE('m',content))"));
  EXPECT_NE(nullptr, find_node(result_.result_tree_, T_COLUMN_MODIFY));
  const ParseNode *definition = find_node(result_.result_tree_, T_AI_COLUMN_DEFINITION);
  ASSERT_NE(nullptr, definition);
  EXPECT_EQ(nullptr, definition->children_[1]);
}

TEST_F(AiColumnParser, show_ai_columns)
{
  ASSERT_EQ(OB_SUCCESS, parse("SHOW AI COLUMNS FROM docs FROM test"));
  const ParseNode *show = find_node(result_.result_tree_, T_SHOW_AI_COLUMNS);
  ASSERT_NE(nullptr, show);
  EXPECT_EQ(3, show->num_child_);
}

TEST_F(AiColumnParser, retry_has_separate_statement_and_predicate)
{
  ASSERT_EQ(OB_SUCCESS, parse("ALTER TABLE test.docs RETRY AI COLUMN tags WHERE status='FAILED' AND id=2"));
  const ParseNode *retry = find_node(result_.result_tree_, T_RETRY_AI_COLUMN);
  ASSERT_NE(nullptr, retry);
  ASSERT_EQ(4, retry->num_child_);
  EXPECT_NE(nullptr, retry->children_[0]);
  EXPECT_NE(nullptr, retry->children_[1]);
  EXPECT_NE(nullptr, retry->children_[2]);
  EXPECT_EQ(nullptr, find_node(result_.result_tree_, T_ALTER_TABLE));
}

TEST_F(AiColumnParser, retry_retains_quoted_business_status)
{
  ASSERT_EQ(OB_SUCCESS, parse("ALTER TABLE docs RETRY AI COLUMN tags WHERE `status`='active'"));
  const ParseNode *retry = find_node(result_.result_tree_, T_RETRY_AI_COLUMN);
  ASSERT_NE(nullptr, retry);
  const ParseNode *column = find_node(retry->children_[2], T_COLUMN_REF);
  ASSERT_NE(nullptr, column);
  ASSERT_NE(nullptr, column->children_[2]);
  EXPECT_TRUE(column->children_[2]->is_input_quoted_);
}

TEST_F(AiColumnParser, retry_is_still_an_identifier)
{
  ASSERT_EQ(OB_SUCCESS, parse("SELECT retry, ai, `status` FROM retry"));
  EXPECT_NE(nullptr, find_node(result_.result_tree_, T_SELECT));
}

TEST_F(AiColumnParser, ordinary_generated_columns_unchanged)
{
  ASSERT_EQ(OB_SUCCESS, parse("CREATE TABLE t(a INT, b INT GENERATED ALWAYS AS (a+1) STORED)"));
  EXPECT_NE(nullptr, find_node(result_.result_tree_, T_GENERATED_COLUMN));
  EXPECT_EQ(nullptr, find_node(result_.result_tree_, T_AI_COLUMN_DEFINITION));
}

TEST_F(AiColumnParser, rejects_empty_ai_expression)
{
  EXPECT_NE(OB_SUCCESS, parse("CREATE TABLE t(a INT AI COLUMN ())"));
}

TEST_F(AiColumnParser, rejects_missing_option_value)
{
  EXPECT_NE(OB_SUCCESS, parse("CREATE TABLE t(a INT AI COLUMN (AI_COMPLETE('m','x')) WITH(max_retries=))"));
}

TEST_F(AiColumnParser, rejects_retry_missing_column)
{
  EXPECT_NE(OB_SUCCESS, parse("ALTER TABLE t RETRY AI COLUMN WHERE status='FAILED'"));
}

TEST_F(AiColumnParser, rejects_retry_with_multiple_columns)
{
  EXPECT_NE(OB_SUCCESS, parse("ALTER TABLE t RETRY AI COLUMN a,b WHERE status='FAILED'"));
}
TEST_F(AiColumnParser, mixed_retry_is_rejected_by_resolver_not_misparsed)
{
  ASSERT_EQ(OB_SUCCESS, parse("ALTER TABLE docs RETRY AI COLUMN tags WHERE id=1, ADD COLUMN x INT"));
  const ParseNode *retry = find_node(result_.result_tree_, T_RETRY_AI_COLUMN);
  ASSERT_NE(nullptr, retry);
  EXPECT_EQ(5, retry->num_child_);
}
} // namespace
