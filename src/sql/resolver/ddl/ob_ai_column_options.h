#pragma once

#include "lib/ob_errno.h"
#include "lib/string/ob_string.h"
#include "sql/parser/parse_node.h"

namespace oceanbase {
namespace sql {

struct AiColumnOptions
{
  // The public contract permits only async. Keep one execution mode rather
  // than adding an unused synchronous path. Two retries is the default policy.
  int64_t max_retries_ = 2;

  static int resolve(const ParseNode *node, AiColumnOptions &options)
  {
    int ret = common::OB_SUCCESS;
    AiColumnOptions resolved;
    bool saw_mode = false;
    bool saw_retries = false;
    if (node != nullptr && (node->type_ != T_AI_COLUMN_OPTIONS || node->num_child_ <= 0)) {
      ret = common::OB_INVALID_ARGUMENT;
    }
    for (int i = 0; ret == common::OB_SUCCESS && node != nullptr && i < node->num_child_; ++i) {
      const ParseNode *option = node->children_[i];
      if (option == nullptr || option->type_ != T_AI_COLUMN_OPTION || option->num_child_ != 2
          || option->children_[0] == nullptr || option->children_[1] == nullptr) {
        ret = common::OB_INVALID_ARGUMENT;
      } else {
        const ParseNode &key = *option->children_[0];
        const ParseNode &value = *option->children_[1];
        const common::ObString name(static_cast<int32_t>(key.str_len_), key.str_value_);
        if (name.case_compare("sync_mode") == 0) {
          if (saw_mode || value.type_ != T_VARCHAR
              || common::ObString(static_cast<int32_t>(value.str_len_), value.str_value_).case_compare("async") != 0) {
            ret = common::OB_INVALID_ARGUMENT;
          }
          saw_mode = true;
        } else if (name.case_compare("max_retries") == 0) {
          if (saw_retries || value.type_ != T_INT || value.value_ < 0) {
            ret = common::OB_INVALID_ARGUMENT;
          } else {
            resolved.max_retries_ = value.value_;
          }
          saw_retries = true;
        } else {
          ret = common::OB_NOT_SUPPORTED;
        }
      }
    }
    // Do not expose partially parsed configuration when any option failed.
    if (ret == common::OB_SUCCESS) { options = resolved; }
    return ret;
  }
};

} // namespace sql
} // namespace oceanbase
