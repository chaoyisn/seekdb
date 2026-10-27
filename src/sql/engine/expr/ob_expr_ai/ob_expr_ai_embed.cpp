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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include <algorithm>
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (!ob_is_text_tc(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    }
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (OB_NOT_NULL(arg_dim) && (arg_dim->is_null() || arg_dim->get_int() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content;
    ret = ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, temp_allocator,
        *arg_content, expr.args_[CONTENT_IDX]->datum_meta_,
        expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content);
    if (OB_SUCC(ret) && (model_id.empty() || content.empty())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->is_null() ? 0 : arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

// Batch only expressions with a common model/configuration. Varying models retain scalar semantics.
int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
    const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObDatum *results = expr.locate_batch_datums(ctx);
  ObBitVector &evaluated = expr.get_evaluated_flags(ctx);
  ObEvalCtx::TempAllocGuard guard(ctx);
  MultimodeAlloctor allocator(guard.get_allocator());
  ObArray<ObString> inputs;
  ObArray<int64_t> rows;
  bool has_active_row = false;
  if (OB_FAIL(expr.args_[CONTENT_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      if (skip.at(i) || evaluated.at(i)) { continue; }
      has_active_row = true;
      ObDatum &input = expr.args_[CONTENT_IDX]->locate_expr_datum(ctx, i);
      if (input.is_null()) {
        results[i].set_null();
        evaluated.set(i);
      } else {
        ObString text;
        if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator, input,
            expr.args_[CONTENT_IDX]->datum_meta_, expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), text))) {
        } else if (text.empty()) { ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(inputs.push_back(text))) {
        } else { ret = rows.push_back(i); }
      }
    }
  }
  if (OB_SUCC(ret) && has_active_row) {
    ObDatum *model_datum = nullptr;
    ObDatum *dimension_datum = nullptr;
    ObJsonObject *config = nullptr;
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint;
    query::ObIAiEndpointResolver *resolver = share::server_service<query::ObIAiEndpointResolver>();
    if (OB_FAIL(expr.args_[MODEL_IDX]->eval(ctx, model_datum))) {
    } else if (expr.arg_cnt_ == 3 && OB_FAIL(expr.args_[DIM_IDX]->eval(ctx, dimension_datum))) {
    } else if (OB_NOT_NULL(dimension_datum)) {
      ObJsonInt *dimension = nullptr;
      if (dimension_datum->is_null() || dimension_datum->get_int() <= 0) {
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimension_datum->get_int(), dimension))) {
      } else { ret = config->add("dimensions", dimension); }
    }
    if (OB_FAIL(ret)) {
    } else if (model_datum->is_null()) {
      for (int64_t i = 0; i < rows.count(); ++i) {
        results[rows.at(i)].set_null();
        evaluated.set(rows.at(i));
      }
    } else if (inputs.empty()) {
      // Validate explicit dimensions even when all active content values are SQL NULL.
    } else {
      ObString model_id = model_datum->get_string();
      if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
      } else if (OB_ISNULL(resolver)) { ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(resolver->resolve_by_model_name(model_id, allocator, endpoint))) {
      } else {
        ObAIFuncModel model(allocator, *info, endpoint);
        const int64_t request_size = 64;
        int64_t inferred_dimension = 0;
        for (int64_t start = 0; OB_SUCC(ret) && start < inputs.count(); start += request_size) {
          ObArray<ObString> batch;
          ObArray<ObString> vectors;
          const int64_t count = std::min(request_size, inputs.count() - start);
          for (int64_t j = 0; OB_SUCC(ret) && j < count; ++j) { ret = batch.push_back(inputs.at(start + j)); }
          if (OB_SUCC(ret)) { ret = model.call_dense_embedding_vector_v2(batch, config, vectors); }
          // A server may change its default dimension between requests. Keep this SQL batch consistent.
          for (int64_t j = 0; OB_SUCC(ret) && j < vectors.count(); ++j) {
            ObIJsonBase *vector = nullptr;
            if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, vectors.at(j),
                ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, vector))) {
            } else if (inferred_dimension == 0) { inferred_dimension = vector->element_count();
            } else if (inferred_dimension != vector->element_count()) { ret = OB_INVALID_DATA; }
          }
          for (int64_t j = 0; OB_SUCC(ret) && j < vectors.count(); ++j) {
            const int64_t row = rows.at(start + j);
            const ObString &vector = vectors.at(j);
            char *buffer = expr.get_str_res_mem(ctx, vector.length(), row);
            if (OB_ISNULL(buffer)) { ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              MEMCPY(buffer, vector.ptr(), vector.length());
              results[row].set_string(buffer, vector.length());
              evaluated.set(row);
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    if (rt_expr.is_batch_result() && rt_expr.args_[CONTENT_IDX]->is_batch_result()
        && !rt_expr.args_[MODEL_IDX]->is_batch_result()
        && (rt_expr.arg_cnt_ == 2 || !rt_expr.args_[DIM_IDX]->is_batch_result())) {
      rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
