#pragma once

#include "share/schema/ob_schema_struct.h"

namespace oceanbase {
namespace share {
namespace schema {

// A definition, not a live task. Model calls belong to the post-commit worker.
// Stored result columns remain ordinary stored columns for MVCC and indexes.
class AiColumnSchema : public ObSchema
{
  OB_UNIS_VERSION(1);
public:
  AiColumnSchema() : ObSchema() { reset(); }
  explicit AiColumnSchema(common::ObIAllocator *allocator) : ObSchema(allocator) { reset(); }
  AiColumnSchema(const AiColumnSchema &) = delete;
  AiColumnSchema &operator=(const AiColumnSchema &) = delete;
  virtual ~AiColumnSchema() = default;
  virtual void reset() override;
  virtual bool is_valid() const override;
  virtual int64_t get_convert_size() const override;
  int assign(const AiColumnSchema &other);
  int convert_column_ids(const common::hash::ObHashMap<uint64_t, uint64_t> &mapping);

  int set_expression(const common::ObString &value) { return deep_copy_str(value, expression_); }
  int set_normalized_expr(const common::ObString &value) { return deep_copy_str(value, normalized_expr_); }
  int set_dependencies(const common::ObIArray<uint64_t> &ids) { return dependency_ids_.assign(ids); }
  const common::ObString &get_expression() const { return expression_; }
  const common::ObString &get_normalized_expr() const { return normalized_expr_; }
  const common::ObIArray<uint64_t> &get_dependencies() const { return dependency_ids_; }
  void set_column_id(uint64_t value) { column_id_ = value; }
  uint64_t get_column_id() const { return column_id_; }
  void set_state_column_id(uint64_t value) { state_column_id_ = value; }
  uint64_t get_state_column_id() const { return state_column_id_; }
  void set_request_column_id(uint64_t value) { request_column_id_ = value; }
  uint64_t get_request_column_id() const { return request_column_id_; }
  void set_result_spec_column_id(uint64_t value) { result_spec_column_id_ = value; }
  uint64_t get_result_spec_column_id() const { return result_spec_column_id_; }
  void set_definition_version(int64_t value) { definition_version_ = value; }
  int64_t get_definition_version() const { return definition_version_; }
  void set_max_retries(int64_t value) { max_retries_ = value; }
  int64_t get_max_retries() const { return max_retries_; }
  TO_STRING_KV(K_(column_id), K_(definition_version), K_(max_retries),
               K_(state_column_id), K_(request_column_id), K_(result_spec_column_id),
               K_(expression), K_(normalized_expr), K_(dependency_ids));
private:
  uint64_t column_id_;
  int64_t definition_version_;
  int64_t max_retries_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  common::ObString expression_;
  common::ObString normalized_expr_;
  common::ObSEArray<uint64_t, 4> dependency_ids_;
};

} // namespace schema
} // namespace share
} // namespace oceanbase
