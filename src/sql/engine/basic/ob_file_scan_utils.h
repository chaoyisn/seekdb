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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_

#include <sys/stat.h>
#include <stdint.h>
#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"
#include "lib/container/ob_se_array.h"
#include "lib/allocator/ob_allocator.h"
#include "common/object/ob_object.h"
#include "common/row/ob_row.h"
#include "sql/parser/parse_node.h"
#include "sql/engine/expr/ob_expr.h"
#include "sql/engine/basic/ob_batch_result_holder.h"

namespace oceanbase
{
namespace sql
{

class ObSQLSessionInfo;
class ObExecContext;
class ObResolverParams;
class ObDMLResolver;
class ObDMLStmt;
class ObSelectStmt;
class ObStmt;
struct TableItem;
class ObRawExpr;
class ObRawExprFactory;
class ObOperator;
struct ObBatchRows;

enum ObFileFormatType : int32_t
{
  OB_FILE_FORMAT_INVALID = 0,
  OB_FILE_FORMAT_CSV     = 1,
  OB_FILE_FORMAT_JSONL   = 2,
  OB_FILE_FORMAT_PARQUET = 3,
  OB_FILE_FORMAT_GLOB    = 4,
};

enum ObFileLogicalType : int32_t
{
  OB_FILE_TYPE_NULL     = 0,
  OB_FILE_TYPE_BOOLEAN  = 1,
  OB_FILE_TYPE_BIGINT   = 2,
  OB_FILE_TYPE_DOUBLE   = 3,
  OB_FILE_TYPE_DATE     = 4,
  OB_FILE_TYPE_DATETIME = 5,
  OB_FILE_TYPE_VARCHAR  = 6,
};

struct ObFileFingerprint
{
  int64_t size_;
  int64_t mtime_sec_;
  int64_t mtime_nsec_;
  uint64_t ino_;
  uint64_t dev_;
  bool is_valid_;

  ObFileFingerprint()
    : size_(0), mtime_sec_(0), mtime_nsec_(0), ino_(0), dev_(0), is_valid_(false)
  {}

  TO_STRING_KV(K_(size), K_(mtime_sec), K_(mtime_nsec), K_(ino), K_(dev), K_(is_valid));
};

struct ObFileScanOptions
{
  ObFileFormatType format_;
  common::ObString file_path_;
  common::ObString canonical_path_;
  bool header_;
  char delimiter_;
  char quote_;
  char escape_;
  common::ObString nullstr_;
  int64_t sample_size_;
  bool auto_detect_;
  common::ObString json_format_;
  ObFileFingerprint fingerprint_;

  ObFileScanOptions()
    : format_(OB_FILE_FORMAT_INVALID),
      file_path_(),
      canonical_path_(),
      header_(true),
      delimiter_(','),
      quote_('"'),
      escape_('"'),
      nullstr_(),
      sample_size_(-1),
      auto_detect_(true),
      json_format_(common::ObString::make_string("newline_delimited")),
      fingerprint_()
  {}

  TO_STRING_KV(K_(format), K_(file_path), K_(canonical_path), K_(header),
               K_(delimiter), K_(quote), K_(escape), K_(nullstr),
               K_(sample_size), K_(auto_detect), K_(json_format), K_(fingerprint));
};

struct ObFileColumnDef
{
  common::ObString name_;
  ObFileLogicalType logical_type_;
  common::ObObjMeta meta_;
  common::ObAccuracy accuracy_;
  bool is_nullable_;
  int32_t parquet_col_idx_;

  ObFileColumnDef()
    : name_(),
      logical_type_(OB_FILE_TYPE_VARCHAR),
      meta_(),
      accuracy_(),
      is_nullable_(true),
      parquet_col_idx_(-1)
  {}

  TO_STRING_KV(K_(name), K_(logical_type), K_(meta), K_(accuracy),
               K_(is_nullable), K_(parquet_col_idx));
};

struct ObFileTableDef
{
  static const uint64_t MAGIC_HEADER = 0x46494C4553514C31ULL; // "FILESQL1"
  uint64_t magic_;
  ObFileScanOptions options_;
  common::ObSEArray<ObFileColumnDef, 16, common::ModulePageAllocator, true> columns_;
  common::ObSEArray<int32_t, 16, common::ModulePageAllocator, true> access_col_indices_;

  ObFileTableDef()
    : magic_(MAGIC_HEADER),
      options_(),
      columns_(),
      access_col_indices_()
  {}

  TO_STRING_KV(K_(magic), K_(options), K_(columns), K_(access_col_indices));
};

struct ObCopyCsvOptions
{
  common::ObString target_path_;
  common::ObString canonical_dir_;
  bool header_;
  char delimiter_;
  char quote_;
  char escape_;
  common::ObString nullstr_;
  common::ObSEArray<common::ObString, 16, common::ModulePageAllocator, true> header_names_;
  common::ObSEArray<bool, 16, common::ModulePageAllocator, true> is_bool_cols_;

  ObCopyCsvOptions()
    : target_path_(),
      canonical_dir_(),
      header_(false),
      delimiter_(','),
      quote_('"'),
      escape_('"'),
      nullstr_(),
      header_names_(),
      is_bool_cols_()
  {}

  TO_STRING_KV(K_(target_path), K_(canonical_dir), K_(header),
               K_(delimiter), K_(quote), K_(escape), K_(nullstr));
};

class ObIFileScanner
{
public:
  static const int64_t FILE_SCANNER_SENTINEL = -777777;
  virtual ~ObIFileScanner() {}
  virtual int open() = 0;
  virtual int get_next_row(const common::ObIArray<ObExpr *> &column_exprs,
                           ObEvalCtx &eval_ctx) = 0;
  virtual int rescan() = 0;
  virtual int close() = 0;
};

class ObFileScanUtils
{
public:
  static const char *const FILE_TABLE_EXPR_PREFIX;
  static const int64_t FILE_TABLE_EXPR_PREFIX_LEN = 10; // "\x01FILE_SQL:"
  static const char *const COPY_CSV_SPEC_PREFIX;
  static const int64_t COPY_CSV_SPEC_PREFIX_LEN = 10;   // "\x01COPY_CSV:"

  // System variable enable_file_sql & secure_file_priv helpers
  static bool is_file_sql_enabled(const ObSQLSessionInfo *session, bool is_global = false);
  static int set_file_sql_enabled(ObSQLSessionInfo *session,
                                  bool is_global,
                                  bool is_set_default,
                                  const common::ObObj &value_obj);
  static int check_file_sql_enabled(const ObSQLSessionInfo *session);

  static void set_global_secure_file_priv_override(const common::ObString &val);
  static int get_effective_secure_file_priv(const ObSQLSessionInfo *session,
                                            common::ObIAllocator &allocator,
                                            common::ObString &secure_file_priv);

  // Path & security validation
  static int validate_secure_read_path(const common::ObString &secure_file_priv,
                                       const common::ObString &input_path,
                                       common::ObIAllocator &allocator,
                                       common::ObString &canonical_path,
                                       ObFileFingerprint &fingerprint);
  static int validate_secure_glob_pattern(const common::ObString &secure_file_priv,
                                          const common::ObString &pattern,
                                          common::ObIAllocator &allocator,
                                          common::ObSEArray<common::ObString, 16> &matched_files);
  static int validate_secure_write_path(const common::ObString &secure_file_priv,
                                        const common::ObString &target_path,
                                        common::ObIAllocator &allocator,
                                        common::ObString &resolved_target_path);
  static int verify_file_fingerprint(const common::ObString &canonical_path,
                                     const ObFileFingerprint &expected);

  // Type inference & schema deduplication
  static ObFileLogicalType infer_scalar_string_type(const common::ObString &str,
                                                    bool is_null,
                                                    bool is_quoted_in_json = false);
  static ObFileLogicalType merge_logical_types(ObFileLogicalType t1, ObFileLogicalType t2);
  static void logical_type_to_ob_meta(ObFileLogicalType logical_type,
                                      common::ObObjMeta &meta,
                                      common::ObAccuracy &accuracy);
  static const char *logical_type_to_describe_name(ObFileLogicalType logical_type);
  static const char *obj_meta_to_describe_name(const common::ObObjMeta &meta,
                                               const common::ObAccuracy &accuracy);
  static int deduplicate_column_names(common::ObIAllocator &allocator,
                                      common::ObIArray<ObFileColumnDef> &columns);

  // Strict value parsing into ObDatum
  static bool parse_strict_bool(const common::ObString &str, int8_t &out_val);
  static bool parse_strict_int64(const common::ObString &str, int64_t &out_val);
  static bool parse_strict_double(const common::ObString &str, double &out_val);
  static bool parse_strict_date(const common::ObString &str, int32_t &out_days);
  static bool parse_strict_datetime(const common::ObString &str, int64_t &out_usec);
  static bool is_valid_utf8_no_nul(const char *buf, int64_t len);

  // Parse tree option parsing & schema inference
  static int parse_file_table_node(const ParseNode &func_node,
                                   common::ObIAllocator &allocator,
                                   ObFileScanOptions &options);
  static int infer_file_table_schema(common::ObIAllocator &allocator,
                                     const ObSQLSessionInfo *session,
                                     ObFileTableDef &table_def);
  static int infer_csv_schema(common::ObIAllocator &allocator,
                              ObFileTableDef &table_def);
  static int infer_jsonl_schema(common::ObIAllocator &allocator,
                                ObFileTableDef &table_def);
  static int infer_parquet_schema(common::ObIAllocator &allocator,
                                  ObFileTableDef &table_def);
  static int infer_glob_schema(common::ObIAllocator &allocator,
                               const ObSQLSessionInfo *session,
                               ObFileTableDef &table_def);

  // Encoding/decoding ObFileTableDef* in TableItem::function_table_expr_
  static int encode_file_table_expr(common::ObIAllocator &allocator,
                                    ObRawExprFactory &expr_factory,
                                    ObFileTableDef *table_def,
                                    ObRawExpr *&out_expr);
  static bool is_file_table_expr(const ObRawExpr *expr, ObFileTableDef *&out_def);
  static bool is_file_table_item(const TableItem &table_item, ObFileTableDef **out_def = NULL);
  static bool extract_file_table_def_from_datum(const common::ObDatum &datum,
                                                ObFileTableDef *&out_def);

  // Resolver hooks
  static int resolve_file_table_item(ObResolverParams &params,
                                     ObDMLResolver &resolver,
                                     const ParseNode &parse_tree,
                                     ObDMLStmt *stmt,
                                     TableItem *&tbl_item);
  static int resolve_describe_stmt(ObResolverParams &params,
                                   const ParseNode &parse_tree,
                                   ObStmt *&stmt);
  static int resolve_copy_stmt(ObResolverParams &params,
                               const ParseNode &parse_tree,
                               ObStmt *&stmt);

  // Scanner factory & COPY execution
  static int create_file_scanner(common::ObIAllocator &allocator,
                                 ObExecContext &exec_ctx,
                                 const ObFileTableDef &table_def,
                                 ObIFileScanner *&scanner);
  static bool is_copy_csv_spec(const common::ObObj &outfile_name);
  static int execute_copy_to_csv(ObExecContext &ctx,
                                 ObEvalCtx &eval_ctx,
                                 ObOperator *child,
                                 const common::ObIArray<ObExpr *> &select_exprs,
                                 const common::ObString &encoded_spec,
                                 int64_t top_limit_cnt,
                                 ObBatchRows &brs,
                                 bool is_batch);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
