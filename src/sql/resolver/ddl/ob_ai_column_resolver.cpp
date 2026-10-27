#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_ai_column_resolver.h"
#include "sql/resolver/ddl/ob_ai_column_options.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/printer/ob_raw_expr_printer.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "query/vector/ob_vector_index_util.h"
#include "share/schema/ob_schema_getter_guard.h"

namespace oceanbase {
namespace sql {
using namespace common;
using namespace share::schema;

const ParseNode *AiColumnResolver::find_definition(const ParseNode &column_node)
{
  const ParseNode *definition = nullptr;
  if (column_node.type_ == T_COLUMN_DEFINITION && column_node.num_child_ >= 3
      && column_node.children_[2] != nullptr) {
    const ParseNode &attributes = *column_node.children_[2];
    for (int i = 0; definition == nullptr && i < attributes.num_child_; ++i) {
      if (attributes.children_[i] != nullptr && attributes.children_[i]->type_ == T_AI_COLUMN_DEFINITION) {
        definition = attributes.children_[i];
      }
    }
  }
  return definition;
}

namespace {
bool contains_id(const ObIArray<uint64_t> &ids, uint64_t value)
{
  bool found = false;
  for (int64_t i = 0; !found && i < ids.count(); ++i) { found = ids.at(i) == value; }
  return found;
}

int validate_input_tree(ObRawExpr &root)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr *, 16> pending;
  if (OB_FAIL(pending.push_back(&root))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < pending.count(); ++i) {
    ObRawExpr *expr = pending.at(i);
    if (expr == nullptr) {
      ret = OB_ERR_UNEXPECTED;
    } else if (expr->is_query_ref_expr() || expr->is_aggr_expr() || expr->is_win_func_expr()
               || expr->is_udf_expr() || expr->is_pseudo_column_expr()
               || expr->get_expr_type() == T_OP_ASSIGN
               || expr->get_expr_type() == T_OP_GET_USER_VAR
               || expr->get_expr_type() == T_OP_GET_SYS_VAR
               || expr->get_expr_type() == T_OP_GET_PACKAGE_VAR
               || expr->get_expr_type() == T_OP_GET_SUBPROGRAM_VAR
               || expr->get_expr_type() == T_FUN_SYS_AI_COMPLETE
               || expr->get_expr_type() == T_FUN_SYS_AI_EMBED
               || expr->get_expr_type() == T_FUN_SYS_AI_RERANK) {
      ret = OB_NOT_SUPPORTED;
    } else if (OB_FAIL(ObRawExprUtils::check_deterministic_single(
                            expr, ObResolverUtils::CHECK_FOR_GENERATED_COLUMN))) {
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < expr->get_param_count(); ++j) {
      ret = pending.push_back(expr->get_param_expr(j));
    }
  }
  return ret;
}

int append_expr(ObResolverParams &params, ObRawExpr *expr, char *buffer,
                int64_t capacity, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  ObRawExprPrinter printer(buffer, capacity, &pos, params.schema_checker_->get_schema_guard(),
                           params.session_info_->get_tz_info_wrap().get_time_zone_info());
  if (expr == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(printer.do_print(expr, T_NONE_SCOPE, true))) {
  } else {
    ret = sql.append(buffer, pos);
  }
  return ret;
}
} // namespace

int AiColumnResolver::resolve(ObResolverParams &params, const ParseNode &definition_node,
                              const ObTableSchema &table, const ObColumnSchemaV2 &column,
                              const ObIArray<uint64_t> &ai_column_ids,
                              uint64_t &next_column_id, AiColumnSchema &definition)
{
  int ret = OB_SUCCESS;
  AiColumnOptions options;
  const ParseNode *node = nullptr;
  const ParseNode *arguments = nullptr;
  bool embedding = false;
  int64_t dimension = 0;
  const char *hint = nullptr;
  if (params.allocator_ == nullptr || params.expr_factory_ == nullptr
      || params.session_info_ == nullptr || params.schema_checker_ == nullptr
      || definition_node.type_ != T_AI_COLUMN_DEFINITION || definition_node.num_child_ != 2
      || (node = definition_node.children_[0]) == nullptr) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(AiColumnOptions::resolve(definition_node.children_[1], options))) {
  } else if (node->type_ != T_FUN_SYS || node->num_child_ != 2
             || node->children_[0] == nullptr || (arguments = node->children_[1]) == nullptr) {
    ret = OB_NOT_SUPPORTED;
  } else {
    const ParseNode &name = *node->children_[0];
    ObString function(static_cast<int32_t>(name.str_len_), name.str_value_);
    embedding = function.case_compare("AI_EMBED") == 0;
    if (!embedding && function.case_compare("AI_COMPLETE") != 0) {
      ret = OB_NOT_SUPPORTED;
    } else if (arguments->num_child_ < 2 || arguments->num_child_ > 3
               || arguments->children_[0] == nullptr
               || arguments->children_[0]->type_ != T_VARCHAR
               || arguments->children_[0]->str_len_ == 0) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  if (OB_SUCC(ret) && embedding) {
    if (!column.is_collection()) {
      ret = OB_NOT_SUPPORTED;
    } else if (OB_FAIL(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
                             column.get_extended_type_info(), dimension))) {
    } else if (dimension <= 0) {
      ret = OB_ERR_INVALID_VECTOR_DIM;
    } else if (arguments->num_child_ == 3) {
      const ParseNode *dim = arguments->children_[2];
      if (dim == nullptr || dim->type_ != T_INT || dim->value_ != dimension) {
        ret = OB_ERR_INVALID_VECTOR_DIM;
      }
    }
  } else if (OB_SUCC(ret)) {
    if (column.is_json()) {
      hint = "Return exactly one valid JSON value. Do not include markdown or explanation.";
    } else if (ob_is_integer_type(column.get_data_type())) {
      hint = "Return exactly one base-10 integer. Do not include markdown or explanation.";
    } else if (column.get_meta_type().is_numeric_type()) {
      hint = "Return exactly one finite number. Do not include markdown or explanation.";
    } else if (column.is_string_type() || ob_is_text_tc(column.get_data_type())) {
      hint = "Return only the generated text. Do not include markdown or explanation.";
    } else {
      ret = OB_NOT_SUPPORTED;
    }
  }
  if (OB_SUCC(ret)) {
    const ParseNode &model = *arguments->children_[0];
    ObString model_name(static_cast<int32_t>(model.str_len_), model.str_value_);
    ObAIFuncExprInfo *info = nullptr;
    if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*params.allocator_, model_name,
                    *params.schema_checker_->get_schema_guard(), info))) {
    } else if (info == nullptr || (embedding ? !ObAIFuncUtils::is_dense_embedding_type(info)
                                           : !ObAIFuncUtils::is_completion_type(info))) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  ObRawExpr *expression = nullptr;
  ObSEArray<ObQualifiedName, 8> references;
  ObSEArray<uint64_t, 8> dependencies;
  if (OB_SUCC(ret) && OB_FAIL(ObRawExprUtils::build_generated_column_expr(
          *params.expr_factory_, *params.session_info_, *node, expression, references,
          &table, nullptr, params.schema_checker_, ObResolverUtils::DISABLE_CHECK))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < references.count(); ++i) {
    const ObQualifiedName &name = references.at(i);
    const ObColumnSchemaV2 *input = table.get_column_schema(name.col_name_);
    if (name.is_sys_func() || !name.database_name_.empty() || !name.tbl_name_.empty()
        || input == nullptr || input->is_hidden()) {
      ret = OB_ERR_BAD_FIELD_ERROR;
      LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, name.col_name_.length(), name.col_name_.ptr(),
                     9, "AI column");
    } else if (input->get_column_id() == column.get_column_id() || input->is_ai_column()
               || input->is_generated_column() || contains_id(ai_column_ids, input->get_column_id())) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI column dependency on an AI or generated column");
    } else if (OB_FAIL(ObRawExprUtils::init_column_expr(*input, params.session_info_, *name.ref_expr_))) {
    } else if (!contains_id(dependencies, input->get_column_id())) {
      ret = dependencies.push_back(input->get_column_id());
    }
  }
  if (OB_SUCC(ret) && expression == nullptr) { ret = OB_ERR_UNEXPECTED; }
  for (int64_t i = 0; OB_SUCC(ret) && i < expression->get_param_count(); ++i) {
    ObRawExpr *input = expression->get_param_expr(i);
    ret = input == nullptr ? OB_ERR_UNEXPECTED : validate_input_tree(*input);
  }
  if (OB_SUCC(ret) && OB_FAIL(expression->formalize(params.session_info_))) {
  }
  ObSqlString normalized;
  if (OB_SUCC(ret)) {
    const int64_t capacity = MIN(OB_MAX_SQL_LENGTH, node->str_len_ * 4 + 4096);
    char *buffer = static_cast<char *>(params.allocator_->alloc(capacity));
    if (buffer == nullptr) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (expression->get_param_count() < 2 || expression->get_param_count() > 3) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(normalized.append(embedding ? "AI_EMBED(" : "AI_COMPLETE("))) {
    } else if (OB_FAIL(append_expr(params, expression->get_param_expr(0), buffer, capacity, normalized))) {
    } else if (OB_FAIL(normalized.append(","))) {
    } else if (OB_FAIL(append_expr(params, expression->get_param_expr(1), buffer, capacity, normalized))) {
    } else if (embedding) {
      ret = normalized.append_fmt(",%ld)", dimension);
    } else if (OB_FAIL(normalized.append(",JSON_SET(COALESCE("))) {
    } else if (expression->get_param_count() == 3
               && OB_FAIL(append_expr(params, expression->get_param_expr(2), buffer, capacity, normalized))) {
    } else if (expression->get_param_count() == 2 && OB_FAIL(normalized.append("NULL"))) {
    } else {
      ret = normalized.append_fmt(",JSON_OBJECT()),'$.__ai_column_system_prompt','%s'))", hint);
    }
  }
  if (OB_SUCC(ret)) {
    definition.reset();
    definition.set_column_id(column.get_column_id());
    definition.set_definition_version(column.get_schema_version());
    definition.set_max_retries(options.max_retries_);
    definition.set_state_column_id(++next_column_id);
    definition.set_request_column_id(++next_column_id);
    definition.set_result_spec_column_id(++next_column_id);
    if (OB_FAIL(definition.set_expression(ObString(static_cast<int32_t>(node->str_len_), node->str_value_)))) {
    } else if (OB_FAIL(definition.set_normalized_expr(normalized.string()))) {
    } else if (OB_FAIL(definition.set_dependencies(dependencies))) {
    } else if (!definition.is_valid()) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int AiColumnResolver::make_hidden_columns(const AiColumnSchema &definition,
                                         ObIArray<ObColumnSchemaV2> &columns)
{
  int ret = definition.is_valid() ? OB_SUCCESS : OB_INVALID_ARGUMENT;
  const uint64_t ids[] = {definition.get_state_column_id(), definition.get_request_column_id(),
                          definition.get_result_spec_column_id()};
  const char *names[] = {"__ai_state_", "__ai_request_", "__ai_result_spec_"};
  for (int i = 0; OB_SUCC(ret) && i < 3; ++i) {
    ObColumnSchemaV2 column;
    ObSqlString name;
    column.set_column_id(ids[i]);
    column.set_is_hidden(true);
    column.set_nullable(i != 0);
    if (i == 0) {
      column.set_data_type(ObIntType);
      ObObj pending;
      pending.set_int(0);
      if (OB_FAIL(column.set_orig_default_value(pending))) {
      } else if (OB_FAIL(column.set_cur_default_value(pending, false))) {
      }
    } else {
      column.set_data_type(ObVarcharType);
      column.set_data_length(i == 1 ? 64 : 4096);
      column.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      column.set_charset_type(CHARSET_UTF8MB4);
    }
    if (OB_SUCC(ret) && OB_FAIL(name.append_fmt("%s%lu", names[i], definition.get_column_id()))) {
    } else if (OB_SUCC(ret) && OB_FAIL(column.set_column_name(name.string()))) {
    } else if (OB_SUCC(ret)) {
      ret = columns.push_back(column);
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
