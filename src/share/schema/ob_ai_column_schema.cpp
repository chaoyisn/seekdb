#define USING_LOG_PREFIX SHARE_SCHEMA
#include "share/schema/ob_ai_column_schema.h"

namespace oceanbase {
namespace share {
namespace schema {
using namespace common;

void AiColumnSchema::reset()
{
  column_id_ = state_column_id_ = request_column_id_ = result_spec_column_id_ = OB_INVALID_ID;
  definition_version_ = 0;
  max_retries_ = 2;
  reset_string(expression_);
  reset_string(normalized_expr_);
  dependency_ids_.reset();
  ObSchema::reset();
}

bool AiColumnSchema::is_valid() const
{
  bool valid = ObSchema::is_valid() && !expression_.empty() && !normalized_expr_.empty()
      && definition_version_ >= 0 && max_retries_ >= 0;
  const uint64_t ids[] = {column_id_, state_column_id_, request_column_id_, result_spec_column_id_};
  for (int i = 0; valid && i < 4; ++i) {
    valid = ids[i] != OB_INVALID_ID && ids[i] >= OB_APP_MIN_COLUMN_ID;
    for (int j = 0; valid && j < i; ++j) { valid = ids[i] != ids[j]; }
  }
  for (int64_t i = 0; valid && i < dependency_ids_.count(); ++i) {
    const uint64_t id = dependency_ids_.at(i);
    valid = id != OB_INVALID_ID && id >= OB_APP_MIN_COLUMN_ID;
    for (int j = 0; valid && j < 4; ++j) { valid = id != ids[j]; }
    for (int64_t j = 0; valid && j < i; ++j) { valid = id != dependency_ids_.at(j); }
  }
  return valid;
}

int64_t AiColumnSchema::get_convert_size() const
{
  return sizeof(*this) + expression_.length() + normalized_expr_.length() + 2
      + dependency_ids_.count() * sizeof(uint64_t);
}

int AiColumnSchema::assign(const AiColumnSchema &other)
{
  int ret = OB_SUCCESS;
  if (this != &other) {
    reset();
    column_id_ = other.column_id_;
    definition_version_ = other.definition_version_;
    max_retries_ = other.max_retries_;
    state_column_id_ = other.state_column_id_;
    request_column_id_ = other.request_column_id_;
    result_spec_column_id_ = other.result_spec_column_id_;
    if (OB_FAIL(set_expression(other.expression_))) {
    } else if (OB_FAIL(set_normalized_expr(other.normalized_expr_))) {
    } else if (OB_FAIL(set_dependencies(other.dependency_ids_))) {
    }
    error_ret_ = ret == OB_SUCCESS ? other.error_ret_ : ret;
  }
  return ret;
}

int AiColumnSchema::convert_column_ids(const hash::ObHashMap<uint64_t, uint64_t> &mapping)
{
  int ret = OB_SUCCESS;
  const uint64_t old_ids[] = {column_id_, state_column_id_, request_column_id_, result_spec_column_id_};
  uint64_t new_ids[4] = {};
  ObSEArray<uint64_t, 4> dependencies;
  for (int i = 0; OB_SUCC(ret) && i < 4; ++i) {
    if (OB_FAIL(mapping.get_refactored(old_ids[i], new_ids[i]))) {
    } else if (new_ids[i] == OB_INVALID_ID || new_ids[i] < OB_APP_MIN_COLUMN_ID) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (int j = 0; OB_SUCC(ret) && j < i; ++j) {
      if (new_ids[i] == new_ids[j]) { ret = OB_INVALID_ARGUMENT; }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < dependency_ids_.count(); ++i) {
    uint64_t id = 0;
    if (OB_FAIL(mapping.get_refactored(dependency_ids_.at(i), id))) {
    } else if (id == OB_INVALID_ID || id < OB_APP_MIN_COLUMN_ID) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      for (int j = 0; OB_SUCC(ret) && j < 4; ++j) {
        if (id == new_ids[j]) { ret = OB_INVALID_ARGUMENT; }
      }
      for (int64_t j = 0; OB_SUCC(ret) && j < dependencies.count(); ++j) {
        if (id == dependencies.at(j)) { ret = OB_INVALID_ARGUMENT; }
      }
      if (OB_SUCC(ret)) { ret = dependencies.push_back(id); }
    }
  }
  // Look up and validate every reference before changing any stored ID.
  if (OB_SUCC(ret) && OB_SUCC(ret = dependency_ids_.assign(dependencies))) {
    column_id_ = new_ids[0];
    state_column_id_ = new_ids[1];
    request_column_id_ = new_ids[2];
    result_spec_column_id_ = new_ids[3];
  }
  return ret;
}

OB_DEF_SERIALIZE(AiColumnSchema)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_ENCODE, column_id_, definition_version_, max_retries_,
              state_column_id_, request_column_id_, result_spec_column_id_,
              expression_, normalized_expr_, dependency_ids_);
  return ret;
}

OB_DEF_DESERIALIZE(AiColumnSchema)
{
  int ret = OB_SUCCESS;
  ObString expression;
  ObString normalized_expr;
  reset();
  LST_DO_CODE(OB_UNIS_DECODE, column_id_, definition_version_, max_retries_,
              state_column_id_, request_column_id_, result_spec_column_id_,
              expression, normalized_expr, dependency_ids_);
  if (OB_SUCC(ret) && OB_FAIL(set_expression(expression))) {
  } else if (OB_SUCC(ret) && OB_FAIL(set_normalized_expr(normalized_expr))) {
  }
  if (OB_SUCC(ret) && !is_valid()) { ret = OB_INVALID_ARGUMENT; }
  if (OB_FAIL(ret)) { reset(); error_ret_ = ret; }
  return ret;
}

OB_DEF_SERIALIZE_SIZE(AiColumnSchema)
{
  int64_t len = 0;
  LST_DO_CODE(OB_UNIS_ADD_LEN, column_id_, definition_version_, max_retries_,
              state_column_id_, request_column_id_, result_spec_column_id_,
              expression_, normalized_expr_, dependency_ids_);
  return len;
}

} // namespace schema
} // namespace share
} // namespace oceanbase
