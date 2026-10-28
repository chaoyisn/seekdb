#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ai_column_retry_resolver.h"
#include "sql/resolver/ob_resolver_define.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/parser/ob_parser.h"
#include "share/schema/ob_ai_column_schema.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase { namespace sql {
using namespace common;
using namespace share::schema;
namespace {
int identifier(ObSqlString &sql, ObString value)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    if (value.ptr()[i] == '`') { OZ(sql.append("`")); }
    OZ(sql.append(value.ptr() + i, 1));
  }
  OZ(sql.append("`"));
  return ret;
}
ParseNode *find(ParseNode *node, ObItemType type)
{
  if (node == nullptr || node->type_ == type) { return node; }
  ParseNode *found = nullptr;
  for (int i = 0; found == nullptr && i < node->num_child_; ++i) {
    found = find(node->children_[i], type);
  }
  return found;
}
int replace_status(ObResolverParams &params, ParseNode *&node, const ObSqlString &state_sql)
{
  int ret = OB_SUCCESS;
  if (node == nullptr || node->type_ == T_SELECT) { return ret; }
  if (node->type_ == T_COLUMN_REF && node->num_child_ == 3
      && node->children_[0] == nullptr && node->children_[1] == nullptr
      && node->children_[2] != nullptr && !node->children_[2]->is_input_quoted_
      && ObString(node->children_[2]->str_len_, node->children_[2]->str_value_).case_compare("status") == 0) {
    // Parse a native CASE expression rather than interpolate the user's predicate.
    ParseResult parsed;
    ObParser parser(*params.allocator_, params.session_info_->get_sql_mode());
    OZ(parser.parse(state_sql.string(), parsed));
    if (OB_SUCC(ret)) {
      ParseNode *expression = find(parsed.result_tree_, T_CASE);
      if (expression == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else { node = expression; }
    }
  } else {
    for (int i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      OZ(replace_status(params, node->children_[i], state_sql));
    }
  }
  return ret;
}
} // namespace

int AiColumnRetryResolver::rewrite(ObResolverParams &params, const ParseNode &retry, ParseNode *&update)
{
  int ret = OB_SUCCESS;
  update = nullptr;
  if (retry.num_child_ != 4) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "RETRY AI COLUMN mixed with another ALTER action");
    return OB_NOT_SUPPORTED;
  }
  if (retry.children_ == nullptr || retry.children_[0] == nullptr || retry.children_[1] == nullptr
      || params.allocator_ == nullptr || params.schema_checker_ == nullptr
      || params.session_info_ == nullptr || params.query_ctx_ == nullptr) { return OB_ERR_UNEXPECTED; }
  const ParseNode &relation = *retry.children_[0];
  if (relation.type_ != T_RELATION_FACTOR || relation.num_child_ < 2 || relation.children_[1] == nullptr) {
    return OB_ERR_UNEXPECTED;
  }
  const ObString database = relation.children_[0] == nullptr ? params.session_info_->get_database_name()
      : ObString(relation.children_[0]->str_len_, relation.children_[0]->str_value_);
  const ObString name(relation.children_[1]->str_len_, relation.children_[1]->str_value_);
  const ParseNode *column_node = retry.children_[1];
  // COLUMN_NAME uses the regular T_COLUMN_REF node.
  if (column_node->type_ == T_COLUMN_REF && column_node->num_child_ >= 3) { column_node = column_node->children_[2]; }
  if (column_node == nullptr) { return OB_ERR_UNEXPECTED; }
  const ObString column_name(column_node->str_len_, column_node->str_value_);
  const ObTableSchema *table = nullptr;
  OZ(params.schema_checker_->get_table_schema(database, name, false, table));
  const auto *column = OB_SUCC(ret) && table != nullptr ? table->get_column_schema(column_name) : nullptr;
  const auto *ai = column == nullptr ? nullptr : column->get_ai_column_schema();
  if (OB_SUCC(ret) && ai == nullptr) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "RETRY requires an AI column");
  }
  const auto *state = ai == nullptr ? nullptr : table->get_column_schema(ai->get_state_column_id());
  if (OB_SUCC(ret) && state == nullptr) { ret = OB_SCHEMA_EAGAIN; }
  ObSqlString sql, state_sql;
  OZ(sql.append("UPDATE /*+ USE_PLAN_CACHE(NONE) NO_PARALLEL */ "));
  OZ(identifier(sql, database)); OZ(sql.append(".")); OZ(identifier(sql, name)); OZ(sql.append(" SET "));
  if (OB_SUCC(ret)) { OZ(identifier(sql, column->get_column_name_str())); }
  OZ(sql.append("="));
  if (OB_SUCC(ret)) { OZ(identifier(sql, column->get_column_name_str())); }
  OZ(state_sql.append("SELECT CASE "));
  if (OB_SUCC(ret)) { OZ(identifier(state_sql, state->get_column_name_str())); }
  OZ(state_sql.append(" WHEN 0 THEN 'PENDING' WHEN 1 THEN 'READY' WHEN 2 THEN 'FAILED' END"));
  ParseResult parsed;
  ObParser parser(*params.allocator_, params.session_info_->get_sql_mode());
  OZ(parser.parse(sql.string(), parsed));
  if (OB_SUCC(ret)) {
    update = find(parsed.result_tree_, T_UPDATE);
    if (update == nullptr || update->num_child_ != 11) { ret = OB_ERR_UNEXPECTED; }
    else {
      update->children_[3] = retry.children_[2];
      OZ(replace_status(params, update->children_[3], state_sql));
    }
  }
  if (OB_SUCC(ret)) {
    params.query_ctx_->ai_column_retry_table_id_ = table->get_table_id();
    params.query_ctx_->ai_column_retry_column_id_ = ai->get_column_id();
  }
  return ret;
}
} } // namespace oceanbase::sql
