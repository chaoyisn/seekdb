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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (nullptr != param.base_param_->relevance_expr_
      && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  active_mask_ = 0;
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
  active_mask_ = 0;
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  // For a few terms, keep one head per posting list and select the minimum
  // directly. Preserve the heap path for boolean, lookup and larger queries.
  if (IS_NOT_INIT || 0 == dim_iters_->count()
      || dim_iters_->count() > SMALL_MERGE_MAX_DIMS
      || mode_flag_ != sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE
      || function_lookup_mode_) {
    return ObSRDaaTIterImpl::get_next_rows(capacity, count);
  }
  int ret = OB_SUCCESS;
  count = 0;
  if (OB_UNLIKELY(0 == capacity)) {
  } else if (iter_param_->limit_param_->is_valid()
      && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(pre_process())) {
  } else {
    const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
    ObSRMergeItem candidate;
    ObSRMergeItem minimum;
    const int64_t dim_count = dim_iters_->count();
    ObTextRetrievalDaaTTokenIter *token_iters[SMALL_MERGE_MAX_DIMS];
    for (int64_t i = 0; i < dim_count; ++i) {
      token_iters[i] = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i));
    }
    // These fixed arrays are allocated once for the dimension count. Their
    // storage stays valid while the posting iterators refill their own batches.
    int64_t *next_round = &next_round_iter_idxes_.at(0);
    const ObDatum **domain_ids = &iter_domain_ids_.at(0);
    while (OB_SUCC(ret) && count < real_capacity) {
      ObSRMergeItem item;
      for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
        const int64_t idx = next_round[i];
        ObTextRetrievalDaaTTokenIter *iter = token_iters[idx];
        if (OB_ISNULL(iter)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(iter->get_next_row_with_info(item.relevance_, domain_ids[idx]))) {
          if (OB_ITER_END == ret) {
            active_mask_ &= ~(1ULL << idx);
            ret = OB_SUCCESS;
          }
        } else {
          if (nullptr != iter_param_->dim_weights_) {
            item.relevance_ = item.relevance_ * iter_param_->field_boost_
                * iter_param_->dim_weights_->at(idx);
          }
          active_scores_[idx] = item.relevance_;
          active_mask_ |= 1ULL << idx;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (0 == active_mask_) {
        ret = OB_ITER_END;
      } else {
        next_round_cnt_ = 0;
        uint64_t matches = 0;
        for (int64_t i = 0; OB_SUCC(ret) && i < dim_count; ++i) {
          const uint64_t bit = 1ULL << i;
          if (0 == (active_mask_ & bit)) {
          } else if (0 == matches) {
            minimum.iter_idx_ = i;
            matches = bit;
          } else {
            int64_t cmp = 0;
            candidate.iter_idx_ = i;
            if (OB_FAIL(merge_cmp_.cmp(candidate, minimum, cmp))) {
            } else if (cmp < 0) {
              minimum.iter_idx_ = i;
              matches = bit;
            } else if (0 == cmp) {
              matches |= bit;
            }
          }
        }
        // Equal IDs are accumulated in iterator order, as in the loser tree.
        for (int64_t i = 0; OB_SUCC(ret) && i < dim_count; ++i) {
          if (0 != (matches & (1ULL << i))) {
            if (OB_FAIL(relevance_collector_->collect_one_dim(i, active_scores_[i]))) {
            } else {
              next_round[next_round_cnt_++] = i;
            }
          }
        }
        active_mask_ &= ~matches;
        double relevance = 0.0;
        bool need_project = false;
        if (OB_SUCC(ret)) {
          const ObDatum &id = *domain_ids[minimum.iter_idx_];
          if (OB_FAIL(relevance_collector_->get_result(relevance, need_project))) {
          } else if (need_project && OB_FAIL(process_collected_row(id, relevance))) {
          } else if (need_project && OB_FAIL(filter_on_demand(count, relevance, need_project))) {
          } else if (need_project && OB_FAIL(cache_result(count, id, relevance))) {
          }
        }
      }
    }
    if ((OB_SUCC(ret) || OB_ITER_END == ret) && count > 0 && OB_FAIL(project_results(count))) {
    }
  }
  return ret;
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (nullptr != iter_param_->relevance_expr_) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
