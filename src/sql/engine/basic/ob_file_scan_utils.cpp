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

#include "sql/engine/basic/ob_file_scan_utils.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <glob.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include "lib/charset/ob_charset.h"
#include "lib/compress/zstd_1_3_8/ob_zstd_compressor_1_3_8.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_schema_utils.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_operator.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/parser/ob_parser.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;

namespace sql
{

const char *const ObFileScanUtils::FILE_TABLE_EXPR_PREFIX = "\x01" "FILE_SQL:";
const char *const ObFileScanUtils::COPY_CSV_SPEC_PREFIX   = "\x01" "COPY_CSV:";

namespace
{

static std::mutex g_secure_file_priv_mutex;
static bool g_has_secure_file_priv_override = false;
static std::string g_secure_file_priv_override;

static bool parse_bool_token(const ObString &s, bool &out_val)
{
  ObString trimmed = s;
  trimmed = trimmed.trim();
  if (0 == trimmed.case_compare("true") ||
      0 == trimmed.case_compare("on") ||
      0 == trimmed.case_compare("1")) {
    out_val = true;
    return true;
  } else if (0 == trimmed.case_compare("false") ||
             0 == trimmed.case_compare("off") ||
             0 == trimmed.case_compare("0")) {
    out_val = false;
    return true;
  }
  return false;
}

static void fill_fingerprint_from_stat(const struct stat &st, ObFileFingerprint &fp)
{
  fp.size_ = static_cast<int64_t>(st.st_size);
  fp.mtime_sec_ = static_cast<int64_t>(st.st_mtime);
#if defined(__linux__)
  fp.mtime_nsec_ = static_cast<int64_t>(st.st_mtim.tv_nsec);
#else
  fp.mtime_nsec_ = 0;
#endif
  fp.ino_ = static_cast<uint64_t>(st.st_ino);
  fp.dev_ = static_cast<uint64_t>(st.st_dev);
  fp.is_valid_ = true;
}

static bool is_canonical_subpath(const char *root_dir, const char *target_path)
{
  if (NULL == root_dir || NULL == target_path) {
    return false;
  }
  const size_t root_len = strlen(root_dir);
  const size_t target_len = strlen(target_path);
  if (0 == root_len || target_len < root_len) {
    return false;
  }
  if (0 != strncmp(root_dir, target_path, root_len)) {
    return false;
  }
  if (root_len == 1 && root_dir[0] == '/') {
    return true;
  }
  if (target_len == root_len) {
    return true;
  }
  return target_path[root_len] == '/';
}

static bool has_dotdot_segment(const ObString &path)
{
  const char *p = path.ptr();
  const int64_t n = path.length();
  if (NULL == p || n <= 0) {
    return false;
  }
  for (int64_t i = 0; i < n; ++i) {
    if (p[i] == '.' && i + 1 < n && p[i + 1] == '.') {
      const bool left_ok = (i == 0 || p[i - 1] == '/' || p[i - 1] == '\\');
      const bool right_ok = (i + 2 == n || p[i + 2] == '/' || p[i + 2] == '\\');
      if (left_ok && right_ok) {
        return true;
      }
    }
  }
  return false;
}

// Pure C++ Snappy decompressor for Parquet SNAPPY pages
static int snappy_decompress_buffer(const uint8_t *src,
                                    int64_t src_len,
                                    uint8_t *dst,
                                    int64_t dst_capacity,
                                    int64_t &out_len)
{
  out_len = 0;
  if (NULL == src || src_len <= 0 || NULL == dst || dst_capacity < 0) {
    return OB_INVALID_DATA;
  }
  int64_t pos = 0;
  uint64_t uncompressed_len = 0;
  int shift = 0;
  while (pos < src_len) {
    uint8_t b = src[pos++];
    uncompressed_len |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) {
      break;
    }
    shift += 7;
    if (shift >= 35 || pos >= src_len) {
      return OB_INVALID_DATA;
    }
  }
  if (static_cast<int64_t>(uncompressed_len) > dst_capacity) {
    return OB_INVALID_DATA;
  }
  int64_t op = 0;
  const int64_t target_len = static_cast<int64_t>(uncompressed_len);
  while (pos < src_len) {
    const uint8_t tag = src[pos++];
    const uint8_t tag_type = tag & 0x03;
    if (tag_type == 0) {
      // Literal
      uint32_t len = (tag >> 2) + 1;
      if (len > 60) {
        const uint32_t extra_bytes = len - 60;
        if (pos + static_cast<int64_t>(extra_bytes) > src_len) {
          return OB_INVALID_DATA;
        }
        uint32_t extra_len = 0;
        for (uint32_t i = 0; i < extra_bytes; ++i) {
          extra_len |= static_cast<uint32_t>(src[pos++]) << (8 * i);
        }
        len = extra_len + 1;
      }
      if (pos + static_cast<int64_t>(len) > src_len ||
          op + static_cast<int64_t>(len) > target_len) {
        return OB_INVALID_DATA;
      }
      MEMCPY(dst + op, src + pos, len);
      pos += len;
      op += len;
    } else {
      uint32_t copy_len = 0;
      uint32_t copy_offset = 0;
      if (tag_type == 1) {
        if (pos + 1 > src_len) {
          return OB_INVALID_DATA;
        }
        copy_len = ((tag >> 2) & 0x07) + 4;
        copy_offset = (static_cast<uint32_t>(tag & 0xE0) << 3) | src[pos++];
      } else if (tag_type == 2) {
        if (pos + 2 > src_len) {
          return OB_INVALID_DATA;
        }
        copy_len = (tag >> 2) + 1;
        copy_offset = static_cast<uint32_t>(src[pos]) |
                      (static_cast<uint32_t>(src[pos + 1]) << 8);
        pos += 2;
      } else {
        if (pos + 4 > src_len) {
          return OB_INVALID_DATA;
        }
        copy_len = (tag >> 2) + 1;
        copy_offset = static_cast<uint32_t>(src[pos]) |
                      (static_cast<uint32_t>(src[pos + 1]) << 8) |
                      (static_cast<uint32_t>(src[pos + 2]) << 16) |
                      (static_cast<uint32_t>(src[pos + 3]) << 24);
        pos += 4;
      }
      if (copy_offset == 0 ||
          static_cast<int64_t>(copy_offset) > op ||
          op + static_cast<int64_t>(copy_len) > target_len) {
        return OB_INVALID_DATA;
      }
      for (uint32_t i = 0; i < copy_len; ++i) {
        dst[op] = dst[op - copy_offset];
        ++op;
      }
    }
  }
  if (op != target_len) {
    return OB_INVALID_DATA;
  }
  out_len = op;
  return OB_SUCCESS;
}

} // anonymous namespace

bool ObFileScanUtils::is_file_sql_enabled(const ObSQLSessionInfo *session, bool is_global)
{
  bool enabled = false;
  if (OB_NOT_NULL(session)) {
    if (is_global) {
      ObObj val;
      ObArenaAllocator alloc("FileSqlSysVar");
      ObDataTypeCastParams dtc_params = session->get_dtc_params();
      if (OB_SUCCESS == ObBasicSessionInfo::get_global_sys_variable(alloc, dtc_params, share::SYS_VAR_ENABLE_FILE_SQL, val)) {
        int64_t int_val = 0;
        if (OB_SUCCESS == val.get_int(int_val)) {
          return int_val != 0;
        }
      }
    } else {
      if (OB_SUCCESS == session->get_enable_file_sql(enabled)) {
        return enabled;
      }
    }
  }
  return false;
}

int ObFileScanUtils::set_file_sql_enabled(ObSQLSessionInfo *session,
                                          bool is_global,
                                          bool is_set_default,
                                          const ObObj &value_obj)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  if (is_set_default) {
    enabled = false;
  } else if (value_obj.is_integer_type()) {
    const int64_t v = value_obj.get_int();
    if (v == 0 || v == 1) {
      enabled = (v == 1);
    } else {
      ret = OB_ERR_WRONG_VALUE_FOR_VAR;
    }
  } else if (value_obj.is_varchar_or_char()) {
    if (!parse_bool_token(value_obj.get_varchar(), enabled)) {
      ret = OB_ERR_WRONG_VALUE_FOR_VAR;
    }
  } else {
    ret = OB_ERR_WRONG_VALUE_FOR_VAR;
  }
  if (OB_SUCC(ret) && OB_NOT_NULL(session)) {
    ret = session->update_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled ? 1 : 0);
  }
  return ret;
}

int ObFileScanUtils::check_file_sql_enabled(const ObSQLSessionInfo *session)
{
  if (!is_file_sql_enabled(session, false)) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql is disabled, set enable_file_sql = ON first");
    return OB_NOT_SUPPORTED;
  }
  return OB_SUCCESS;
}

void ObFileScanUtils::set_global_secure_file_priv_override(const ObString &val)
{
  std::lock_guard<std::mutex> lock(g_secure_file_priv_mutex);
  g_has_secure_file_priv_override = true;
  if (val.empty() || NULL == val.ptr()) {
    g_secure_file_priv_override.clear();
  } else {
    g_secure_file_priv_override.assign(val.ptr(), static_cast<size_t>(val.length()));
  }
}

int ObFileScanUtils::get_effective_secure_file_priv(const ObSQLSessionInfo *session,
                                                    ObIAllocator &allocator,
                                                    ObString &secure_file_priv)
{
  int ret = OB_SUCCESS;
  secure_file_priv.reset();
  {
    std::lock_guard<std::mutex> lock(g_secure_file_priv_mutex);
    if (g_has_secure_file_priv_override) {
      ObString tmp(static_cast<int32_t>(g_secure_file_priv_override.size()),
                   g_secure_file_priv_override.data());
      return ob_write_string(allocator, tmp, secure_file_priv, true);
    }
  }
  if (OB_NOT_NULL(GCTX.schema_service_)) {
    if (OB_SUCCESS == ObSchemaUtils::get_runtime_varchar_variable(
            *GCTX.schema_service_, SYS_VAR_SECURE_FILE_PRIV, allocator, secure_file_priv)) {
      return OB_SUCCESS;
    }
  }
  if (OB_NOT_NULL(session)) {
    ObString sess_priv;
    if (OB_SUCCESS == session->get_secure_file_priv(sess_priv)) {
      return ob_write_string(allocator, sess_priv, secure_file_priv, true);
    }
  }
  return ob_write_string(allocator, ObString::make_string("/"), secure_file_priv, true);
}

int ObFileScanUtils::validate_secure_read_path(const ObString &secure_file_priv,
                                               const ObString &input_path,
                                               ObIAllocator &allocator,
                                               ObString &canonical_path,
                                               ObFileFingerprint &fingerprint)
{
  int ret = OB_SUCCESS;
  canonical_path.reset();
  fingerprint = ObFileFingerprint();

  ObString trimmed_path = input_path;
  trimmed_path = trimmed_path.trim();
  if (trimmed_path.empty() || !is_valid_utf8_no_nul(trimmed_path.ptr(), trimmed_path.length())) {
    return OB_INVALID_ARGUMENT;
  }
  if (secure_file_priv.empty() || 0 == secure_file_priv.case_compare("NULL")) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }
  if (secure_file_priv.length() >= PATH_MAX || trimmed_path.length() >= PATH_MAX) {
    return OB_INVALID_ARGUMENT;
  }

  char priv_buf[PATH_MAX + 1] = {0};
  MEMCPY(priv_buf, secure_file_priv.ptr(), secure_file_priv.length());
  char real_priv[PATH_MAX + 1] = {0};
  struct stat priv_st;
  if (0 != ::stat(priv_buf, &priv_st) || !S_ISDIR(priv_st.st_mode) ||
      NULL == ::realpath(priv_buf, real_priv)) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }

  char path_buf[PATH_MAX + 1] = {0};
  MEMCPY(path_buf, trimmed_path.ptr(), trimmed_path.length());
  if (path_buf[0] != '/') {
    if (has_dotdot_segment(trimmed_path)) {
      return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
    }
  }

  char real_target[PATH_MAX + 1] = {0};
  if (NULL == ::realpath(path_buf, real_target)) {
    const int saved_errno = errno;
    if (has_dotdot_segment(trimmed_path)) {
      return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
    }
    // Check if parent directory is outside real_priv
    const char *last_slash = strrchr(path_buf, '/');
    if (NULL != last_slash) {
      char parent_buf[PATH_MAX + 1] = {0};
      const size_t parent_len = (last_slash == path_buf) ? 1 : static_cast<size_t>(last_slash - path_buf);
      MEMCPY(parent_buf, path_buf, parent_len);
      char real_parent[PATH_MAX + 1] = {0};
      if (NULL != ::realpath(parent_buf, real_parent)) {
        if (!is_canonical_subpath(real_priv, real_parent)) {
          return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
        }
      } else if (0 != strncmp(path_buf, real_priv, strlen(real_priv))) {
        return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
      }
    }
    if (saved_errno == EACCES || saved_errno == EPERM) {
      return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
    }
    return OB_FILE_NOT_EXIST;
  }

  if (!is_canonical_subpath(real_priv, real_target)) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }

  struct stat target_st;
  if (0 != ::stat(real_target, &target_st)) {
    return OB_FILE_NOT_EXIST;
  }
  if (!S_ISREG(target_st.st_mode)) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }

  fill_fingerprint_from_stat(target_st, fingerprint);
  ret = ob_write_string(allocator, ObString::make_string(real_target), canonical_path, true);
  return ret;
}

int ObFileScanUtils::validate_secure_glob_pattern(const ObString &secure_file_priv,
                                                  const ObString &pattern,
                                                  ObIAllocator &allocator,
                                                  ObSEArray<ObString, 16> &matched_files)
{
  int ret = OB_SUCCESS;
  matched_files.reuse();

  ObString trimmed = pattern;
  trimmed = trimmed.trim();
  if (trimmed.empty() || !is_valid_utf8_no_nul(trimmed.ptr(), trimmed.length())) {
    return OB_INVALID_ARGUMENT;
  }
  if (secure_file_priv.empty() || 0 == secure_file_priv.case_compare("NULL")) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }
  if (secure_file_priv.length() >= PATH_MAX || trimmed.length() >= PATH_MAX) {
    return OB_INVALID_ARGUMENT;
  }
  if (has_dotdot_segment(trimmed)) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }

  char priv_buf[PATH_MAX + 1] = {0};
  MEMCPY(priv_buf, secure_file_priv.ptr(), secure_file_priv.length());
  char real_priv[PATH_MAX + 1] = {0};
  struct stat priv_st;
  if (0 != ::stat(priv_buf, &priv_st) || !S_ISDIR(priv_st.st_mode) ||
      NULL == ::realpath(priv_buf, real_priv)) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }

  char pat_buf[PATH_MAX + 1] = {0};
  MEMCPY(pat_buf, trimmed.ptr(), trimmed.length());

  // Extract static directory prefix before the first wildcard ('*', '?', '[')
  int64_t first_wild = trimmed.length();
  for (int64_t i = 0; i < trimmed.length(); ++i) {
    const char c = pat_buf[i];
    if (c == '*' || c == '?' || c == '[') {
      first_wild = i;
      break;
    }
  }
  int64_t last_slash_before_wild = -1;
  for (int64_t i = first_wild - 1; i >= 0; --i) {
    if (pat_buf[i] == '/') {
      last_slash_before_wild = i;
      break;
    }
  }
  char prefix_dir[PATH_MAX + 1] = {0};
  if (last_slash_before_wild < 0) {
    STRCPY(prefix_dir, ".");
  } else if (last_slash_before_wild == 0) {
    STRCPY(prefix_dir, "/");
  } else {
    MEMCPY(prefix_dir, pat_buf, static_cast<size_t>(last_slash_before_wild));
  }

  char real_prefix_dir[PATH_MAX + 1] = {0};
  if (NULL == ::realpath(prefix_dir, real_prefix_dir)) {
    if (pat_buf[0] == '/' && !is_canonical_subpath(real_priv, prefix_dir)) {
      return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
    }
    // Non-existent subdirectory inside secure_file_priv -> empty result set
    return OB_SUCCESS;
  }
  if (!is_canonical_subpath(real_priv, real_prefix_dir)) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }

  glob_t glob_res;
  MEMSET(&glob_res, 0, sizeof(glob_res));
  const int glob_rc = ::glob(pat_buf, GLOB_NOSORT, NULL, &glob_res);
  if (0 == glob_rc) {
    std::vector<std::string> valid_paths;
    for (size_t i = 0; i < glob_res.gl_pathc; ++i) {
      const char *cand = glob_res.gl_pathv[i];
      if (NULL == cand) {
        continue;
      }
      char real_cand[PATH_MAX + 1] = {0};
      struct stat cand_st;
      if (NULL != ::realpath(cand, real_cand) &&
          is_canonical_subpath(real_priv, real_cand) &&
          0 == ::stat(real_cand, &cand_st) &&
          S_ISREG(cand_st.st_mode)) {
        valid_paths.emplace_back(real_cand);
      }
    }
    ::globfree(&glob_res);
    std::sort(valid_paths.begin(), valid_paths.end());
    valid_paths.erase(std::unique(valid_paths.begin(), valid_paths.end()), valid_paths.end());
    for (size_t i = 0; OB_SUCC(ret) && i < valid_paths.size(); ++i) {
      ObString copied;
      if (OB_FAIL(ob_write_string(allocator,
                                  ObString(static_cast<int32_t>(valid_paths[i].size()),
                                           valid_paths[i].data()),
                                  copied,
                                  true))) {
      } else if (OB_FAIL(matched_files.push_back(copied))) {
      }
    }
  } else {
    ::globfree(&glob_res);
    if (glob_rc != GLOB_NOMATCH) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int ObFileScanUtils::validate_secure_write_path(const ObString &secure_file_priv,
                                                const ObString &target_path,
                                                ObIAllocator &allocator,
                                                ObString &resolved_target_path)
{
  int ret = OB_SUCCESS;
  resolved_target_path.reset();

  ObString trimmed = target_path;
  trimmed = trimmed.trim();
  if (trimmed.empty() || !is_valid_utf8_no_nul(trimmed.ptr(), trimmed.length())) {
    return OB_INVALID_ARGUMENT;
  }
  if (secure_file_priv.empty() || 0 == secure_file_priv.case_compare("NULL")) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }
  if (secure_file_priv.length() >= PATH_MAX || trimmed.length() >= PATH_MAX) {
    return OB_INVALID_ARGUMENT;
  }
  if (has_dotdot_segment(trimmed)) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }

  char priv_buf[PATH_MAX + 1] = {0};
  MEMCPY(priv_buf, secure_file_priv.ptr(), secure_file_priv.length());
  char real_priv[PATH_MAX + 1] = {0};
  struct stat priv_st;
  if (0 != ::stat(priv_buf, &priv_st) || !S_ISDIR(priv_st.st_mode) ||
      NULL == ::realpath(priv_buf, real_priv)) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }

  char path_buf[PATH_MAX + 1] = {0};
  MEMCPY(path_buf, trimmed.ptr(), trimmed.length());
  const char *last_slash = strrchr(path_buf, '/');
  char dir_buf[PATH_MAX + 1] = {0};
  const char *base_name = NULL;
  if (NULL == last_slash) {
    STRCPY(dir_buf, ".");
    base_name = path_buf;
  } else if (last_slash == path_buf) {
    STRCPY(dir_buf, "/");
    base_name = path_buf + 1;
  } else {
    MEMCPY(dir_buf, path_buf, static_cast<size_t>(last_slash - path_buf));
    base_name = last_slash + 1;
  }
  if (NULL == base_name || '\0' == base_name[0] ||
      0 == strcmp(base_name, ".") || 0 == strcmp(base_name, "..")) {
    return OB_INVALID_ARGUMENT;
  }

  char real_dir[PATH_MAX + 1] = {0};
  if (NULL == ::realpath(dir_buf, real_dir)) {
    if (path_buf[0] == '/' && !is_canonical_subpath(real_priv, dir_buf)) {
      return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
    }
    return OB_FILE_NOT_EXIST;
  }
  if (!is_canonical_subpath(real_priv, real_dir)) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }

  char final_target[PATH_MAX * 2 + 2] = {0};
  if (0 == strcmp(real_dir, "/")) {
    snprintf(final_target, sizeof(final_target), "/%s", base_name);
  } else {
    snprintf(final_target, sizeof(final_target), "%s/%s", real_dir, base_name);
  }

  struct stat existing_st;
  if (0 == ::lstat(final_target, &existing_st)) {
    LOG_USER_ERROR(OB_FILE_ALREADY_EXIST, final_target);
    return OB_FILE_ALREADY_EXIST;
  }

  ret = ob_write_string(allocator, ObString::make_string(final_target), resolved_target_path, true);
  return ret;
}

int ObFileScanUtils::verify_file_fingerprint(const ObString &canonical_path,
                                             const ObFileFingerprint &expected)
{
  int ret = OB_SUCCESS;
  if (!expected.is_valid_) {
    return OB_SUCCESS;
  }
  char path_buf[PATH_MAX + 1] = {0};
  if (canonical_path.empty() || canonical_path.length() >= PATH_MAX) {
    return OB_INVALID_ARGUMENT;
  }
  MEMCPY(path_buf, canonical_path.ptr(), canonical_path.length());
  struct stat st;
  if (0 != ::stat(path_buf, &st) || !S_ISREG(st.st_mode)) {
    return OB_FILE_NOT_EXIST;
  }
  ObFileFingerprint cur;
  fill_fingerprint_from_stat(st, cur);
  if (cur.size_ != expected.size_ ||
      cur.mtime_sec_ != expected.mtime_sec_ ||
      cur.mtime_nsec_ != expected.mtime_nsec_ ||
      cur.ino_ != expected.ino_ ||
      cur.dev_ != expected.dev_) {
    LOG_WARN("underlying file changed during execution", K(canonical_path), K(expected), K(cur));
    return OB_INVALID_DATA;
  }
  return OB_SUCCESS;
}

bool ObFileScanUtils::is_valid_utf8_no_nul(const char *buf, int64_t len)
{
  if (NULL == buf && len > 0) {
    return false;
  }
  int64_t i = 0;
  while (i < len) {
    const uint8_t c = static_cast<uint8_t>(buf[i]);
    if (c == 0) {
      return false;
    } else if (c < 0x80) {
      ++i;
    } else if ((c & 0xE0) == 0xC0) {
      if (i + 1 >= len || c < 0xC2) {
        return false;
      }
      const uint8_t c1 = static_cast<uint8_t>(buf[i + 1]);
      if ((c1 & 0xC0) != 0x80) {
        return false;
      }
      i += 2;
    } else if ((c & 0xF0) == 0xE0) {
      if (i + 2 >= len) {
        return false;
      }
      const uint8_t c1 = static_cast<uint8_t>(buf[i + 1]);
      const uint8_t c2 = static_cast<uint8_t>(buf[i + 2]);
      if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) {
        return false;
      }
      if (c == 0xE0 && c1 < 0xA0) {
        return false;
      }
      if (c == 0xED && c1 >= 0xA0) {
        return false; // UTF-16 surrogates invalid in UTF-8
      }
      i += 3;
    } else if ((c & 0xF8) == 0xF0) {
      if (i + 3 >= len || c > 0xF4) {
        return false;
      }
      const uint8_t c1 = static_cast<uint8_t>(buf[i + 1]);
      const uint8_t c2 = static_cast<uint8_t>(buf[i + 2]);
      const uint8_t c3 = static_cast<uint8_t>(buf[i + 3]);
      if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
        return false;
      }
      if (c == 0xF0 && c1 < 0x90) {
        return false;
      }
      if (c == 0xF4 && c1 >= 0x90) {
        return false;
      }
      i += 4;
    } else {
      return false;
    }
  }
  return true;
}

bool ObFileScanUtils::parse_strict_bool(const ObString &str, int8_t &out_val)
{
  if (4 == str.length() && 0 == strncasecmp(str.ptr(), "true", 4)) {
    out_val = 1;
    return true;
  } else if (5 == str.length() && 0 == strncasecmp(str.ptr(), "false", 5)) {
    out_val = 0;
    return true;
  }
  return false;
}

bool ObFileScanUtils::parse_strict_int64(const ObString &str, int64_t &out_val)
{
  if (str.empty() || NULL == str.ptr()) {
    return false;
  }
  const char *p = str.ptr();
  const int64_t len = str.length();
  int64_t idx = 0;
  bool neg = false;
  if (p[0] == '+' || p[0] == '-') {
    neg = (p[0] == '-');
    idx = 1;
  }
  if (idx >= len) {
    return false;
  }
  uint64_t acc = 0;
  const uint64_t limit = neg ? (static_cast<uint64_t>(INT64_MAX) + 1ULL)
                             : static_cast<uint64_t>(INT64_MAX);
  for (; idx < len; ++idx) {
    const char c = p[idx];
    if (c < '0' || c > '9') {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (acc > (limit - digit) / 10ULL) {
      return false;
    }
    acc = acc * 10ULL + digit;
  }
  if (neg) {
    if (acc == static_cast<uint64_t>(INT64_MAX) + 1ULL) {
      out_val = INT64_MIN;
    } else {
      out_val = -static_cast<int64_t>(acc);
    }
  } else {
    out_val = static_cast<int64_t>(acc);
  }
  return true;
}

bool ObFileScanUtils::parse_strict_double(const ObString &str, double &out_val)
{
  if (str.empty() || NULL == str.ptr() || str.length() >= 128) {
    return false;
  }
  const char *p = str.ptr();
  const int64_t len = str.length();
  // Reject leading/trailing whitespace or hex floats or inf/nan
  for (int64_t i = 0; i < len; ++i) {
    const char c = p[i];
    if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E')) {
      return false;
    }
  }
  char buf[128];
  MEMCPY(buf, p, static_cast<size_t>(len));
  buf[len] = '\0';
  char *endptr = NULL;
  errno = 0;
  const double v = strtod(buf, &endptr);
  if (endptr != buf + len || errno == ERANGE || !std::isfinite(v)) {
    return false;
  }
  out_val = v;
  return true;
}

static bool is_valid_ymd(int32_t y, int32_t m, int32_t d)
{
  if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1) {
    return false;
  }
  static const int32_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int32_t max_d = mdays[m - 1];
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0))) {
    max_d = 29;
  }
  return d <= max_d;
}

static bool parse_digits(const char *p, int64_t count, int32_t &out)
{
  int32_t v = 0;
  for (int64_t i = 0; i < count; ++i) {
    if (p[i] < '0' || p[i] > '9') {
      return false;
    }
    v = v * 10 + (p[i] - '0');
  }
  out = v;
  return true;
}

bool ObFileScanUtils::parse_strict_date(const ObString &str, int32_t &out_days)
{
  if (str.length() != 10 || NULL == str.ptr()) {
    return false;
  }
  const char *p = str.ptr();
  if (p[4] != '-' || p[7] != '-') {
    return false;
  }
  int32_t y = 0, m = 0, d = 0;
  if (!parse_digits(p, 4, y) || !parse_digits(p + 5, 2, m) || !parse_digits(p + 8, 2, d)) {
    return false;
  }
  if (!is_valid_ymd(y, m, d)) {
    return false;
  }
  int32_t val = 0;
  if (OB_SUCCESS != ObTimeConverter::str_to_date(str, val)) {
    return false;
  }
  out_days = val;
  return true;
}

bool ObFileScanUtils::parse_strict_datetime(const ObString &str, int64_t &out_usec)
{
  if (str.length() < 19 || str.length() > 26 || NULL == str.ptr()) {
    return false;
  }
  const char *p = str.ptr();
  const int64_t len = str.length();
  if (p[4] != '-' || p[7] != '-' || (p[10] != ' ' && p[10] != 'T') ||
      p[13] != ':' || p[16] != ':') {
    return false;
  }
  int32_t y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
  if (!parse_digits(p, 4, y) || !parse_digits(p + 5, 2, m) || !parse_digits(p + 8, 2, d) ||
      !parse_digits(p + 11, 2, hh) || !parse_digits(p + 14, 2, mm) || !parse_digits(p + 17, 2, ss)) {
    return false;
  }
  if (!is_valid_ymd(y, m, d) || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) {
    return false;
  }
  if (len > 19) {
    if (p[19] != '.' || len == 20) {
      return false;
    }
    for (int64_t i = 20; i < len; ++i) {
      if (p[i] < '0' || p[i] > '9') {
        return false;
      }
    }
  }
  ObTimeConvertCtx cvrt_ctx(NULL, false);
  int64_t val = 0;
  if (OB_SUCCESS != ObTimeConverter::str_to_datetime(str, cvrt_ctx, val)) {
    return false;
  }
  out_usec = val;
  return true;
}

ObFileLogicalType ObFileScanUtils::infer_scalar_string_type(const ObString &str,
                                                            bool is_null,
                                                            bool is_quoted_in_json)
{
  if (is_null) {
    return OB_FILE_TYPE_NULL;
  }
  if (!is_quoted_in_json) {
    int8_t bval = 0;
    if (parse_strict_bool(str, bval)) {
      return OB_FILE_TYPE_BOOLEAN;
    }
    int64_t ival = 0;
    if (parse_strict_int64(str, ival)) {
      return OB_FILE_TYPE_BIGINT;
    }
    double dval = 0.0;
    if (parse_strict_double(str, dval)) {
      return OB_FILE_TYPE_DOUBLE;
    }
  }
  int32_t date_val = 0;
  if (parse_strict_date(str, date_val)) {
    return OB_FILE_TYPE_DATE;
  }
  int64_t dt_val = 0;
  if (parse_strict_datetime(str, dt_val)) {
    return OB_FILE_TYPE_DATETIME;
  }
  return OB_FILE_TYPE_VARCHAR;
}

ObFileLogicalType ObFileScanUtils::merge_logical_types(ObFileLogicalType t1, ObFileLogicalType t2)
{
  if (t1 == OB_FILE_TYPE_NULL) {
    return t2;
  }
  if (t2 == OB_FILE_TYPE_NULL) {
    return t1;
  }
  if (t1 == t2) {
    return t1;
  }
  if ((t1 == OB_FILE_TYPE_BIGINT && t2 == OB_FILE_TYPE_DOUBLE) ||
      (t1 == OB_FILE_TYPE_DOUBLE && t2 == OB_FILE_TYPE_BIGINT)) {
    return OB_FILE_TYPE_DOUBLE;
  }
  if ((t1 == OB_FILE_TYPE_DATE && t2 == OB_FILE_TYPE_DATETIME) ||
      (t1 == OB_FILE_TYPE_DATETIME && t2 == OB_FILE_TYPE_DATE)) {
    return OB_FILE_TYPE_DATETIME;
  }
  return OB_FILE_TYPE_VARCHAR;
}

void ObFileScanUtils::logical_type_to_ob_meta(ObFileLogicalType logical_type,
                                              ObObjMeta &meta,
                                              ObAccuracy &accuracy)
{
  meta.reset();
  accuracy.reset();
  switch (logical_type) {
    case OB_FILE_TYPE_BOOLEAN:
      meta.set_tinyint();
      accuracy.set_length(1);
      break;
    case OB_FILE_TYPE_BIGINT:
      meta.set_int();
      accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY[ObIntType];
      break;
    case OB_FILE_TYPE_DOUBLE:
      meta.set_double();
      accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY[ObDoubleType];
      break;
    case OB_FILE_TYPE_DATE:
      meta.set_date();
      accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY[ObDateType];
      break;
    case OB_FILE_TYPE_DATETIME:
      meta.set_datetime();
      accuracy = ObAccuracy(19, 0);
      break;
    case OB_FILE_TYPE_VARCHAR:
    case OB_FILE_TYPE_NULL:
    default:
      meta.set_varchar();
      meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
      meta.set_collation_level(CS_LEVEL_IMPLICIT);
      accuracy.set_length(OB_MAX_VARCHAR_LENGTH);
      break;
  }
}

const char *ObFileScanUtils::logical_type_to_describe_name(ObFileLogicalType logical_type)
{
  switch (logical_type) {
    case OB_FILE_TYPE_BOOLEAN:  return "BOOLEAN";
    case OB_FILE_TYPE_BIGINT:   return "BIGINT";
    case OB_FILE_TYPE_DOUBLE:   return "DOUBLE";
    case OB_FILE_TYPE_DATE:     return "DATE";
    case OB_FILE_TYPE_DATETIME: return "DATETIME";
    case OB_FILE_TYPE_VARCHAR:
    case OB_FILE_TYPE_NULL:
    default:                    return "VARCHAR";
  }
}

const char *ObFileScanUtils::obj_meta_to_describe_name(const ObObjMeta &meta,
                                                       const ObAccuracy &accuracy)
{
  switch (meta.get_type()) {
    case ObTinyIntType:
      return (accuracy.get_length() == 1) ? "BOOLEAN" : "BIGINT";
    case ObSmallIntType:
    case ObMediumIntType:
    case ObInt32Type:
    case ObIntType:
    case ObUTinyIntType:
    case ObUSmallIntType:
    case ObUMediumIntType:
    case ObUInt32Type:
    case ObUInt64Type:
      return "BIGINT";
    case ObFloatType:
    case ObUFloatType:
    case ObDoubleType:
    case ObUDoubleType:
    case ObNumberType:
    case ObUNumberType:
    case ObDecimalIntType:
      return "DOUBLE";
    case ObDateType:
    case ObMySQLDateType:
      return "DATE";
    case ObDateTimeType:
    case ObMySQLDateTimeType:
    case ObTimestampType:
      return "DATETIME";
    default:
      return "VARCHAR";
  }
}

int ObFileScanUtils::deduplicate_column_names(ObIAllocator &allocator,
                                              ObIArray<ObFileColumnDef> &columns)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 16> assigned;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    ObString base_name = columns.at(i).name_;
    char fallback_buf[64];
    if (base_name.empty()) {
      const int n = snprintf(fallback_buf, sizeof(fallback_buf), "column%ld", static_cast<long>(i));
      if (OB_FAIL(ob_write_string(allocator, ObString(n, fallback_buf), base_name, true))) {
        break;
      }
    }
    // Count how many times base_name or candidates already exist (case-insensitive)
    bool conflict = false;
    for (int64_t j = 0; j < assigned.count(); ++j) {
      if (ObCharset::case_insensitive_equal(assigned.at(j), base_name)) {
        conflict = true;
        break;
      }
    }
    ObString final_name = base_name;
    if (conflict) {
      int64_t suffix = 2;
      while (OB_SUCC(ret)) {
        ObSqlString cand;
        if (OB_FAIL(cand.append_fmt("%.*s__%ld",
                                    base_name.length(),
                                    base_name.ptr(),
                                    static_cast<long>(suffix)))) {
          break;
        }
        bool cand_conflict = false;
        for (int64_t j = 0; j < assigned.count(); ++j) {
          if (ObCharset::case_insensitive_equal(assigned.at(j), cand.string())) {
            cand_conflict = true;
            break;
          }
        }
        if (!cand_conflict) {
          ret = ob_write_string(allocator, cand.string(), final_name, true);
          break;
        }
        ++suffix;
      }
    } else {
      ret = ob_write_string(allocator, base_name, final_name, true);
    }
    if (OB_SUCC(ret)) {
      columns.at(i).name_ = final_name;
      ret = assigned.push_back(final_name);
    }
  }
  return ret;
}

namespace
{

static bool extract_bool_from_parse_node(const ParseNode *node, bool &out_val)
{
  if (NULL == node) {
    return false;
  }
  if (node->type_ == T_BOOL) {
    out_val = (node->value_ != 0);
    return true;
  } else if (node->type_ == T_INT) {
    if (node->value_ == 0 || node->value_ == 1) {
      out_val = (node->value_ == 1);
      return true;
    }
    return false;
  } else if (node->type_ == T_VARCHAR || node->type_ == T_CHAR || node->type_ == T_IDENT ||
             node->type_ == T_COLUMN_REF || node->type_ == T_QUESTIONMARK) {
    const ParseNode *str_node = node;
    if (node->type_ == T_COLUMN_REF && node->num_child_ >= 3 && NULL != node->children_[2]) {
      str_node = node->children_[2];
    }
    if (NULL != str_node->str_value_ && str_node->str_len_ > 0) {
      if (parse_bool_token(
              ObString(static_cast<int32_t>(str_node->str_len_), str_node->str_value_), out_val)) {
        return true;
      }
    }
    if (NULL != str_node->raw_text_ && str_node->text_len_ > 0) {
      if (parse_bool_token(
              ObString(static_cast<int32_t>(str_node->text_len_), str_node->raw_text_), out_val)) {
        return true;
      }
    }
  }
  return false;
}

static bool extract_int_from_parse_node(const ParseNode *node, int64_t &out_val)
{
  if (NULL == node) {
    return false;
  }
  if (node->type_ == T_INT) {
    out_val = node->is_neg_ ? -node->value_ : node->value_;
    return true;
  } else if (node->type_ == T_OP_NEG && node->num_child_ == 1 &&
             NULL != node->children_[0] && node->children_[0]->type_ == T_INT) {
    out_val = -node->children_[0]->value_;
    return true;
  } else if (node->type_ == T_OP_POS && node->num_child_ == 1 &&
             NULL != node->children_[0] && node->children_[0]->type_ == T_INT) {
    out_val = node->children_[0]->value_;
    return true;
  } else if (node->type_ == T_QUESTIONMARK) {
    if (NULL != node->raw_text_ && node->text_len_ > 0) {
      ObString s(static_cast<int32_t>(node->text_len_), node->raw_text_);
      if (ObFileScanUtils::parse_strict_int64(s, out_val)) {
        return true;
      }
    }
    if (NULL != node->str_value_ && node->str_len_ > 0) {
      ObString s(static_cast<int32_t>(node->str_len_), node->str_value_);
      if (ObFileScanUtils::parse_strict_int64(s, out_val)) {
        return true;
      }
    }
  }
  return false;
}

static bool extract_string_from_parse_node(const ParseNode *node,
                                           bool allow_ident,
                                           ObString &out_str)
{
  out_str.reset();
  if (NULL == node) {
    return false;
  }
  if (node->type_ == T_VARCHAR || node->type_ == T_CHAR || node->type_ == T_QUESTIONMARK) {
    if (NULL != node->str_value_) {
      out_str.assign_ptr(node->str_value_, static_cast<int32_t>(node->str_len_));
      return true;
    } else if (NULL != node->raw_text_) {
      const char *p = node->raw_text_;
      int32_t len = static_cast<int32_t>(node->text_len_);
      if (len >= 2 && ((p[0] == '\'' && p[len - 1] == '\'') || (p[0] == '"' && p[len - 1] == '"'))) {
        out_str.assign_ptr(p + 1, len - 2);
      } else {
        out_str.assign_ptr(p, len);
      }
      return true;
    }
  } else if (allow_ident && (node->type_ == T_IDENT || node->type_ == T_COLUMN_REF)) {
    const ParseNode *id_node = node;
    if (node->type_ == T_COLUMN_REF && node->num_child_ >= 3 && NULL != node->children_[2]) {
      id_node = node->children_[2];
    }
    if (NULL != id_node->str_value_) {
      out_str.assign_ptr(id_node->str_value_, static_cast<int32_t>(id_node->str_len_));
      return true;
    } else if (NULL != id_node->raw_text_) {
      out_str.assign_ptr(id_node->raw_text_, static_cast<int32_t>(id_node->text_len_));
      return true;
    }
  }
  return false;
}

static void collect_option_nodes(const ParseNode *opt_tree, ObIArray<const ParseNode *> &out_nodes)
{
  if (NULL == opt_tree) {
    return;
  }
  if (opt_tree->type_ == T_LINK_NODE || opt_tree->type_ == T_EXPR_LIST ||
      opt_tree->type_ == T_TABLE_OPTION_LIST) {
    for (int64_t i = 0; i < opt_tree->num_child_; ++i) {
      collect_option_nodes(opt_tree->children_[i], out_nodes);
    }
  } else {
    (void)out_nodes.push_back(opt_tree);
  }
}

} // anonymous namespace

int ObFileScanUtils::parse_file_table_node(const ParseNode &func_node,
                                           ObIAllocator &allocator,
                                           ObFileScanOptions &options)
{
  int ret = OB_SUCCESS;
  options = ObFileScanOptions();

  if (NULL == func_node.str_value_ || func_node.str_len_ <= 0 || func_node.num_child_ < 1) {
    return OB_INVALID_ARGUMENT;
  }
  ObString func_name(static_cast<int32_t>(func_node.str_len_), func_node.str_value_);
  if (0 == func_name.case_compare("read_csv")) {
    options.format_ = OB_FILE_FORMAT_CSV;
  } else if (0 == func_name.case_compare("read_json") ||
             0 == func_name.case_compare("read_ndjson")) {
    options.format_ = OB_FILE_FORMAT_JSONL;
  } else if (0 == func_name.case_compare("read_parquet")) {
    options.format_ = OB_FILE_FORMAT_PARQUET;
  } else if (0 == func_name.case_compare("glob")) {
    options.format_ = OB_FILE_FORMAT_GLOB;
  } else {
    return OB_NOT_SUPPORTED;
  }

  const ParseNode *path_node = func_node.children_[0];
  ObString raw_path;
  if (!extract_string_from_parse_node(path_node, false, raw_path) || raw_path.empty()) {
    return OB_INVALID_ARGUMENT;
  }
  if (OB_FAIL(ob_write_string(allocator, raw_path, options.file_path_, true))) {
    return ret;
  }

  const ParseNode *opt_root = (func_node.num_child_ >= 2) ? func_node.children_[1] : NULL;
  ObSEArray<const ParseNode *, 8> opt_nodes;
  collect_option_nodes(opt_root, opt_nodes);

  if (!opt_nodes.empty() &&
      (options.format_ == OB_FILE_FORMAT_PARQUET || options.format_ == OB_FILE_FORMAT_GLOB)) {
    return OB_NOT_SUPPORTED;
  }

  ObSEArray<ObString, 8> seen_keys;
  for (int64_t i = 0; OB_SUCC(ret) && i < opt_nodes.count(); ++i) {
    const ParseNode *kv = opt_nodes.at(i);
    if (OB_ISNULL(kv) || kv->num_child_ < 2 ||
        OB_ISNULL(kv->children_[0]) || OB_ISNULL(kv->children_[1])) {
      ret = OB_INVALID_ARGUMENT;
      break;
    }
    const ParseNode *key_node = kv->children_[0];
    const ParseNode *val_node = kv->children_[1];
    ObString key;
    if (!extract_string_from_parse_node(key_node, true, key)) {
      ret = OB_INVALID_ARGUMENT;
      break;
    }
    key = key.trim();
    if (key.empty()) {
      ret = OB_INVALID_ARGUMENT;
      break;
    }
    ObString norm_key = key;
    if (0 == key.case_compare("delim")) {
      norm_key = ObString::make_string("delimiter");
    }
    for (int64_t j = 0; j < seen_keys.count(); ++j) {
      if (ObCharset::case_insensitive_equal(seen_keys.at(j), norm_key)) {
        ret = OB_INVALID_ARGUMENT;
        break;
      }
    }
    if (OB_FAIL(ret) || OB_FAIL(seen_keys.push_back(norm_key))) {
      break;
    }

    if (options.format_ == OB_FILE_FORMAT_CSV) {
      if (0 == norm_key.case_compare("header")) {
        if (!extract_bool_from_parse_node(val_node, options.header_)) {
          ret = OB_INVALID_ARGUMENT;
        }
      } else if (0 == norm_key.case_compare("delimiter")) {
        ObString s;
        if (!extract_string_from_parse_node(val_node, false, s) ||
            s.length() != 1 || static_cast<uint8_t>(s.ptr()[0]) >= 0x80 ||
            s.ptr()[0] == '\0' || s.ptr()[0] == '\n' || s.ptr()[0] == '\r') {
          ret = OB_INVALID_ARGUMENT;
        } else {
          options.delimiter_ = s.ptr()[0];
        }
      } else if (0 == norm_key.case_compare("quote")) {
        ObString s;
        if (!extract_string_from_parse_node(val_node, false, s) ||
            s.length() != 1 || static_cast<uint8_t>(s.ptr()[0]) >= 0x80 ||
            s.ptr()[0] == '\0' || s.ptr()[0] == '\n' || s.ptr()[0] == '\r') {
          ret = OB_INVALID_ARGUMENT;
        } else {
          options.quote_ = s.ptr()[0];
        }
      } else if (0 == norm_key.case_compare("escape")) {
        ObString s;
        if (!extract_string_from_parse_node(val_node, false, s) ||
            s.length() != 1 || static_cast<uint8_t>(s.ptr()[0]) >= 0x80 ||
            s.ptr()[0] == '\0' || s.ptr()[0] == '\n' || s.ptr()[0] == '\r') {
          ret = OB_INVALID_ARGUMENT;
        } else {
          options.escape_ = s.ptr()[0];
        }
      } else if (0 == norm_key.case_compare("nullstr")) {
        ObString s;
        if (!extract_string_from_parse_node(val_node, false, s)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          ret = ob_write_string(allocator, s, options.nullstr_, true);
        }
      } else if (0 == norm_key.case_compare("sample_size")) {
        int64_t sz = 0;
        if (!extract_int_from_parse_node(val_node, sz)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (sz != -1) {
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size other than -1");
          ret = OB_NOT_SUPPORTED;
        } else {
          options.sample_size_ = -1;
        }
      } else if (0 == norm_key.case_compare("auto_detect")) {
        bool ad = true;
        if (!extract_bool_from_parse_node(val_node, ad)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (!ad) {
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "auto_detect = false");
          ret = OB_NOT_SUPPORTED;
        } else {
          options.auto_detect_ = true;
        }
      } else {
        ret = OB_NOT_SUPPORTED;
      }
    } else if (options.format_ == OB_FILE_FORMAT_JSONL) {
      if (0 == norm_key.case_compare("format")) {
        ObString s;
        if (!extract_string_from_parse_node(val_node, true, s)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (0 == s.case_compare("newline_delimited") ||
                   0 == s.case_compare("ndjson")) {
          ret = ob_write_string(allocator, s, options.json_format_, true);
        } else {
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "json format other than newline_delimited");
          ret = OB_NOT_SUPPORTED;
        }
      } else if (0 == norm_key.case_compare("sample_size")) {
        int64_t sz = 0;
        if (!extract_int_from_parse_node(val_node, sz)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (sz != -1) {
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size other than -1");
          ret = OB_NOT_SUPPORTED;
        } else {
          options.sample_size_ = -1;
        }
      } else if (0 == norm_key.case_compare("auto_detect")) {
        bool ad = true;
        if (!extract_bool_from_parse_node(val_node, ad)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (!ad) {
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "auto_detect = false");
          ret = OB_NOT_SUPPORTED;
        } else {
          options.auto_detect_ = true;
        }
      } else {
        ret = OB_NOT_SUPPORTED;
      }
    }
  }

  if (OB_SUCC(ret) && options.format_ == OB_FILE_FORMAT_CSV) {
    if (options.delimiter_ == options.quote_) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

// ============================================================================
// CSV Streaming Reader & Scanner
// ============================================================================
namespace
{

struct CsvFieldVal
{
  std::string value_;
  bool is_quoted_;
  bool is_null_;
};

class ObCsvStreamingReader
{
public:
  static const size_t READ_BUF_SIZE = 65536;

  explicit ObCsvStreamingReader(const ObFileScanOptions &opts)
    : opts_(opts),
      fd_(-1),
      buf_pos_(0),
      buf_len_(0),
      eof_(false),
      first_chunk_(true),
      line_no_(1)
  {}

  ~ObCsvStreamingReader()
  {
    close();
  }

  int open()
  {
    close();
    char path_buf[PATH_MAX + 1] = {0};
    const ObString &p = opts_.canonical_path_.empty() ? opts_.file_path_ : opts_.canonical_path_;
    if (p.empty() || p.length() >= PATH_MAX) {
      return OB_INVALID_ARGUMENT;
    }
    MEMCPY(path_buf, p.ptr(), p.length());
    fd_ = ::open(path_buf, O_RDONLY);
    if (fd_ < 0) {
      return (errno == EACCES || errno == EPERM) ? OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS : OB_FILE_NOT_EXIST;
    }
    buf_pos_ = 0;
    buf_len_ = 0;
    eof_ = false;
    first_chunk_ = true;
    line_no_ = 1;
    return OB_SUCCESS;
  }

  int reset()
  {
    if (fd_ < 0) {
      return open();
    }
    if (::lseek(fd_, 0, SEEK_SET) < 0) {
      return OB_IO_ERROR;
    }
    buf_pos_ = 0;
    buf_len_ = 0;
    eof_ = false;
    first_chunk_ = true;
    line_no_ = 1;
    return OB_SUCCESS;
  }

  void close()
  {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  int64_t get_line_no() const { return line_no_; }

  // Reads next CSV record. Returns OB_SUCCESS, OB_ITER_END, or OB_INVALID_DATA.
  int read_next_record(std::vector<CsvFieldVal> &fields, int64_t &record_start_line)
  {
    fields.clear();
    record_start_line = line_no_;

    int ch = 0;
    int ret = peek_char(ch);
    if (ret == OB_ITER_END) {
      return OB_ITER_END;
    } else if (OB_FAIL(ret)) {
      return ret;
    }

    // Skip trailing empty lines at EOF if completely empty
    record_start_line = line_no_;
    const char delim = opts_.delimiter_;
    const char quote = opts_.quote_;
    const char escape = opts_.escape_;

    while (true) {
      CsvFieldVal field;
      field.is_quoted_ = false;
      field.is_null_ = false;

      ret = peek_char(ch);
      if (ret == OB_ITER_END) {
        // Reached EOF right after a delimiter -> empty unquoted last field
        finalize_field(field);
        fields.push_back(std::move(field));
        break;
      } else if (OB_FAIL(ret)) {
        return ret;
      }

      if (ch == quote) {
        // Quoted field
        field.is_quoted_ = true;
        consume_char();
        bool closed = false;
        while (true) {
          ret = next_char(ch);
          if (ret == OB_ITER_END) {
            LOG_WARN("unclosed quote in CSV file", K(opts_.file_path_), K(record_start_line));
            return OB_INVALID_DATA;
          } else if (OB_FAIL(ret)) {
            return ret;
          }
          if (ch == '\0') {
            LOG_WARN("NUL byte in CSV file", K(opts_.file_path_), K(line_no_));
            return OB_INVALID_DATA;
          }
          if (escape == quote) {
            if (ch == quote) {
              int next_c = 0;
              int peek_ret = peek_char(next_c);
              if (peek_ret == OB_SUCCESS && next_c == quote) {
                consume_char();
                field.value_.push_back(quote);
              } else if (peek_ret == OB_SUCCESS || peek_ret == OB_ITER_END) {
                closed = true;
                break;
              } else {
                return peek_ret;
              }
            } else {
              if (ch == '\n') {
                ++line_no_;
              }
              field.value_.push_back(static_cast<char>(ch));
            }
          } else {
            if (ch == escape) {
              int esc_c = 0;
              ret = next_char(esc_c);
              if (OB_FAIL(ret)) {
                return OB_INVALID_DATA;
              }
              if (esc_c == '\n') {
                ++line_no_;
              }
              field.value_.push_back(static_cast<char>(esc_c));
            } else if (ch == quote) {
              int next_c = 0;
              int peek_ret = peek_char(next_c);
              if (peek_ret == OB_SUCCESS && next_c == quote) {
                consume_char();
                field.value_.push_back(quote);
              } else if (peek_ret == OB_SUCCESS || peek_ret == OB_ITER_END) {
                closed = true;
                break;
              } else {
                return peek_ret;
              }
            } else {
              if (ch == '\n') {
                ++line_no_;
              }
              field.value_.push_back(static_cast<char>(ch));
            }
          }
        }
        if (!closed || !ObFileScanUtils::is_valid_utf8_no_nul(
                           field.value_.data(), static_cast<int64_t>(field.value_.size()))) {
          LOG_WARN("invalid UTF-8 or unclosed quote in CSV", K(opts_.file_path_), K(record_start_line));
          return OB_INVALID_DATA;
        }
        fields.push_back(std::move(field));

        // After closing quote, next char must be delimiter, CR/LF, or EOF
        ret = peek_char(ch);
        if (ret == OB_ITER_END) {
          break;
        } else if (OB_FAIL(ret)) {
          return ret;
        }
        if (ch == delim) {
          consume_char();
          continue;
        } else if (ch == '\r') {
          consume_char();
          int lf = 0;
          if (OB_SUCCESS == peek_char(lf) && lf == '\n') {
            consume_char();
          }
          ++line_no_;
          break;
        } else if (ch == '\n') {
          consume_char();
          ++line_no_;
          break;
        } else {
          LOG_WARN("unexpected trailing character after closing quote in CSV",
                   K(opts_.file_path_), K(line_no_), K(ch));
          return OB_INVALID_DATA;
        }
      } else {
        // Unquoted field
        bool hit_delim = false;
        while (true) {
          ret = peek_char(ch);
          if (ret == OB_ITER_END) {
            break;
          } else if (OB_FAIL(ret)) {
            return ret;
          }
          if (ch == '\0' || ch == quote) {
            LOG_WARN("invalid quote or NUL byte in unquoted CSV field",
                     K(opts_.file_path_), K(line_no_));
            return OB_INVALID_DATA;
          }
          if (ch == delim) {
            consume_char();
            hit_delim = true;
            break;
          } else if (ch == '\r') {
            consume_char();
            int lf = 0;
            if (OB_SUCCESS == peek_char(lf) && lf == '\n') {
              consume_char();
            }
            ++line_no_;
            break;
          } else if (ch == '\n') {
            consume_char();
            ++line_no_;
            break;
          } else {
            consume_char();
            field.value_.push_back(static_cast<char>(ch));
          }
        }
        if (!ObFileScanUtils::is_valid_utf8_no_nul(
                field.value_.data(), static_cast<int64_t>(field.value_.size()))) {
          LOG_WARN("invalid UTF-8 in CSV field", K(opts_.file_path_), K(record_start_line));
          return OB_INVALID_DATA;
        }
        finalize_field(field);
        fields.push_back(std::move(field));
        if (!hit_delim) {
          break;
        }
      }
    }
    return OB_SUCCESS;
  }

private:
  void finalize_field(CsvFieldVal &field) const
  {
    if (!field.is_quoted_) {
      if (opts_.nullstr_.empty()) {
        if (field.value_.empty()) {
          field.is_null_ = true;
        }
      } else {
        if (field.value_.empty() ||
            (static_cast<int64_t>(field.value_.size()) == opts_.nullstr_.length() &&
             0 == MEMCMP(field.value_.data(), opts_.nullstr_.ptr(), opts_.nullstr_.length()))) {
          field.is_null_ = true;
        }
      }
    }
  }

  int fill_buf()
  {
    if (eof_) {
      return OB_ITER_END;
    }
    const ssize_t n = ::read(fd_, buf_, READ_BUF_SIZE);
    if (n < 0) {
      return OB_IO_ERROR;
    } else if (n == 0) {
      eof_ = true;
      return OB_ITER_END;
    }
    buf_pos_ = 0;
    buf_len_ = static_cast<size_t>(n);
    if (first_chunk_) {
      first_chunk_ = false;
      if (buf_len_ >= 3 &&
          static_cast<uint8_t>(buf_[0]) == 0xEF &&
          static_cast<uint8_t>(buf_[1]) == 0xBB &&
          static_cast<uint8_t>(buf_[2]) == 0xBF) {
        buf_pos_ = 3;
      }
    }
    if (buf_pos_ >= buf_len_) {
      return fill_buf();
    }
    return OB_SUCCESS;
  }

  int peek_char(int &ch)
  {
    if (buf_pos_ >= buf_len_) {
      int ret = fill_buf();
      if (OB_FAIL(ret)) {
        return ret;
      }
    }
    ch = static_cast<uint8_t>(buf_[buf_pos_]);
    return OB_SUCCESS;
  }

  void consume_char()
  {
    if (buf_pos_ < buf_len_) {
      ++buf_pos_;
    }
  }

  int next_char(int &ch)
  {
    int ret = peek_char(ch);
    if (OB_SUCC(ret)) {
      consume_char();
    }
    return ret;
  }

  const ObFileScanOptions &opts_;
  int fd_;
  char buf_[READ_BUF_SIZE];
  size_t buf_pos_;
  size_t buf_len_;
  bool eof_;
  bool first_chunk_;
  int64_t line_no_;
};

static int write_scalar_to_datum(const ObFileColumnDef &col_def,
                                 const ObString &raw_str,
                                 bool is_null,
                                 ObIAllocator &row_alloc,
                                 ObExpr *expr,
                                 ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr)) {
    return OB_ERR_UNEXPECTED;
  }
  ObDatum &datum = expr->locate_datum_for_write(eval_ctx);
  if (is_null) {
    datum.set_null();
    expr->set_evaluated_projected(eval_ctx);
    return OB_SUCCESS;
  }
  switch (col_def.logical_type_) {
    case OB_FILE_TYPE_BOOLEAN: {
      int8_t bval = 0;
      if (!ObFileScanUtils::parse_strict_bool(raw_str, bval)) {
        return OB_INVALID_DATA;
      }
      datum.set_int(bval);
      break;
    }
    case OB_FILE_TYPE_BIGINT: {
      int64_t ival = 0;
      if (!ObFileScanUtils::parse_strict_int64(raw_str, ival)) {
        return OB_INVALID_DATA;
      }
      datum.set_int(ival);
      break;
    }
    case OB_FILE_TYPE_DOUBLE: {
      double dval = 0.0;
      if (!ObFileScanUtils::parse_strict_double(raw_str, dval)) {
        return OB_INVALID_DATA;
      }
      datum.set_double(dval);
      break;
    }
    case OB_FILE_TYPE_DATE: {
      int32_t days = 0;
      if (!ObFileScanUtils::parse_strict_date(raw_str, days)) {
        return OB_INVALID_DATA;
      }
      datum.set_date(days);
      break;
    }
    case OB_FILE_TYPE_DATETIME: {
      int64_t usec = 0;
      if (ObFileScanUtils::parse_strict_datetime(raw_str, usec)) {
        datum.set_datetime(usec);
      } else {
        int32_t days = 0;
        if (ObFileScanUtils::parse_strict_date(raw_str, days)) {
          datum.set_datetime(static_cast<int64_t>(days) * USECS_PER_DAY);
        } else {
          return OB_INVALID_DATA;
        }
      }
      break;
    }
    case OB_FILE_TYPE_VARCHAR:
    case OB_FILE_TYPE_NULL:
    default: {
      ObString copied;
      if (raw_str.empty()) {
        datum.set_string(ObString(0, ""));
      } else if (OB_FAIL(ob_write_string(row_alloc, raw_str, copied, false))) {
        return ret;
      } else {
        datum.set_string(copied);
      }
      break;
    }
  }
  expr->set_evaluated_projected(eval_ctx);
  return ret;
}

class ObCsvFileScanner final : public ObIFileScanner
{
public:
  ObCsvFileScanner(const ObFileTableDef &table_def, ObExecContext &exec_ctx)
    : table_def_(table_def),
      exec_ctx_(exec_ctx),
      reader_(table_def_.options_),
      row_alloc_("CsvScanRow"),
      opened_(false)
  {}

  ~ObCsvFileScanner() override
  {
    close();
  }

  int open() override
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(ObFileScanUtils::check_file_sql_enabled(exec_ctx_.get_my_session()))) {
    } else if (OB_FAIL(ObFileScanUtils::verify_file_fingerprint(
                   table_def_.options_.canonical_path_, table_def_.options_.fingerprint_))) {
    } else if (OB_FAIL(reader_.open())) {
    } else {
      opened_ = true;
      if (table_def_.options_.header_) {
        std::vector<CsvFieldVal> header_fields;
        int64_t line_no = 0;
        ret = reader_.read_next_record(header_fields, line_no);
        if (ret == OB_ITER_END) {
          ret = OB_INVALID_DATA;
        }
      }
    }
    return ret;
  }

  int get_next_row(const ObIArray<ObExpr *> &column_exprs, ObEvalCtx &eval_ctx) override
  {
    int ret = OB_SUCCESS;
    if (!opened_) {
      return OB_NOT_INIT;
    }
    row_alloc_.reuse();
    std::vector<CsvFieldVal> fields;
    int64_t rec_line = 0;
    while (true) {
      ret = reader_.read_next_record(fields, rec_line);
      if (ret == OB_ITER_END) {
        if (OB_FAIL(ObFileScanUtils::verify_file_fingerprint(
                table_def_.options_.canonical_path_, table_def_.options_.fingerprint_))) {
          return ret;
        }
        return OB_ITER_END;
      } else if (OB_FAIL(ret)) {
        return ret;
      }
      // Skip trailing blank line with single empty unquoted field if file has > 1 column
      if (fields.size() == 1 && !fields[0].is_quoted_ && fields[0].value_.empty() &&
          table_def_.columns_.count() > 1) {
        continue;
      }
      break;
    }
    if (static_cast<int64_t>(fields.size()) != table_def_.columns_.count()) {
      LOG_WARN("CSV column count mismatch",
               K(table_def_.options_.file_path_), K(rec_line),
               "expected", table_def_.columns_.count(), "actual", fields.size());
      return OB_INVALID_DATA;
    }
    const int64_t proj_cnt = column_exprs.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < proj_cnt; ++i) {
      const int32_t col_idx = (i < table_def_.access_col_indices_.count())
                                ? table_def_.access_col_indices_.at(i)
                                : static_cast<int32_t>(i);
      if (col_idx < 0 || col_idx >= table_def_.columns_.count()) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      const CsvFieldVal &fv = fields[col_idx];
      ObString val_str(static_cast<int32_t>(fv.value_.size()), fv.value_.data());
      ret = write_scalar_to_datum(table_def_.columns_.at(col_idx),
                                  val_str,
                                  fv.is_null_,
                                  row_alloc_,
                                  column_exprs.at(i),
                                  eval_ctx);
    }
    return ret;
  }

  int rescan() override
  {
    int ret = OB_SUCCESS;
    row_alloc_.reuse();
    if (OB_FAIL(ObFileScanUtils::verify_file_fingerprint(
            table_def_.options_.canonical_path_, table_def_.options_.fingerprint_))) {
    } else if (OB_FAIL(reader_.reset())) {
    } else if (table_def_.options_.header_) {
      std::vector<CsvFieldVal> header_fields;
      int64_t line_no = 0;
      ret = reader_.read_next_record(header_fields, line_no);
      if (ret == OB_ITER_END) {
        ret = OB_INVALID_DATA;
      }
    }
    return ret;
  }

  int close() override
  {
    reader_.close();
    row_alloc_.reset();
    opened_ = false;
    return OB_SUCCESS;
  }

private:
  const ObFileTableDef &table_def_;
  ObExecContext &exec_ctx_;
  ObCsvStreamingReader reader_;
  ObArenaAllocator row_alloc_;
  bool opened_;
};

} // anonymous namespace

int ObFileScanUtils::infer_csv_schema(ObIAllocator &allocator, ObFileTableDef &table_def)
{
  int ret = OB_SUCCESS;
  table_def.columns_.reuse();

  ObCsvStreamingReader reader(table_def.options_);
  if (OB_FAIL(reader.open())) {
    return ret;
  }

  std::vector<CsvFieldVal> fields;
  int64_t rec_line = 0;
  ret = reader.read_next_record(fields, rec_line);
  if (ret == OB_ITER_END || fields.empty()) {
    LOG_WARN("empty CSV file cannot infer schema", K(table_def.options_.file_path_));
    return OB_INVALID_DATA;
  } else if (OB_FAIL(ret)) {
    return ret;
  }

  const int64_t num_cols = static_cast<int64_t>(fields.size());
  std::vector<ObFileLogicalType> col_types(num_cols, OB_FILE_TYPE_NULL);
  std::vector<bool> col_has_null(num_cols, false);
  std::vector<int16_t> col_dt_scale(num_cols, 0);
  int64_t data_rows = 0;

  if (table_def.options_.header_) {
    for (int64_t i = 0; OB_SUCC(ret) && i < num_cols; ++i) {
      ObFileColumnDef col;
      ObString raw_name(static_cast<int32_t>(fields[i].value_.size()), fields[i].value_.data());
      if (OB_FAIL(ob_write_string(allocator, raw_name, col.name_, true))) {
      } else if (OB_FAIL(table_def.columns_.push_back(col))) {
      }
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < num_cols; ++i) {
      ObFileColumnDef col;
      char name_buf[64];
      const int n = snprintf(name_buf, sizeof(name_buf), "column%ld", static_cast<long>(i));
      if (OB_FAIL(ob_write_string(allocator, ObString(n, name_buf), col.name_, true))) {
      } else if (OB_FAIL(table_def.columns_.push_back(col))) {
      }
    }
    // First row is also a data row when header = false
    ++data_rows;
    for (int64_t i = 0; i < num_cols; ++i) {
      if (fields[i].is_null_) {
        col_has_null[i] = true;
      } else {
        ObString val_str(static_cast<int32_t>(fields[i].value_.size()), fields[i].value_.data());
        ObFileLogicalType t = infer_scalar_string_type(val_str, false, false);
        col_types[i] = merge_logical_types(col_types[i], t);
        if (t == OB_FILE_TYPE_DATETIME && val_str.length() > 19 && val_str.ptr()[19] == '.') {
          int16_t sc = static_cast<int16_t>(val_str.length() - 20);
          if (sc > col_dt_scale[i]) {
            col_dt_scale[i] = sc;
          }
        }
      }
    }
  }

  if (OB_SUCC(ret)) {
    ret = deduplicate_column_names(allocator, table_def.columns_);
  }

  while (OB_SUCC(ret)) {
    int r = reader.read_next_record(fields, rec_line);
    if (r == OB_ITER_END) {
      break;
    } else if (OB_FAIL(r)) {
      ret = r;
      break;
    }
    if (fields.size() == 1 && !fields[0].is_quoted_ && fields[0].value_.empty() && num_cols > 1) {
      continue;
    }
    if (static_cast<int64_t>(fields.size()) != num_cols) {
      LOG_WARN("CSV column count mismatch during schema inference",
               K(table_def.options_.file_path_), K(rec_line),
               K(num_cols), "actual", fields.size());
      ret = OB_INVALID_DATA;
      break;
    }
    ++data_rows;
    for (int64_t i = 0; i < num_cols; ++i) {
      if (fields[i].is_null_) {
        col_has_null[i] = true;
      } else {
        ObString val_str(static_cast<int32_t>(fields[i].value_.size()), fields[i].value_.data());
        ObFileLogicalType t = infer_scalar_string_type(val_str, false, false);
        col_types[i] = merge_logical_types(col_types[i], t);
        if (t == OB_FILE_TYPE_DATETIME && val_str.length() > 19 && val_str.ptr()[19] == '.') {
          int16_t sc = static_cast<int16_t>(val_str.length() - 20);
          if (sc > col_dt_scale[i]) {
            col_dt_scale[i] = sc;
          }
        }
      }
    }
  }

  if (OB_SUCC(ret)) {
    for (int64_t i = 0; i < num_cols; ++i) {
      ObFileColumnDef &col = table_def.columns_.at(i);
      col.logical_type_ = (col_types[i] == OB_FILE_TYPE_NULL) ? OB_FILE_TYPE_VARCHAR : col_types[i];
      col.is_nullable_ = (data_rows == 0) ? true : col_has_null[i];
      logical_type_to_ob_meta(col.logical_type_, col.meta_, col.accuracy_);
      if (col.logical_type_ == OB_FILE_TYPE_DATETIME && col_dt_scale[i] > 0) {
        col.accuracy_.set_scale(std::min<int16_t>(col_dt_scale[i], 6));
      }
    }
  }
  return ret;
}

// ============================================================================
// JSONL / NDJSON Streaming Reader & Scanner
// ============================================================================
namespace
{

enum JsonScalarKind
{
  JSON_SCALAR_NULL   = 0,
  JSON_SCALAR_BOOL   = 1,
  JSON_SCALAR_INT    = 2,
  JSON_SCALAR_DOUBLE = 3,
  JSON_SCALAR_STRING = 4,
};

struct JsonFieldEntry
{
  std::string key_;
  JsonScalarKind kind_;
  std::string str_val_;
};

static void append_utf8_codepoint(uint32_t cp, std::string &out)
{
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

static bool parse_hex4(const char *p, uint32_t &out)
{
  out = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = p[i];
    uint32_t v = 0;
    if (c >= '0' && c <= '9') {
      v = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      v = static_cast<uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      v = static_cast<uint32_t>(c - 'A' + 10);
    } else {
      return false;
    }
    out = (out << 4) | v;
  }
  return true;
}

static void skip_json_ws(const char *s, size_t len, size_t &pos)
{
  while (pos < len) {
    const char c = s[pos];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++pos;
    } else {
      break;
    }
  }
}

static bool parse_json_string_token(const char *s, size_t len, size_t &pos, std::string &out)
{
  out.clear();
  if (pos >= len || s[pos] != '"') {
    return false;
  }
  ++pos;
  while (pos < len) {
    const unsigned char c = static_cast<unsigned char>(s[pos++]);
    if (c == '"') {
      return ObFileScanUtils::is_valid_utf8_no_nul(out.data(), static_cast<int64_t>(out.size()));
    }
    if (c < 0x20) {
      return false; // unescaped control character in JSON string
    }
    if (c == '\\') {
      if (pos >= len) {
        return false;
      }
      const char esc = s[pos++];
      switch (esc) {
        case '"':  out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/':  out.push_back('/'); break;
        case 'b':  out.push_back('\b'); break;
        case 'f':  out.push_back('\f'); break;
        case 'n':  out.push_back('\n'); break;
        case 'r':  out.push_back('\r'); break;
        case 't':  out.push_back('\t'); break;
        case 'u': {
          if (pos + 4 > len) {
            return false;
          }
          uint32_t cp = 0;
          if (!parse_hex4(s + pos, cp)) {
            return false;
          }
          pos += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate, must be followed by \uDC00..\uDFFF
            if (pos + 6 > len || s[pos] != '\\' || s[pos + 1] != 'u') {
              return false;
            }
            uint32_t low = 0;
            if (!parse_hex4(s + pos + 2, low) || low < 0xDC00 || low > 0xDFFF) {
              return false;
            }
            pos += 6;
            cp = 0x10000U + (((cp - 0xD800U) << 10) | (low - 0xDC00U));
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
          }
          if (cp == 0) {
            return false;
          }
          append_utf8_codepoint(cp, out);
          break;
        }
        default:
          return false;
      }
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return false;
}

// Parses a single flat JSON object line. Rejects arrays, nested objects, primitives, trailing tokens.
static int parse_flat_json_object_line(const char *line,
                                       size_t len,
                                       std::vector<JsonFieldEntry> &entries)
{
  entries.clear();
  if (!ObFileScanUtils::is_valid_utf8_no_nul(line, static_cast<int64_t>(len))) {
    return OB_INVALID_DATA;
  }
  size_t pos = 0;
  skip_json_ws(line, len, pos);
  if (pos >= len || line[pos] != '{') {
    return OB_INVALID_DATA;
  }
  ++pos;
  skip_json_ws(line, len, pos);
  if (pos >= len) {
    return OB_INVALID_DATA;
  }
  if (line[pos] == '}') {
    ++pos;
    skip_json_ws(line, len, pos);
    return (pos == len) ? OB_SUCCESS : OB_INVALID_DATA;
  }

  while (true) {
    skip_json_ws(line, len, pos);
    std::string key;
    if (!parse_json_string_token(line, len, pos, key) || key.empty()) {
      return OB_INVALID_DATA;
    }
    // Check duplicate key in same JSON object
    for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].key_ == key) {
        return OB_INVALID_DATA;
      }
    }
    skip_json_ws(line, len, pos);
    if (pos >= len || line[pos] != ':') {
      return OB_INVALID_DATA;
    }
    ++pos;
    skip_json_ws(line, len, pos);
    if (pos >= len) {
      return OB_INVALID_DATA;
    }

    JsonFieldEntry entry;
    entry.key_ = std::move(key);
    const char vc = line[pos];
    if (vc == '{' || vc == '[') {
      // Nested object or array is forbidden in flat JSONL table scan
      return OB_INVALID_DATA;
    } else if (vc == '"') {
      entry.kind_ = JSON_SCALAR_STRING;
      if (!parse_json_string_token(line, len, pos, entry.str_val_)) {
        return OB_INVALID_DATA;
      }
    } else if (vc == 't') {
      if (pos + 4 > len || 0 != strncmp(line + pos, "true", 4)) {
        return OB_INVALID_DATA;
      }
      pos += 4;
      entry.kind_ = JSON_SCALAR_BOOL;
      entry.str_val_ = "true";
    } else if (vc == 'f') {
      if (pos + 5 > len || 0 != strncmp(line + pos, "false", 5)) {
        return OB_INVALID_DATA;
      }
      pos += 5;
      entry.kind_ = JSON_SCALAR_BOOL;
      entry.str_val_ = "false";
    } else if (vc == 'n') {
      if (pos + 4 > len || 0 != strncmp(line + pos, "null", 4)) {
        return OB_INVALID_DATA;
      }
      pos += 4;
      entry.kind_ = JSON_SCALAR_NULL;
    } else if (vc == '-' || (vc >= '0' && vc <= '9')) {
      const size_t num_start = pos;
      if (line[pos] == '-') {
        ++pos;
        if (pos >= len || line[pos] < '0' || line[pos] > '9') {
          return OB_INVALID_DATA;
        }
      }
      if (line[pos] == '0') {
        ++pos;
        // Leading zero cannot be followed by another digit in strict JSON
        if (pos < len && line[pos] >= '0' && line[pos] <= '9') {
          return OB_INVALID_DATA;
        }
      } else {
        while (pos < len && line[pos] >= '0' && line[pos] <= '9') {
          ++pos;
        }
      }
      bool is_float = false;
      if (pos < len && line[pos] == '.') {
        is_float = true;
        ++pos;
        if (pos >= len || line[pos] < '0' || line[pos] > '9') {
          return OB_INVALID_DATA;
        }
        while (pos < len && line[pos] >= '0' && line[pos] <= '9') {
          ++pos;
        }
      }
      if (pos < len && (line[pos] == 'e' || line[pos] == 'E')) {
        is_float = true;
        ++pos;
        if (pos < len && (line[pos] == '+' || line[pos] == '-')) {
          ++pos;
        }
        if (pos >= len || line[pos] < '0' || line[pos] > '9') {
          return OB_INVALID_DATA;
        }
        while (pos < len && line[pos] >= '0' && line[pos] <= '9') {
          ++pos;
        }
      }
      entry.str_val_.assign(line + num_start, pos - num_start);
      ObString num_str(static_cast<int32_t>(entry.str_val_.size()), entry.str_val_.data());
      int64_t ival = 0;
      double dval = 0.0;
      if (!is_float && ObFileScanUtils::parse_strict_int64(num_str, ival)) {
        entry.kind_ = JSON_SCALAR_INT;
      } else if (ObFileScanUtils::parse_strict_double(num_str, dval)) {
        entry.kind_ = JSON_SCALAR_DOUBLE;
      } else {
        return OB_INVALID_DATA;
      }
    } else {
      return OB_INVALID_DATA;
    }

    entries.push_back(std::move(entry));
    skip_json_ws(line, len, pos);
    if (pos >= len) {
      return OB_INVALID_DATA;
    }
    if (line[pos] == ',') {
      ++pos;
      continue;
    } else if (line[pos] == '}') {
      ++pos;
      break;
    } else {
      return OB_INVALID_DATA;
    }
  }
  skip_json_ws(line, len, pos);
  return (pos == len) ? OB_SUCCESS : OB_INVALID_DATA;
}

class ObJsonlStreamingReader
{
public:
  static const size_t READ_BUF_SIZE = 65536;

  explicit ObJsonlStreamingReader(const ObFileScanOptions &opts)
    : opts_(opts),
      fd_(-1),
      buf_pos_(0),
      buf_len_(0),
      eof_(false),
      first_chunk_(true),
      line_no_(0)
  {}

  ~ObJsonlStreamingReader()
  {
    close();
  }

  int open()
  {
    close();
    char path_buf[PATH_MAX + 1] = {0};
    const ObString &p = opts_.canonical_path_.empty() ? opts_.file_path_ : opts_.canonical_path_;
    if (p.empty() || p.length() >= PATH_MAX) {
      return OB_INVALID_ARGUMENT;
    }
    MEMCPY(path_buf, p.ptr(), p.length());
    fd_ = ::open(path_buf, O_RDONLY);
    if (fd_ < 0) {
      return (errno == EACCES || errno == EPERM) ? OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS : OB_FILE_NOT_EXIST;
    }
    buf_pos_ = 0;
    buf_len_ = 0;
    eof_ = false;
    first_chunk_ = true;
    line_no_ = 0;
    return OB_SUCCESS;
  }

  int reset()
  {
    if (fd_ < 0) {
      return open();
    }
    if (::lseek(fd_, 0, SEEK_SET) < 0) {
      return OB_IO_ERROR;
    }
    buf_pos_ = 0;
    buf_len_ = 0;
    eof_ = false;
    first_chunk_ = true;
    line_no_ = 0;
    return OB_SUCCESS;
  }

  void close()
  {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  int read_next_object(std::vector<JsonFieldEntry> &entries, int64_t &out_line_no)
  {
    std::string line;
    while (true) {
      line.clear();
      bool got_any = false;
      while (true) {
        if (buf_pos_ >= buf_len_) {
          if (eof_) {
            break;
          }
          const ssize_t n = ::read(fd_, buf_, READ_BUF_SIZE);
          if (n < 0) {
            return OB_IO_ERROR;
          } else if (n == 0) {
            eof_ = true;
            break;
          }
          buf_pos_ = 0;
          buf_len_ = static_cast<size_t>(n);
          if (first_chunk_) {
            first_chunk_ = false;
            if (buf_len_ >= 3 &&
                static_cast<uint8_t>(buf_[0]) == 0xEF &&
                static_cast<uint8_t>(buf_[1]) == 0xBB &&
                static_cast<uint8_t>(buf_[2]) == 0xBF) {
              buf_pos_ = 3;
            }
          }
          if (buf_pos_ >= buf_len_) {
            continue;
          }
        }
        got_any = true;
        const char c = buf_[buf_pos_++];
        if (c == '\0') {
          return OB_INVALID_DATA;
        }
        if (c == '\n') {
          break;
        }
        line.push_back(c);
      }
      if (!got_any && line.empty()) {
        return OB_ITER_END;
      }
      ++line_no_;
      out_line_no = line_no_;
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      size_t ws_pos = 0;
      skip_json_ws(line.data(), line.size(), ws_pos);
      if (ws_pos == line.size()) {
        if (eof_ && buf_pos_ >= buf_len_) {
          return OB_ITER_END;
        }
        continue; // skip blank line
      }
      int ret = parse_flat_json_object_line(line.data(), line.size(), entries);
      if (OB_FAIL(ret)) {
        LOG_WARN("invalid JSONL line", K(opts_.file_path_), K(out_line_no), K(ret));
        return ret;
      }
      return OB_SUCCESS;
    }
  }

private:
  const ObFileScanOptions &opts_;
  int fd_;
  char buf_[READ_BUF_SIZE];
  size_t buf_pos_;
  size_t buf_len_;
  bool eof_;
  bool first_chunk_;
  int64_t line_no_;
};

class ObJsonlFileScanner final : public ObIFileScanner
{
public:
  ObJsonlFileScanner(const ObFileTableDef &table_def, ObExecContext &exec_ctx)
    : table_def_(table_def),
      exec_ctx_(exec_ctx),
      reader_(table_def_.options_),
      row_alloc_("JsonlScanRow"),
      opened_(false)
  {}

  ~ObJsonlFileScanner() override
  {
    close();
  }

  int open() override
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(ObFileScanUtils::check_file_sql_enabled(exec_ctx_.get_my_session()))) {
    } else if (OB_FAIL(ObFileScanUtils::verify_file_fingerprint(
                   table_def_.options_.canonical_path_, table_def_.options_.fingerprint_))) {
    } else if (OB_FAIL(reader_.open())) {
    } else {
      opened_ = true;
    }
    return ret;
  }

  int get_next_row(const ObIArray<ObExpr *> &column_exprs, ObEvalCtx &eval_ctx) override
  {
    int ret = OB_SUCCESS;
    if (!opened_) {
      return OB_NOT_INIT;
    }
    row_alloc_.reuse();
    std::vector<JsonFieldEntry> entries;
    int64_t line_no = 0;
    ret = reader_.read_next_object(entries, line_no);
    if (ret == OB_ITER_END) {
      if (OB_FAIL(ObFileScanUtils::verify_file_fingerprint(
              table_def_.options_.canonical_path_, table_def_.options_.fingerprint_))) {
        return ret;
      }
      return OB_ITER_END;
    } else if (OB_FAIL(ret)) {
      return ret;
    }

    const int64_t proj_cnt = column_exprs.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < proj_cnt; ++i) {
      const int32_t col_idx = (i < table_def_.access_col_indices_.count())
                                ? table_def_.access_col_indices_.at(i)
                                : static_cast<int32_t>(i);
      if (col_idx < 0 || col_idx >= table_def_.columns_.count()) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      const ObFileColumnDef &col_def = table_def_.columns_.at(col_idx);
      const JsonFieldEntry *found = NULL;
      for (size_t j = 0; j < entries.size(); ++j) {
        if (static_cast<int64_t>(entries[j].key_.size()) == col_def.name_.length() &&
            0 == MEMCMP(entries[j].key_.data(), col_def.name_.ptr(), col_def.name_.length())) {
          found = &entries[j];
          break;
        }
      }
      if (NULL == found || found->kind_ == JSON_SCALAR_NULL) {
        ret = write_scalar_to_datum(
            col_def, ObString(), true, row_alloc_, column_exprs.at(i), eval_ctx);
      } else {
        ObString val_str(static_cast<int32_t>(found->str_val_.size()), found->str_val_.data());
        ret = write_scalar_to_datum(
            col_def, val_str, false, row_alloc_, column_exprs.at(i), eval_ctx);
      }
    }
    return ret;
  }

  int rescan() override
  {
    row_alloc_.reuse();
    int ret = ObFileScanUtils::verify_file_fingerprint(
        table_def_.options_.canonical_path_, table_def_.options_.fingerprint_);
    if (OB_SUCC(ret)) {
      ret = reader_.reset();
    }
    return ret;
  }

  int close() override
  {
    reader_.close();
    row_alloc_.reset();
    opened_ = false;
    return OB_SUCCESS;
  }

private:
  const ObFileTableDef &table_def_;
  ObExecContext &exec_ctx_;
  ObJsonlStreamingReader reader_;
  ObArenaAllocator row_alloc_;
  bool opened_;
};

} // anonymous namespace

int ObFileScanUtils::infer_jsonl_schema(ObIAllocator &allocator, ObFileTableDef &table_def)
{
  int ret = OB_SUCCESS;
  table_def.columns_.reuse();

  ObJsonlStreamingReader reader(table_def.options_);
  if (OB_FAIL(reader.open())) {
    return ret;
  }

  struct JsonColState
  {
    std::string raw_name_;
    ObFileLogicalType type_;
    int64_t non_null_cnt_;
    int16_t max_dt_scale_;
  };
  std::vector<JsonColState> cols;
  std::vector<JsonFieldEntry> entries;
  int64_t line_no = 0;
  int64_t total_rows = 0;

  while (OB_SUCC(ret)) {
    int r = reader.read_next_object(entries, line_no);
    if (r == OB_ITER_END) {
      break;
    } else if (OB_FAIL(r)) {
      ret = r;
      break;
    }
    ++total_rows;
    for (size_t i = 0; i < entries.size(); ++i) {
      const JsonFieldEntry &e = entries[i];
      size_t col_idx = cols.size();
      for (size_t j = 0; j < cols.size(); ++j) {
        if (cols[j].raw_name_ == e.key_) {
          col_idx = j;
          break;
        }
      }
      if (col_idx == cols.size()) {
        JsonColState st;
        st.raw_name_ = e.key_;
        st.type_ = OB_FILE_TYPE_NULL;
        st.non_null_cnt_ = 0;
        st.max_dt_scale_ = 0;
        cols.push_back( std::move(st) );
      }
      ObFileLogicalType elem_type = OB_FILE_TYPE_NULL;
      if (e.kind_ == JSON_SCALAR_NULL) {
        elem_type = OB_FILE_TYPE_NULL;
      } else {
        ++cols[col_idx].non_null_cnt_;
        if (e.kind_ == JSON_SCALAR_BOOL) {
          elem_type = OB_FILE_TYPE_BOOLEAN;
        } else if (e.kind_ == JSON_SCALAR_INT) {
          elem_type = OB_FILE_TYPE_BIGINT;
        } else if (e.kind_ == JSON_SCALAR_DOUBLE) {
          elem_type = OB_FILE_TYPE_DOUBLE;
        } else {
          ObString sv(static_cast<int32_t>(e.str_val_.size()), e.str_val_.data());
          elem_type = infer_scalar_string_type(sv, false, true /*is_quoted_in_json*/);
          if (elem_type == OB_FILE_TYPE_DATETIME && sv.length() > 19 && sv.ptr()[19] == '.') {
            int16_t sc = static_cast<int16_t>(sv.length() - 20);
            if (sc > cols[col_idx].max_dt_scale_) {
              cols[col_idx].max_dt_scale_ = sc;
            }
          }
        }
      }
      cols[col_idx].type_ = merge_logical_types(cols[col_idx].type_, elem_type);
    }
  }

  if (OB_SUCC(ret)) {
    if (total_rows == 0 || cols.empty()) {
      LOG_WARN("empty JSONL file or no fields", K(table_def.options_.file_path_));
      return OB_INVALID_DATA;
    }
    for (size_t i = 0; OB_SUCC(ret) && i < cols.size(); ++i) {
      ObFileColumnDef col;
      if (OB_FAIL(ob_write_string(allocator,
                                  ObString(static_cast<int32_t>(cols[i].raw_name_.size()),
                                           cols[i].raw_name_.data()),
                                  col.name_,
                                  true))) {
        break;
      }
      col.logical_type_ = (cols[i].type_ == OB_FILE_TYPE_NULL) ? OB_FILE_TYPE_VARCHAR : cols[i].type_;
      col.is_nullable_ = (cols[i].non_null_cnt_ < total_rows);
      logical_type_to_ob_meta(col.logical_type_, col.meta_, col.accuracy_);
      if (col.logical_type_ == OB_FILE_TYPE_DATETIME && cols[i].max_dt_scale_ > 0) {
        col.accuracy_.set_scale(std::min<int16_t>(cols[i].max_dt_scale_, 6));
      }
      ret = table_def.columns_.push_back(col);
    }
    if (OB_SUCC(ret)) {
      ret = deduplicate_column_names(allocator, table_def.columns_);
    }
  }
  return ret;
}

// ============================================================================
// Parquet Native Reader & Scanner (Thrift Compact Protocol + Page Decoder)
// ============================================================================
namespace
{

enum ParquetPhysicalType : int32_t
{
  PQ_BOOLEAN              = 0,
  PQ_INT32                = 1,
  PQ_INT64                = 2,
  PQ_INT96                = 3,
  PQ_FLOAT                = 4,
  PQ_DOUBLE               = 5,
  PQ_BYTE_ARRAY           = 6,
  PQ_FIXED_LEN_BYTE_ARRAY = 7,
};

enum ParquetRepetitionType : int32_t
{
  PQ_REQUIRED = 0,
  PQ_OPTIONAL = 1,
  PQ_REPEATED = 2,
};

enum ParquetConvertedType : int32_t
{
  PQ_CT_NONE             = -1,
  PQ_CT_UTF8             = 0,
  PQ_CT_MAP              = 1,
  PQ_CT_MAP_KEY_VALUE    = 2,
  PQ_CT_LIST             = 3,
  PQ_CT_ENUM             = 4,
  PQ_CT_DECIMAL          = 5,
  PQ_CT_DATE             = 6,
  PQ_CT_TIME_MILLIS      = 7,
  PQ_CT_TIME_MICROS      = 8,
  PQ_CT_TIMESTAMP_MILLIS = 9,
  PQ_CT_TIMESTAMP_MICROS = 10,
  PQ_CT_UINT_8           = 11,
  PQ_CT_UINT_16          = 12,
  PQ_CT_UINT_32          = 13,
  PQ_CT_UINT_64          = 14,
  PQ_CT_INT_8            = 15,
  PQ_CT_INT_16           = 16,
  PQ_CT_INT_32           = 17,
  PQ_CT_INT_64           = 18,
  PQ_CT_JSON             = 19,
  PQ_CT_BSON             = 20,
  PQ_CT_INTERVAL         = 21,
};

enum ParquetCodecType : int32_t
{
  PQ_CODEC_UNCOMPRESSED = 0,
  PQ_CODEC_SNAPPY       = 1,
  PQ_CODEC_GZIP         = 2,
  PQ_CODEC_LZO          = 3,
  PQ_CODEC_BROTLI       = 4,
  PQ_CODEC_LZ4          = 5,
  PQ_CODEC_ZSTD         = 6,
};

enum ParquetEncodingType : int32_t
{
  PQ_ENC_PLAIN            = 0,
  PQ_ENC_PLAIN_DICTIONARY = 2,
  PQ_ENC_RLE              = 3,
  PQ_ENC_BIT_PACKED       = 4,
  PQ_ENC_RLE_DICTIONARY   = 8,
};

enum ParquetPageType : int32_t
{
  PQ_PAGE_DATA       = 0,
  PQ_PAGE_INDEX      = 1,
  PQ_PAGE_DICTIONARY = 2,
  PQ_PAGE_DATA_V2    = 3,
};

class ThriftCompactDecoder
{
public:
  ThriftCompactDecoder(const uint8_t *buf, int64_t len)
    : buf_(buf), len_(len), pos_(0)
  {}

  int64_t position() const { return pos_; }
  int64_t remaining() const { return len_ - pos_; }

  bool read_byte(uint8_t &out)
  {
    if (pos_ >= len_) {
      return false;
    }
    out = buf_[pos_++];
    return true;
  }

  bool read_varint(uint64_t &out)
  {
    out = 0;
    int shift = 0;
    while (pos_ < len_) {
      const uint8_t b = buf_[pos_++];
      out |= static_cast<uint64_t>(b & 0x7F) << shift;
      if ((b & 0x80) == 0) {
        return true;
      }
      shift += 7;
      if (shift >= 64) {
        return false;
      }
    }
    return false;
  }

  bool read_zigzag_i32(int32_t &out)
  {
    uint64_t u = 0;
    if (!read_varint(u)) {
      return false;
    }
    const uint32_t u32 = static_cast<uint32_t>(u);
    out = static_cast<int32_t>((u32 >> 1) ^ (~(u32 & 1) + 1));
    return true;
  }

  bool read_zigzag_i64(int64_t &out)
  {
    uint64_t u = 0;
    if (!read_varint(u)) {
      return false;
    }
    out = static_cast<int64_t>((u >> 1) ^ (~(u & 1ULL) + 1ULL));
    return true;
  }

  bool read_string(std::string &out)
  {
    uint64_t slen = 0;
    if (!read_varint(slen) || pos_ + static_cast<int64_t>(slen) > len_) {
      return false;
    }
    out.assign(reinterpret_cast<const char *>(buf_ + pos_), static_cast<size_t>(slen));
    pos_ += static_cast<int64_t>(slen);
    return true;
  }

  bool read_field_header(int16_t &last_field_id, int16_t &field_id, uint8_t &field_type)
  {
    uint8_t b = 0;
    if (!read_byte(b)) {
      return false;
    }
    if (b == 0) {
      field_type = 0;
      field_id = 0;
      return true;
    }
    const int16_t delta = static_cast<int16_t>((b & 0xF0) >> 4);
    field_type = static_cast<uint8_t>(b & 0x0F);
    if (delta == 0) {
      int32_t fid = 0;
      if (!read_zigzag_i32(fid)) {
        return false;
      }
      field_id = static_cast<int16_t>(fid);
    } else {
      field_id = static_cast<int16_t>(last_field_id + delta);
    }
    last_field_id = field_id;
    return true;
  }

  bool read_list_header(uint8_t &elem_type, int32_t &size)
  {
    uint8_t b = 0;
    if (!read_byte(b)) {
      return false;
    }
    elem_type = static_cast<uint8_t>(b & 0x0F);
    const uint8_t hi = static_cast<uint8_t>((b >> 4) & 0x0F);
    if (hi == 0x0F) {
      uint64_t v = 0;
      if (!read_varint(v) || v > static_cast<uint64_t>(INT32_MAX)) {
        return false;
      }
      size = static_cast<int32_t>(v);
    } else {
      size = static_cast<int32_t>(hi);
    }
    return true;
  }

  bool skip_value(uint8_t field_type, int depth = 0)
  {
    if (depth > 64) {
      return false;
    }
    switch (field_type) {
      case 1: // BOOL_TRUE
      case 2: // BOOL_FALSE
        return true;
      case 3: { // BYTE
        uint8_t dummy = 0;
        return read_byte(dummy);
      }
      case 4: // I16
      case 5: // I32
      case 6: { // I64
        uint64_t dummy = 0;
        return read_varint(dummy);
      }
      case 7: { // DOUBLE (8 bytes)
        if (pos_ + 8 > len_) {
          return false;
        }
        pos_ += 8;
        return true;
      }
      case 8: { // BINARY / STRING
        uint64_t slen = 0;
        if (!read_varint(slen) || pos_ + static_cast<int64_t>(slen) > len_) {
          return false;
        }
        pos_ += static_cast<int64_t>(slen);
        return true;
      }
      case 9:   // LIST
      case 10: { // SET
        uint8_t elem_type = 0;
        int32_t sz = 0;
        if (!read_list_header(elem_type, sz) || sz < 0) {
          return false;
        }
        for (int32_t i = 0; i < sz; ++i) {
          if (!skip_value(elem_type, depth + 1)) {
            return false;
          }
        }
        return true;
      }
      case 11: { // MAP
        uint64_t sz = 0;
        if (!read_varint(sz)) {
          return false;
        }
        if (sz == 0) {
          return true;
        }
        uint8_t kv_type = 0;
        if (!read_byte(kv_type)) {
          return false;
        }
        const uint8_t ktype = static_cast<uint8_t>((kv_type >> 4) & 0x0F);
        const uint8_t vtype = static_cast<uint8_t>(kv_type & 0x0F);
        for (uint64_t i = 0; i < sz; ++i) {
          if (!skip_value(ktype, depth + 1) || !skip_value(vtype, depth + 1)) {
            return false;
          }
        }
        return true;
      }
      case 12: { // STRUCT
        int16_t last_fid = 0;
        while (true) {
          int16_t fid = 0;
          uint8_t ftype = 0;
          if (!read_field_header(last_fid, fid, ftype)) {
            return false;
          }
          if (ftype == 0) {
            return true;
          }
          if (!skip_value(ftype, depth + 1)) {
            return false;
          }
        }
      }
      case 13: { // UUID (16 bytes)
        if (pos_ + 16 > len_) {
          return false;
        }
        pos_ += 16;
        return true;
      }
      default:
        return false;
    }
  }

private:
  const uint8_t *buf_;
  int64_t len_;
  int64_t pos_;
};

struct ParquetSchemaElement
{
  bool has_type_ = false;
  int32_t type_ = -1;
  int32_t type_length_ = 0;
  bool has_repetition_ = false;
  int32_t repetition_type_ = PQ_REQUIRED;
  std::string name_;
  int32_t num_children_ = 0;
  int32_t converted_type_ = PQ_CT_NONE;
  int32_t logical_type_tag_ = -1; // 1=STRING, 2=MAP, 3=LIST, 4=ENUM, 5=DECIMAL, 6=DATE, 7=TIME, 8=TIMESTAMP, 10=INT
  bool ts_is_millis_ = false;
  bool ts_is_nanos_ = false;
  int32_t int_bit_width_ = 64;
  bool int_is_signed_ = true;
};

struct ParquetColumnChunkMeta
{
  int32_t type_ = -1;
  int32_t codec_ = PQ_CODEC_UNCOMPRESSED;
  int64_t num_values_ = 0;
  int64_t total_uncompressed_size_ = 0;
  int64_t total_compressed_size_ = 0;
  int64_t data_page_offset_ = -1;
  int64_t dictionary_page_offset_ = -1;
  bool has_dictionary_page_offset_ = false;
};

struct ParquetRowGroupMeta
{
  int64_t num_rows_ = 0;
  std::vector<ParquetColumnChunkMeta> columns_;
};

struct ParquetFileMeta
{
  int64_t file_size_ = 0;
  int64_t num_rows_ = 0;
  std::vector<ParquetSchemaElement> leaf_columns_;
  std::vector<ParquetRowGroupMeta> row_groups_;
};

static bool parse_parquet_logical_type(ThriftCompactDecoder &dec, ParquetSchemaElement &elem)
{
  int16_t last_fid = 0;
  while (true) {
    int16_t fid = 0;
    uint8_t ftype = 0;
    if (!dec.read_field_header(last_fid, fid, ftype)) {
      return false;
    }
    if (ftype == 0) {
      return true;
    }
    elem.logical_type_tag_ = fid;
    if (fid == 8 && ftype == 12) {
      // TimestampType struct: field 2 is TimeUnit union (1=MILLIS, 2=MICROS, 3=NANOS)
      int16_t ts_last = 0;
      while (true) {
        int16_t ts_fid = 0;
        uint8_t ts_ft = 0;
        if (!dec.read_field_header(ts_last, ts_fid, ts_ft)) {
          return false;
        }
        if (ts_ft == 0) {
          break;
        }
        if (ts_fid == 2 && ts_ft == 12) {
          int16_t u_last = 0;
          while (true) {
            int16_t u_fid = 0;
            uint8_t u_ft = 0;
            if (!dec.read_field_header(u_last, u_fid, u_ft)) {
              return false;
            }
            if (u_ft == 0) {
              break;
            }
            if (u_fid == 1) {
              elem.ts_is_millis_ = true;
            } else if (u_fid == 3) {
              elem.ts_is_nanos_ = true;
            }
            if (!dec.skip_value(u_ft)) {
              return false;
            }
          }
        } else if (!dec.skip_value(ts_ft)) {
          return false;
        }
      }
    } else if (fid == 10 && ftype == 12) {
      // IntType struct: field 1 = bitWidth (byte), field 2 = isSigned (bool)
      int16_t i_last = 0;
      while (true) {
        int16_t i_fid = 0;
        uint8_t i_ft = 0;
        if (!dec.read_field_header(i_last, i_fid, i_ft)) {
          return false;
        }
        if (i_ft == 0) {
          break;
        }
        if (i_fid == 1 && i_ft == 3) {
          uint8_t bw = 0;
          if (!dec.read_byte(bw)) {
            return false;
          }
          elem.int_bit_width_ = bw;
        } else if (i_fid == 2 && (i_ft == 1 || i_ft == 2)) {
          elem.int_is_signed_ = (i_ft == 1);
        } else if (!dec.skip_value(i_ft)) {
          return false;
        }
      }
    } else if (!dec.skip_value(ftype)) {
      return false;
    }
  }
}

static bool parse_parquet_schema_element(ThriftCompactDecoder &dec, ParquetSchemaElement &elem)
{
  elem = ParquetSchemaElement();
  int16_t last_fid = 0;
  while (true) {
    int16_t fid = 0;
    uint8_t ftype = 0;
    if (!dec.read_field_header(last_fid, fid, ftype)) {
      return false;
    }
    if (ftype == 0) {
      return true;
    }
    if (fid == 1 && ftype == 5) {
      elem.has_type_ = true;
      if (!dec.read_zigzag_i32(elem.type_)) {
        return false;
      }
    } else if (fid == 2 && ftype == 5) {
      if (!dec.read_zigzag_i32(elem.type_length_)) {
        return false;
      }
    } else if (fid == 3 && ftype == 5) {
      elem.has_repetition_ = true;
      if (!dec.read_zigzag_i32(elem.repetition_type_)) {
        return false;
      }
    } else if (fid == 4 && ftype == 8) {
      if (!dec.read_string(elem.name_)) {
        return false;
      }
    } else if (fid == 5 && ftype == 5) {
      if (!dec.read_zigzag_i32(elem.num_children_)) {
        return false;
      }
    } else if (fid == 6 && ftype == 5) {
      if (!dec.read_zigzag_i32(elem.converted_type_)) {
        return false;
      }
    } else if (fid == 10 && ftype == 12) {
      if (!parse_parquet_logical_type(dec, elem)) {
        return false;
      }
    } else if (!dec.skip_value(ftype)) {
      return false;
    }
  }
}

static bool parse_parquet_column_meta(ThriftCompactDecoder &dec, ParquetColumnChunkMeta &cm)
{
  int16_t last_fid = 0;
  while (true) {
    int16_t fid = 0;
    uint8_t ftype = 0;
    if (!dec.read_field_header(last_fid, fid, ftype)) {
      return false;
    }
    if (ftype == 0) {
      return true;
    }
    if (fid == 1 && ftype == 5) {
      if (!dec.read_zigzag_i32(cm.type_)) {
        return false;
      }
    } else if (fid == 4 && ftype == 5) {
      if (!dec.read_zigzag_i32(cm.codec_)) {
        return false;
      }
    } else if (fid == 5 && ftype == 6) {
      if (!dec.read_zigzag_i64(cm.num_values_)) {
        return false;
      }
    } else if (fid == 6 && ftype == 6) {
      if (!dec.read_zigzag_i64(cm.total_uncompressed_size_)) {
        return false;
      }
    } else if (fid == 7 && ftype == 6) {
      if (!dec.read_zigzag_i64(cm.total_compressed_size_)) {
        return false;
      }
    } else if (fid == 9 && ftype == 6) {
      if (!dec.read_zigzag_i64(cm.data_page_offset_)) {
        return false;
      }
    } else if (fid == 11 && ftype == 6) {
      cm.has_dictionary_page_offset_ = true;
      if (!dec.read_zigzag_i64(cm.dictionary_page_offset_)) {
        return false;
      }
    } else if (!dec.skip_value(ftype)) {
      return false;
    }
  }
}

static bool parse_parquet_column_chunk(ThriftCompactDecoder &dec, ParquetColumnChunkMeta &cm)
{
  cm = ParquetColumnChunkMeta();
  int64_t file_offset = 0;
  bool has_meta = false;
  int16_t last_fid = 0;
  while (true) {
    int16_t fid = 0;
    uint8_t ftype = 0;
    if (!dec.read_field_header(last_fid, fid, ftype)) {
      return false;
    }
    if (ftype == 0) {
      if (!has_meta && file_offset > 0) {
        return false;
      }
      return true;
    }
    if (fid == 1 && ftype == 8) {
      // external file_path not supported
      std::string ext_path;
      if (!dec.read_string(ext_path) || !ext_path.empty()) {
        return false;
      }
    } else if (fid == 2 && ftype == 6) {
      if (!dec.read_zigzag_i64(file_offset)) {
        return false;
      }
    } else if (fid == 3 && ftype == 12) {
      has_meta = true;
      if (!parse_parquet_column_meta(dec, cm)) {
        return false;
      }
    } else if (!dec.skip_value(ftype)) {
      return false;
    }
  }
}

static bool parse_parquet_row_group(ThriftCompactDecoder &dec, ParquetRowGroupMeta &rg)
{
  rg = ParquetRowGroupMeta();
  int16_t last_fid = 0;
  while (true) {
    int16_t fid = 0;
    uint8_t ftype = 0;
    if (!dec.read_field_header(last_fid, fid, ftype)) {
      return false;
    }
    if (ftype == 0) {
      return true;
    }
    if (fid == 1 && ftype == 9) {
      uint8_t elem_type = 0;
      int32_t cnt = 0;
      if (!dec.read_list_header(elem_type, cnt) || elem_type != 12 || cnt < 0) {
        return false;
      }
      rg.columns_.resize(static_cast<size_t>(cnt));
      for (int32_t i = 0; i < cnt; ++i) {
        if (!parse_parquet_column_chunk(dec, rg.columns_[i])) {
          return false;
        }
      }
    } else if (fid == 3 && ftype == 6) {
      if (!dec.read_zigzag_i64(rg.num_rows_)) {
        return false;
      }
    } else if (!dec.skip_value(ftype)) {
      return false;
    }
  }
}

static int parse_parquet_footer(int fd, const ObString &file_path, ParquetFileMeta &meta)
{
  int ret = OB_SUCCESS;
  meta = ParquetFileMeta();
  struct stat st;
  if (0 != ::fstat(fd, &st) || st.st_size < 12) {
    LOG_WARN("Parquet file too small", K(file_path));
    return OB_INVALID_DATA;
  }
  meta.file_size_ = static_cast<int64_t>(st.st_size);

  uint8_t head_magic[4] = {0};
  if (::pread(fd, head_magic, 4, 0) != 4 || 0 != MEMCMP(head_magic, "PAR1", 4)) {
    LOG_WARN("invalid Parquet header magic", K(file_path));
    return OB_INVALID_DATA;
  }
  uint8_t tail_buf[8] = {0};
  if (::pread(fd, tail_buf, 8, meta.file_size_ - 8) != 8 ||
      0 != MEMCMP(tail_buf + 4, "PAR1", 4)) {
    LOG_WARN("invalid Parquet trailer magic", K(file_path));
    return OB_INVALID_DATA;
  }
  const uint32_t footer_len = static_cast<uint32_t>(tail_buf[0]) |
                              (static_cast<uint32_t>(tail_buf[1]) << 8) |
                              (static_cast<uint32_t>(tail_buf[2]) << 16) |
                              (static_cast<uint32_t>(tail_buf[3]) << 24);
  if (footer_len == 0 || static_cast<int64_t>(footer_len) + 12 > meta.file_size_) {
    LOG_WARN("invalid Parquet footer length", K(file_path), K(footer_len), K(meta.file_size_));
    return OB_INVALID_DATA;
  }

  std::vector<uint8_t> footer_bytes(footer_len);
  const int64_t footer_off = meta.file_size_ - 8 - static_cast<int64_t>(footer_len);
  if (::pread(fd, footer_bytes.data(), footer_len, footer_off) != static_cast<ssize_t>(footer_len)) {
    return OB_IO_ERROR;
  }

  ThriftCompactDecoder dec(footer_bytes.data(), static_cast<int64_t>(footer_len));
  std::vector<ParquetSchemaElement> all_schema;
  int16_t last_fid = 0;
  while (true) {
    int16_t fid = 0;
    uint8_t ftype = 0;
    if (!dec.read_field_header(last_fid, fid, ftype)) {
      return OB_INVALID_DATA;
    }
    if (ftype == 0) {
      break;
    }
    if (fid == 2 && ftype == 9) {
      uint8_t elem_type = 0;
      int32_t cnt = 0;
      if (!dec.read_list_header(elem_type, cnt) || elem_type != 12 || cnt <= 1) {
        return OB_INVALID_DATA;
      }
      all_schema.resize(static_cast<size_t>(cnt));
      for (int32_t i = 0; i < cnt; ++i) {
        if (!parse_parquet_schema_element(dec, all_schema[i])) {
          return OB_INVALID_DATA;
        }
      }
    } else if (fid == 3 && ftype == 6) {
      if (!dec.read_zigzag_i64(meta.num_rows_) || meta.num_rows_ < 0) {
        return OB_INVALID_DATA;
      }
    } else if (fid == 4 && ftype == 9) {
      uint8_t elem_type = 0;
      int32_t cnt = 0;
      if (!dec.read_list_header(elem_type, cnt) || elem_type != 12 || cnt < 0) {
        return OB_INVALID_DATA;
      }
      meta.row_groups_.resize(static_cast<size_t>(cnt));
      for (int32_t i = 0; i < cnt; ++i) {
        if (!parse_parquet_row_group(dec, meta.row_groups_[i])) {
          return OB_INVALID_DATA;
        }
      }
    } else if (!dec.skip_value(ftype)) {
      return OB_INVALID_DATA;
    }
  }

  if (all_schema.size() <= 1) {
    return OB_INVALID_DATA;
  }
  const ParquetSchemaElement &root = all_schema[0];
  if (root.num_children_ <= 0 ||
      static_cast<size_t>(root.num_children_) + 1 != all_schema.size()) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested Parquet schema (LIST/MAP/STRUCT)");
    return OB_NOT_SUPPORTED;
  }
  for (size_t i = 1; i < all_schema.size(); ++i) {
    const ParquetSchemaElement &se = all_schema[i];
    if (!se.has_type_ || se.num_children_ > 0 || se.repetition_type_ == PQ_REPEATED) {
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested or repeated Parquet column");
      return OB_NOT_SUPPORTED;
    }
    meta.leaf_columns_.push_back(se);
  }
  for (size_t rg_idx = 0; rg_idx < meta.row_groups_.size(); ++rg_idx) {
    if (meta.row_groups_[rg_idx].columns_.size() != meta.leaf_columns_.size()) {
      return OB_INVALID_DATA;
    }
  }
  return OB_SUCCESS;
}

static int map_parquet_schema_to_logical_type(const ParquetSchemaElement &se,
                                              ObFileLogicalType &out_type)
{
  if (se.converted_type_ == PQ_CT_DECIMAL || se.logical_type_tag_ == 5 ||
      se.converted_type_ == PQ_CT_MAP || se.converted_type_ == PQ_CT_LIST ||
      se.converted_type_ == PQ_CT_INTERVAL || se.converted_type_ == PQ_CT_UINT_64 ||
      (se.logical_type_tag_ == 10 && !se.int_is_signed_ && se.int_bit_width_ == 64) ||
      (se.logical_type_tag_ == 8 && se.ts_is_nanos_)) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "unsupported Parquet column logical type");
    return OB_NOT_SUPPORTED;
  }
  switch (se.type_) {
    case PQ_BOOLEAN:
      out_type = OB_FILE_TYPE_BOOLEAN;
      return OB_SUCCESS;
    case PQ_INT32:
      if (se.converted_type_ == PQ_CT_DATE || se.logical_type_tag_ == 6) {
        out_type = OB_FILE_TYPE_DATE;
      } else if (se.converted_type_ == PQ_CT_TIME_MILLIS || se.logical_type_tag_ == 7) {
        return OB_NOT_SUPPORTED;
      } else {
        out_type = OB_FILE_TYPE_BIGINT;
      }
      return OB_SUCCESS;
    case PQ_INT64:
      if (se.converted_type_ == PQ_CT_TIMESTAMP_MILLIS ||
          se.converted_type_ == PQ_CT_TIMESTAMP_MICROS ||
          se.logical_type_tag_ == 8) {
        out_type = OB_FILE_TYPE_DATETIME;
      } else if (se.converted_type_ == PQ_CT_TIME_MICROS || se.logical_type_tag_ == 7) {
        return OB_NOT_SUPPORTED;
      } else {
        out_type = OB_FILE_TYPE_BIGINT;
      }
      return OB_SUCCESS;
    case PQ_FLOAT:
    case PQ_DOUBLE:
      out_type = OB_FILE_TYPE_DOUBLE;
      return OB_SUCCESS;
    case PQ_BYTE_ARRAY:
      if (se.converted_type_ == PQ_CT_NONE ||
          se.converted_type_ == PQ_CT_UTF8 ||
          se.converted_type_ == PQ_CT_JSON ||
          se.converted_type_ == PQ_CT_ENUM ||
          se.logical_type_tag_ == -1 ||
          se.logical_type_tag_ == 1 ||
          se.logical_type_tag_ == 4) {
        out_type = OB_FILE_TYPE_VARCHAR;
        return OB_SUCCESS;
      }
      return OB_NOT_SUPPORTED;
    case PQ_FIXED_LEN_BYTE_ARRAY:
    case PQ_INT96:
    default:
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "unsupported Parquet physical type");
      return OB_NOT_SUPPORTED;
  }
}

struct ParquetPageHeaderInfo
{
  int32_t page_type_ = -1;
  int32_t uncompressed_size_ = 0;
  int32_t compressed_size_ = 0;
  int32_t num_values_ = 0;
  int32_t encoding_ = PQ_ENC_PLAIN;
  int32_t def_level_encoding_ = PQ_ENC_RLE;
  int32_t rep_level_encoding_ = PQ_ENC_RLE;
  int32_t v2_def_levels_byte_len_ = 0;
  int32_t v2_rep_levels_byte_len_ = 0;
  bool v2_is_compressed_ = true;
};

static bool parse_parquet_page_header(ThriftCompactDecoder &dec, ParquetPageHeaderInfo &ph)
{
  ph = ParquetPageHeaderInfo();
  int16_t last_fid = 0;
  while (true) {
    int16_t fid = 0;
    uint8_t ftype = 0;
    if (!dec.read_field_header(last_fid, fid, ftype)) {
      return false;
    }
    if (ftype == 0) {
      return ph.page_type_ >= 0 && ph.uncompressed_size_ >= 0 && ph.compressed_size_ >= 0;
    }
    if (fid == 1 && ftype == 5) {
      if (!dec.read_zigzag_i32(ph.page_type_)) {
        return false;
      }
    } else if (fid == 2 && ftype == 5) {
      if (!dec.read_zigzag_i32(ph.uncompressed_size_)) {
        return false;
      }
    } else if (fid == 3 && ftype == 5) {
      if (!dec.read_zigzag_i32(ph.compressed_size_)) {
        return false;
      }
    } else if (fid == 5 && ftype == 12) {
      // DataPageHeader (v1)
      int16_t dp_last = 0;
      while (true) {
        int16_t dp_fid = 0;
        uint8_t dp_ft = 0;
        if (!dec.read_field_header(dp_last, dp_fid, dp_ft)) {
          return false;
        }
        if (dp_ft == 0) {
          break;
        }
        if (dp_fid == 1 && dp_ft == 5) {
          if (!dec.read_zigzag_i32(ph.num_values_)) {
            return false;
          }
        } else if (dp_fid == 2 && dp_ft == 5) {
          if (!dec.read_zigzag_i32(ph.encoding_)) {
            return false;
          }
        } else if (dp_fid == 3 && dp_ft == 5) {
          if (!dec.read_zigzag_i32(ph.def_level_encoding_)) {
            return false;
          }
        } else if (dp_fid == 4 && dp_ft == 5) {
          if (!dec.read_zigzag_i32(ph.rep_level_encoding_)) {
            return false;
          }
        } else if (!dec.skip_value(dp_ft)) {
          return false;
        }
      }
    } else if (fid == 7 && ftype == 12) {
      // DictionaryPageHeader
      int16_t dh_last = 0;
      while (true) {
        int16_t dh_fid = 0;
        uint8_t dh_ft = 0;
        if (!dec.read_field_header(dh_last, dh_fid, dh_ft)) {
          return false;
        }
        if (dh_ft == 0) {
          break;
        }
        if (dh_fid == 1 && dh_ft == 5) {
          if (!dec.read_zigzag_i32(ph.num_values_)) {
            return false;
          }
        } else if (dh_fid == 2 && dh_ft == 5) {
          if (!dec.read_zigzag_i32(ph.encoding_)) {
            return false;
          }
        } else if (!dec.skip_value(dh_ft)) {
          return false;
        }
      }
    } else if (fid == 8 && ftype == 12) {
      // DataPageHeaderV2
      ph.v2_is_compressed_ = true;
      int16_t v2_last = 0;
      while (true) {
        int16_t v2_fid = 0;
        uint8_t v2_ft = 0;
        if (!dec.read_field_header(v2_last, v2_fid, v2_ft)) {
          return false;
        }
        if (v2_ft == 0) {
          break;
        }
        if (v2_fid == 1 && v2_ft == 5) {
          if (!dec.read_zigzag_i32(ph.num_values_)) {
            return false;
          }
        } else if (v2_fid == 4 && v2_ft == 5) {
          if (!dec.read_zigzag_i32(ph.encoding_)) {
            return false;
          }
        } else if (v2_fid == 5 && v2_ft == 5) {
          if (!dec.read_zigzag_i32(ph.v2_def_levels_byte_len_)) {
            return false;
          }
        } else if (v2_fid == 6 && v2_ft == 5) {
          if (!dec.read_zigzag_i32(ph.v2_rep_levels_byte_len_)) {
            return false;
          }
        } else if (v2_fid == 7 && (v2_ft == 1 || v2_ft == 2)) {
          ph.v2_is_compressed_ = (v2_ft == 1);
        } else if (!dec.skip_value(v2_ft)) {
          return false;
        }
      }
    } else if (!dec.skip_value(ftype)) {
      return false;
    }
  }
}

// Decodes Parquet RLE / Bit-Packed hybrid stream of `num_values` integers with `bit_width` bits.
static bool decode_rle_bit_packed(const uint8_t *buf,
                                  int64_t len,
                                  int32_t bit_width,
                                  int32_t num_values,
                                  std::vector<uint32_t> &out_vals,
                                  int64_t &bytes_consumed)
{
  out_vals.clear();
  out_vals.reserve(static_cast<size_t>(num_values));
  bytes_consumed = 0;
  if (num_values == 0) {
    return true;
  }
  if (bit_width == 0) {
    out_vals.assign(static_cast<size_t>(num_values), 0);
    return true;
  }
  if (bit_width < 0 || bit_width > 32) {
    return false;
  }
  int64_t pos = 0;
  const uint32_t byte_width = static_cast<uint32_t>((bit_width + 7) / 8);
  const uint64_t mask = (bit_width == 32) ? 0xFFFFFFFFULL : ((1ULL << bit_width) - 1ULL);

  while (static_cast<int32_t>(out_vals.size()) < num_values) {
    // Read unsigned varint header
    uint64_t header = 0;
    int shift = 0;
    while (true) {
      if (pos >= len) {
        return false;
      }
      const uint8_t b = buf[pos++];
      header |= static_cast<uint64_t>(b & 0x7F) << shift;
      if ((b & 0x80) == 0) {
        break;
      }
      shift += 7;
      if (shift >= 35) {
        return false;
      }
    }
    if ((header & 1ULL) == 0) {
      // RLE run: count = header >> 1
      const uint64_t run_len = header >> 1;
      if (run_len == 0 || pos + static_cast<int64_t>(byte_width) > len) {
        return false;
      }
      uint32_t val = 0;
      for (uint32_t i = 0; i < byte_width; ++i) {
        val |= static_cast<uint32_t>(buf[pos++]) << (8 * i);
      }
      val &= static_cast<uint32_t>(mask);
      const int32_t to_take = std::min<int32_t>(
          static_cast<int32_t>(run_len), num_values - static_cast<int32_t>(out_vals.size()));
      for (int32_t i = 0; i < to_take; ++i) {
        out_vals.push_back(val);
      }
    } else {
      // Bit-packed run: num_groups = header >> 1, total = num_groups * 8 values
      const uint64_t num_groups = header >> 1;
      const uint64_t total_bits = num_groups * 8ULL * static_cast<uint64_t>(bit_width);
      const uint64_t total_bytes = total_bits / 8ULL;
      if (num_groups == 0 || pos + static_cast<int64_t>(total_bytes) > len) {
        return false;
      }
      const uint64_t total_pack_vals = num_groups * 8ULL;
      uint64_t bit_buf = 0;
      int bits_in_buf = 0;
      int64_t pack_pos = pos;
      for (uint64_t i = 0; i < total_pack_vals && static_cast<int32_t>(out_vals.size()) < num_values; ++i) {
        while (bits_in_buf < bit_width) {
          bit_buf |= static_cast<uint64_t>(buf[pack_pos++]) << bits_in_buf;
          bits_in_buf += 8;
        }
        const uint32_t v = static_cast<uint32_t>(bit_buf & mask);
        out_vals.push_back(v);
        bit_buf >>= bit_width;
        bits_in_buf -= bit_width;
      }
      pos += static_cast<int64_t>(total_bytes);
    }
  }
  bytes_consumed = pos;
  return true;
}

static int decompress_parquet_buffer(int32_t codec,
                                     const uint8_t *src,
                                     int64_t src_len,
                                     int64_t uncompressed_len,
                                     std::vector<uint8_t> &out_buf)
{
  if (src_len < 0 || uncompressed_len < 0) {
    return OB_INVALID_DATA;
  }
  out_buf.resize(static_cast<size_t>(uncompressed_len));
  if (uncompressed_len == 0) {
    return (src_len == 0) ? OB_SUCCESS : OB_INVALID_DATA;
  }
  if (codec == PQ_CODEC_UNCOMPRESSED) {
    if (src_len != uncompressed_len) {
      return OB_INVALID_DATA;
    }
    MEMCPY(out_buf.data(), src, static_cast<size_t>(uncompressed_len));
    return OB_SUCCESS;
  } else if (codec == PQ_CODEC_SNAPPY) {
    int64_t actual_len = 0;
    int ret = snappy_decompress_buffer(src, src_len, out_buf.data(), uncompressed_len, actual_len);
    if (OB_FAIL(ret) || actual_len != uncompressed_len) {
      return OB_INVALID_DATA;
    }
    return OB_SUCCESS;
  } else if (codec == PQ_CODEC_ZSTD) {
    ObArenaAllocator zstd_alloc("ZstdParq");
    zstd_1_3_8::ObZstdCompressor_1_3_8 zstd_comp(zstd_alloc);
    int64_t actual_len = 0;
    int ret = zstd_comp.decompress(reinterpret_cast<const char *>(src),
                                   src_len,
                                   reinterpret_cast<char *>(out_buf.data()),
                                   uncompressed_len,
                                   actual_len);
    if (OB_FAIL(ret) || actual_len != uncompressed_len) {
      return OB_INVALID_DATA;
    }
    return OB_SUCCESS;
  } else {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "unsupported Parquet compression codec");
    return OB_NOT_SUPPORTED;
  }
}

struct DecodedParquetValue
{
  bool is_null_ = false;
  int64_t int_val_ = 0;
  double dbl_val_ = 0.0;
  std::string str_val_;
};

// Decodes all values of one ColumnChunk in one RowGroup into `out_values`.
static int decode_parquet_column_chunk(int fd,
                                       int64_t file_size,
                                       const ParquetSchemaElement &se,
                                       const ParquetColumnChunkMeta &cm,
                                       int64_t expected_rows,
                                       std::vector<DecodedParquetValue> &out_values)
{
  out_values.clear();
  out_values.reserve(static_cast<size_t>(expected_rows));
  if (expected_rows == 0) {
    return OB_SUCCESS;
  }
  int64_t cur_offset = (cm.has_dictionary_page_offset_ && cm.dictionary_page_offset_ > 0)
                         ? cm.dictionary_page_offset_
                         : cm.data_page_offset_;
  if (cur_offset < 4 || cur_offset >= file_size) {
    return OB_INVALID_DATA;
  }

  std::vector<DecodedParquetValue> dict_values;
  bool has_dict = false;

  while (static_cast<int64_t>(out_values.size()) < expected_rows) {
    if (cur_offset >= file_size - 8) {
      return OB_INVALID_DATA;
    }
    const int64_t max_hdr_read = std::min<int64_t>(1024, file_size - 8 - cur_offset);
    uint8_t hdr_buf[1024];
    const ssize_t hdr_n = ::pread(fd, hdr_buf, static_cast<size_t>(max_hdr_read), cur_offset);
    if (hdr_n <= 0) {
      return OB_INVALID_DATA;
    }
    ThriftCompactDecoder hdr_dec(hdr_buf, static_cast<int64_t>(hdr_n));
    ParquetPageHeaderInfo ph;
    if (!parse_parquet_page_header(hdr_dec, ph)) {
      return OB_INVALID_DATA;
    }
    const int64_t payload_offset = cur_offset + hdr_dec.position();
    if (ph.compressed_size_ < 0 || ph.uncompressed_size_ < 0 ||
        payload_offset + ph.compressed_size_ > file_size - 8) {
      return OB_INVALID_DATA;
    }
    std::vector<uint8_t> comp_payload(static_cast<size_t>(ph.compressed_size_));
    if (ph.compressed_size_ > 0 &&
        ::pread(fd, comp_payload.data(), static_cast<size_t>(ph.compressed_size_), payload_offset) !=
            static_cast<ssize_t>(ph.compressed_size_)) {
      return OB_IO_ERROR;
    }
    cur_offset = payload_offset + ph.compressed_size_;

    if (ph.page_type_ == PQ_PAGE_DICTIONARY) {
      if (ph.encoding_ != PQ_ENC_PLAIN && ph.encoding_ != PQ_ENC_PLAIN_DICTIONARY) {
        return OB_NOT_SUPPORTED;
      }
      std::vector<uint8_t> dict_raw;
      int ret = decompress_parquet_buffer(
          cm.codec_, comp_payload.data(), ph.compressed_size_, ph.uncompressed_size_, dict_raw);
      if (OB_FAIL(ret)) {
        return ret;
      }
      dict_values.resize(static_cast<size_t>(ph.num_values_));
      int64_t dpos = 0;
      const int64_t dlen = static_cast<int64_t>(dict_raw.size());
      for (int32_t i = 0; i < ph.num_values_; ++i) {
        DecodedParquetValue &dv = dict_values[i];
        dv.is_null_ = false;
        if (se.type_ == PQ_INT32) {
          if (dpos + 4 > dlen) {
            return OB_INVALID_DATA;
          }
          int32_t v32 = 0;
          MEMCPY(&v32, dict_raw.data() + dpos, 4);
          dpos += 4;
          if (se.converted_type_ == PQ_CT_UINT_8 || se.converted_type_ == PQ_CT_UINT_16 ||
              se.converted_type_ == PQ_CT_UINT_32 ||
              (se.logical_type_tag_ == 10 && !se.int_is_signed_)) {
            dv.int_val_ = static_cast<int64_t>(static_cast<uint32_t>(v32));
          } else {
            dv.int_val_ = static_cast<int64_t>(v32);
          }
        } else if (se.type_ == PQ_INT64) {
          if (dpos + 8 > dlen) {
            return OB_INVALID_DATA;
          }
          int64_t v64 = 0;
          MEMCPY(&v64, dict_raw.data() + dpos, 8);
          dpos += 8;
          dv.int_val_ = v64;
        } else if (se.type_ == PQ_FLOAT) {
          if (dpos + 4 > dlen) {
            return OB_INVALID_DATA;
          }
          float vf = 0.0f;
          MEMCPY(&vf, dict_raw.data() + dpos, 4);
          dpos += 4;
          dv.dbl_val_ = static_cast<double>(vf);
        } else if (se.type_ == PQ_DOUBLE) {
          if (dpos + 8 > dlen) {
            return OB_INVALID_DATA;
          }
          double vd = 0.0;
          MEMCPY(&vd, dict_raw.data() + dpos, 8);
          dpos += 8;
          dv.dbl_val_ = vd;
        } else if (se.type_ == PQ_BYTE_ARRAY) {
          if (dpos + 4 > dlen) {
            return OB_INVALID_DATA;
          }
          uint32_t slen = 0;
          MEMCPY(&slen, dict_raw.data() + dpos, 4);
          dpos += 4;
          if (dpos + static_cast<int64_t>(slen) > dlen) {
            return OB_INVALID_DATA;
          }
          dv.str_val_.assign(reinterpret_cast<const char *>(dict_raw.data() + dpos), slen);
          dpos += slen;
        } else {
          return OB_NOT_SUPPORTED;
        }
      }
      has_dict = true;
      continue;
    }

    if (ph.page_type_ != PQ_PAGE_DATA && ph.page_type_ != PQ_PAGE_DATA_V2) {
      continue;
    }

    std::vector<uint32_t> def_levels;
    std::vector<uint8_t> values_buf;
    int64_t vpos = 0;

    if (ph.page_type_ == PQ_PAGE_DATA) {
      std::vector<uint8_t> page_raw;
      int ret = decompress_parquet_buffer(
          cm.codec_, comp_payload.data(), ph.compressed_size_, ph.uncompressed_size_, page_raw);
      if (OB_FAIL(ret)) {
        return ret;
      }
      int64_t ppos = 0;
      const int64_t plen = static_cast<int64_t>(page_raw.size());
      if (se.repetition_type_ == PQ_OPTIONAL) {
        if (ph.def_level_encoding_ != PQ_ENC_RLE) {
          return OB_NOT_SUPPORTED;
        }
        if (ppos + 4 > plen) {
          return OB_INVALID_DATA;
        }
        uint32_t rle_byte_len = 0;
        MEMCPY(&rle_byte_len, page_raw.data() + ppos, 4);
        ppos += 4;
        if (ppos + static_cast<int64_t>(rle_byte_len) > plen) {
          return OB_INVALID_DATA;
        }
        int64_t consumed = 0;
        if (!decode_rle_bit_packed(page_raw.data() + ppos,
                                   rle_byte_len,
                                   1,
                                   ph.num_values_,
                                   def_levels,
                                   consumed)) {
          return OB_INVALID_DATA;
        }
        ppos += static_cast<int64_t>(rle_byte_len);
      } else {
        def_levels.assign(static_cast<size_t>(ph.num_values_), 1);
      }
      values_buf = std::move(page_raw);
      vpos = ppos;
    } else {
      // DATA_PAGE_V2: rep levels and def levels are uncompressed at start of payload
      const int64_t levels_len =
          static_cast<int64_t>(ph.v2_rep_levels_byte_len_) +
          static_cast<int64_t>(ph.v2_def_levels_byte_len_);
      if (ph.v2_rep_levels_byte_len_ < 0 || ph.v2_def_levels_byte_len_ < 0 ||
          levels_len > ph.compressed_size_ || levels_len > ph.uncompressed_size_) {
        return OB_INVALID_DATA;
      }
      if (se.repetition_type_ == PQ_OPTIONAL) {
        int64_t consumed = 0;
        if (!decode_rle_bit_packed(comp_payload.data() + ph.v2_rep_levels_byte_len_,
                                   ph.v2_def_levels_byte_len_,
                                   1,
                                   ph.num_values_,
                                   def_levels,
                                   consumed)) {
          return OB_INVALID_DATA;
        }
      } else {
        def_levels.assign(static_cast<size_t>(ph.num_values_), 1);
      }
      const int64_t comp_vals_len = ph.compressed_size_ - levels_len;
      const int64_t uncomp_vals_len = ph.uncompressed_size_ - levels_len;
      const int32_t eff_codec = ph.v2_is_compressed_ ? cm.codec_ : PQ_CODEC_UNCOMPRESSED;
      int ret = decompress_parquet_buffer(eff_codec,
                                          comp_payload.data() + levels_len,
                                          comp_vals_len,
                                          uncomp_vals_len,
                                          values_buf);
      if (OB_FAIL(ret)) {
        return ret;
      }
      vpos = 0;
    }

    int32_t non_null_cnt = 0;
    for (int32_t i = 0; i < ph.num_values_; ++i) {
      if (def_levels[i] > 0) {
        ++non_null_cnt;
      }
    }

    const int64_t vlen = static_cast<int64_t>(values_buf.size());
    if (ph.encoding_ == PQ_ENC_PLAIN_DICTIONARY || ph.encoding_ == PQ_ENC_RLE_DICTIONARY) {
      if (!has_dict) {
        return OB_INVALID_DATA;
      }
      std::vector<uint32_t> dict_indices;
      if (non_null_cnt > 0) {
        if (vpos >= vlen) {
          return OB_INVALID_DATA;
        }
        const int32_t bit_width = static_cast<int32_t>(values_buf[vpos++]);
        int64_t consumed = 0;
        if (!decode_rle_bit_packed(values_buf.data() + vpos,
                                   vlen - vpos,
                                   bit_width,
                                   non_null_cnt,
                                   dict_indices,
                                   consumed)) {
          return OB_INVALID_DATA;
        }
      }
      int32_t nn_idx = 0;
      for (int32_t i = 0; i < ph.num_values_; ++i) {
        if (def_levels[i] == 0) {
          DecodedParquetValue nv;
          nv.is_null_ = true;
          out_values.push_back(std::move(nv));
        } else {
          const uint32_t didx = dict_indices[nn_idx++];
          if (didx >= dict_values.size()) {
            return OB_INVALID_DATA;
          }
          out_values.push_back(dict_values[didx]);
        }
      }
    } else if (ph.encoding_ == PQ_ENC_RLE && se.type_ == PQ_BOOLEAN) {
      std::vector<uint32_t> bool_vals;
      if (non_null_cnt > 0) {
        if (ph.page_type_ == PQ_PAGE_DATA) {
          if (vpos + 4 > vlen) {
            return OB_INVALID_DATA;
          }
          uint32_t rle_len = 0;
          MEMCPY(&rle_len, values_buf.data() + vpos, 4);
          vpos += 4;
          if (vpos + static_cast<int64_t>(rle_len) > vlen) {
            return OB_INVALID_DATA;
          }
          int64_t consumed = 0;
          if (!decode_rle_bit_packed(
                  values_buf.data() + vpos, rle_len, 1, non_null_cnt, bool_vals, consumed)) {
            return OB_INVALID_DATA;
          }
        } else {
          int64_t consumed = 0;
          if (!decode_rle_bit_packed(
                  values_buf.data() + vpos, vlen - vpos, 1, non_null_cnt, bool_vals, consumed)) {
            return OB_INVALID_DATA;
          }
        }
      }
      int32_t nn_idx = 0;
      for (int32_t i = 0; i < ph.num_values_; ++i) {
        DecodedParquetValue dv;
        if (def_levels[i] == 0) {
          dv.is_null_ = true;
        } else {
          dv.int_val_ = (bool_vals[nn_idx++] != 0) ? 1 : 0;
        }
        out_values.push_back(std::move(dv));
      }
    } else if (ph.encoding_ == PQ_ENC_PLAIN) {
      int32_t bool_bit_idx = 0;
      for (int32_t i = 0; i < ph.num_values_; ++i) {
        DecodedParquetValue dv;
        if (def_levels[i] == 0) {
          dv.is_null_ = true;
          out_values.push_back(std::move(dv));
          continue;
        }
        if (se.type_ == PQ_BOOLEAN) {
          const int64_t byte_off = vpos + (bool_bit_idx / 8);
          if (byte_off >= vlen) {
            return OB_INVALID_DATA;
          }
          dv.int_val_ = ((values_buf[byte_off] >> (bool_bit_idx % 8)) & 1);
          ++bool_bit_idx;
        } else if (se.type_ == PQ_INT32) {
          if (vpos + 4 > vlen) {
            return OB_INVALID_DATA;
          }
          int32_t v32 = 0;
          MEMCPY(&v32, values_buf.data() + vpos, 4);
          vpos += 4;
          if (se.converted_type_ == PQ_CT_UINT_8 || se.converted_type_ == PQ_CT_UINT_16 ||
              se.converted_type_ == PQ_CT_UINT_32 ||
              (se.logical_type_tag_ == 10 && !se.int_is_signed_)) {
            dv.int_val_ = static_cast<int64_t>(static_cast<uint32_t>(v32));
          } else {
            dv.int_val_ = static_cast<int64_t>(v32);
          }
        } else if (se.type_ == PQ_INT64) {
          if (vpos + 8 > vlen) {
            return OB_INVALID_DATA;
          }
          int64_t v64 = 0;
          MEMCPY(&v64, values_buf.data() + vpos, 8);
          vpos += 8;
          dv.int_val_ = v64;
        } else if (se.type_ == PQ_FLOAT) {
          if (vpos + 4 > vlen) {
            return OB_INVALID_DATA;
          }
          float vf = 0.0f;
          MEMCPY(&vf, values_buf.data() + vpos, 4);
          vpos += 4;
          dv.dbl_val_ = static_cast<double>(vf);
        } else if (se.type_ == PQ_DOUBLE) {
          if (vpos + 8 > vlen) {
            return OB_INVALID_DATA;
          }
          double vd = 0.0;
          MEMCPY(&vd, values_buf.data() + vpos, 8);
          vpos += 8;
          dv.dbl_val_ = vd;
        } else if (se.type_ == PQ_BYTE_ARRAY) {
          if (vpos + 4 > vlen) {
            return OB_INVALID_DATA;
          }
          uint32_t slen = 0;
          MEMCPY(&slen, values_buf.data() + vpos, 4);
          vpos += 4;
          if (vpos + static_cast<int64_t>(slen) > vlen) {
            return OB_INVALID_DATA;
          }
          dv.str_val_.assign(reinterpret_cast<const char *>(values_buf.data() + vpos), slen);
          vpos += slen;
        } else {
          return OB_NOT_SUPPORTED;
        }
        out_values.push_back(std::move(dv));
      }
    } else {
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "unsupported Parquet page encoding");
      return OB_NOT_SUPPORTED;
    }
  }
  return OB_SUCCESS;
}

class ObParquetFileScanner final : public ObIFileScanner
{
public:
  ObParquetFileScanner(const ObFileTableDef &table_def, ObExecContext &exec_ctx)
    : table_def_(table_def),
      exec_ctx_(exec_ctx),
      fd_(-1),
      meta_(),
      cur_rg_idx_(0),
      cur_row_in_rg_(0),
      rg_rows_(0),
      row_alloc_("ParquetScanRow"),
      opened_(false)
  {}

  ~ObParquetFileScanner() override
  {
    close();
  }

  int open() override
  {
    int ret = OB_SUCCESS;
    close();
    if (OB_FAIL(ObFileScanUtils::check_file_sql_enabled(exec_ctx_.get_my_session()))) {
      return ret;
    }
    if (OB_FAIL(ObFileScanUtils::verify_file_fingerprint(
            table_def_.options_.canonical_path_, table_def_.options_.fingerprint_))) {
      return ret;
    }
    char path_buf[PATH_MAX + 1] = {0};
    const ObString &p = table_def_.options_.canonical_path_.empty()
                          ? table_def_.options_.file_path_
                          : table_def_.options_.canonical_path_;
    if (p.empty() || p.length() >= PATH_MAX) {
      return OB_INVALID_ARGUMENT;
    }
    MEMCPY(path_buf, p.ptr(), p.length());
    fd_ = ::open(path_buf, O_RDONLY);
    if (fd_ < 0) {
      return (errno == EACCES || errno == EPERM) ? OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS : OB_FILE_NOT_EXIST;
    }
    if (OB_FAIL(parse_parquet_footer(fd_, table_def_.options_.file_path_, meta_))) {
      close();
      return ret;
    }
    cur_rg_idx_ = 0;
    cur_row_in_rg_ = 0;
    rg_rows_ = 0;
    decoded_cols_.clear();
    opened_ = true;
    return OB_SUCCESS;
  }

  int get_next_row(const ObIArray<ObExpr *> &column_exprs, ObEvalCtx &eval_ctx) override
  {
    int ret = OB_SUCCESS;
    if (!opened_) {
      return OB_NOT_INIT;
    }
    row_alloc_.reuse();

    while (cur_row_in_rg_ >= rg_rows_) {
      if (cur_rg_idx_ >= static_cast<int64_t>(meta_.row_groups_.size())) {
        if (OB_FAIL(ObFileScanUtils::verify_file_fingerprint(
                table_def_.options_.canonical_path_, table_def_.options_.fingerprint_))) {
          return ret;
        }
        return OB_ITER_END;
      }
      if (OB_FAIL(load_row_group(cur_rg_idx_, column_exprs.count()))) {
        return ret;
      }
      ++cur_rg_idx_;
    }

    const int64_t proj_cnt = column_exprs.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < proj_cnt; ++i) {
      const int32_t col_idx = (i < table_def_.access_col_indices_.count())
                                ? table_def_.access_col_indices_.at(i)
                                : static_cast<int32_t>(i);
      if (col_idx < 0 || col_idx >= table_def_.columns_.count() ||
          i >= static_cast<int64_t>(decoded_cols_.size()) ||
          cur_row_in_rg_ >= static_cast<int64_t>(decoded_cols_[i].size())) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      const ObFileColumnDef &col_def = table_def_.columns_.at(col_idx);
      const ParquetSchemaElement &se = meta_.leaf_columns_[col_idx];
      const DecodedParquetValue &dv = decoded_cols_[i][cur_row_in_rg_];
      ObExpr *expr = column_exprs.at(i);
      ObDatum &datum = expr->locate_datum_for_write(eval_ctx);
      if (dv.is_null_) {
        datum.set_null();
      } else {
        switch (col_def.logical_type_) {
          case OB_FILE_TYPE_BOOLEAN:
            datum.set_int(static_cast<int64_t>(dv.int_val_ != 0 ? 1 : 0));
            break;
          case OB_FILE_TYPE_BIGINT:
            datum.set_int(dv.int_val_);
            break;
          case OB_FILE_TYPE_DOUBLE:
            datum.set_double(dv.dbl_val_);
            break;
          case OB_FILE_TYPE_DATE:
            datum.set_date(static_cast<int32_t>(dv.int_val_));
            break;
          case OB_FILE_TYPE_DATETIME: {
            const bool is_millis =
                (se.converted_type_ == PQ_CT_TIMESTAMP_MILLIS) ||
                (se.logical_type_tag_ == 8 && se.ts_is_millis_);
            const int64_t usec = is_millis ? (dv.int_val_ * 1000LL) : dv.int_val_;
            datum.set_datetime(usec);
            break;
          }
          case OB_FILE_TYPE_VARCHAR:
          default: {
            if (!ObFileScanUtils::is_valid_utf8_no_nul(
                    dv.str_val_.data(), static_cast<int64_t>(dv.str_val_.size()))) {
              return OB_INVALID_DATA;
            }
            if (dv.str_val_.empty()) {
              datum.set_string(ObString(0, ""));
            } else {
              ObString copied;
              if (OB_FAIL(ob_write_string(
                      row_alloc_,
                      ObString(static_cast<int32_t>(dv.str_val_.size()), dv.str_val_.data()),
                      copied,
                      false))) {
                return ret;
              }
              datum.set_string(copied);
            }
            break;
          }
        }
      }
      expr->set_evaluated_projected(eval_ctx);
    }
    ++cur_row_in_rg_;
    return ret;
  }

  int rescan() override
  {
    int ret = ObFileScanUtils::verify_file_fingerprint(
        table_def_.options_.canonical_path_, table_def_.options_.fingerprint_);
    if (OB_SUCC(ret)) {
      cur_rg_idx_ = 0;
      cur_row_in_rg_ = 0;
      rg_rows_ = 0;
      decoded_cols_.clear();
      row_alloc_.reuse();
    }
    return ret;
  }

  int close() override
  {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    decoded_cols_.clear();
    row_alloc_.reset();
    opened_ = false;
    return OB_SUCCESS;
  }

private:
  int load_row_group(int64_t rg_idx, int64_t proj_cnt)
  {
    const ParquetRowGroupMeta &rg = meta_.row_groups_[rg_idx];
    rg_rows_ = rg.num_rows_;
    cur_row_in_rg_ = 0;
    decoded_cols_.clear();
    decoded_cols_.resize(static_cast<size_t>(proj_cnt));
    if (rg_rows_ == 0 || proj_cnt == 0) {
      return OB_SUCCESS;
    }
    // Column pruning: ONLY decode the projected columns in access_col_indices_!
    for (int64_t i = 0; i < proj_cnt; ++i) {
      const int32_t col_idx = (i < table_def_.access_col_indices_.count())
                                ? table_def_.access_col_indices_.at(i)
                                : static_cast<int32_t>(i);
      if (col_idx < 0 || col_idx >= static_cast<int32_t>(meta_.leaf_columns_.size())) {
        return OB_ERR_UNEXPECTED;
      }
      int ret = decode_parquet_column_chunk(fd_,
                                            meta_.file_size_,
                                            meta_.leaf_columns_[col_idx],
                                            rg.columns_[col_idx],
                                            rg_rows_,
                                            decoded_cols_[i]);
      if (OB_FAIL(ret)) {
        return ret;
      }
    }
    return OB_SUCCESS;
  }

  const ObFileTableDef &table_def_;
  ObExecContext &exec_ctx_;
  int fd_;
  ParquetFileMeta meta_;
  int64_t cur_rg_idx_;
  int64_t cur_row_in_rg_;
  int64_t rg_rows_;
  std::vector<std::vector<DecodedParquetValue>> decoded_cols_;
  ObArenaAllocator row_alloc_;
  bool opened_;
};

} // anonymous namespace

int ObFileScanUtils::infer_parquet_schema(ObIAllocator &allocator, ObFileTableDef &table_def)
{
  int ret = OB_SUCCESS;
  table_def.columns_.reuse();

  char path_buf[PATH_MAX + 1] = {0};
  const ObString &p = table_def.options_.canonical_path_.empty()
                        ? table_def.options_.file_path_
                        : table_def.options_.canonical_path_;
  if (p.empty() || p.length() >= PATH_MAX) {
    return OB_INVALID_ARGUMENT;
  }
  MEMCPY(path_buf, p.ptr(), p.length());
  int fd = ::open(path_buf, O_RDONLY);
  if (fd < 0) {
    return (errno == EACCES || errno == EPERM) ? OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS : OB_FILE_NOT_EXIST;
  }

  ParquetFileMeta meta;
  ret = parse_parquet_footer(fd, table_def.options_.file_path_, meta);
  ::close(fd);
  if (OB_FAIL(ret)) {
    return ret;
  }

  // Validate codecs in all row groups upfront
  for (size_t rg_i = 0; rg_i < meta.row_groups_.size(); ++rg_i) {
    const ParquetRowGroupMeta &rg = meta.row_groups_[rg_i];
    for (size_t c_i = 0; c_i < rg.columns_.size(); ++c_i) {
      const int32_t codec = rg.columns_[c_i].codec_;
      if (codec != PQ_CODEC_UNCOMPRESSED && codec != PQ_CODEC_SNAPPY && codec != PQ_CODEC_ZSTD) {
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "unsupported Parquet compression codec");
        return OB_NOT_SUPPORTED;
      }
    }
  }

  for (size_t i = 0; OB_SUCC(ret) && i < meta.leaf_columns_.size(); ++i) {
    const ParquetSchemaElement &se = meta.leaf_columns_[i];
    ObFileColumnDef col;
    col.parquet_col_idx_ = static_cast<int32_t>(i);
    if (OB_FAIL(map_parquet_schema_to_logical_type(se, col.logical_type_))) {
      break;
    }
    col.is_nullable_ = (se.repetition_type_ != PQ_REQUIRED);
    logical_type_to_ob_meta(col.logical_type_, col.meta_, col.accuracy_);
    if (col.logical_type_ == OB_FILE_TYPE_DATETIME) {
      if (se.converted_type_ == PQ_CT_TIMESTAMP_MILLIS) {
        col.accuracy_.set_scale(3);
      } else if (se.converted_type_ == PQ_CT_TIMESTAMP_MICROS) {
        col.accuracy_.set_scale(6);
      }
    }
    if (OB_FAIL(ob_write_string(allocator,
                                ObString(static_cast<int32_t>(se.name_.size()), se.name_.data()),
                                col.name_,
                                true))) {
      break;
    }
    ret = table_def.columns_.push_back(col);
  }
  if (OB_SUCC(ret)) {
    ret = deduplicate_column_names(allocator, table_def.columns_);
  }
  return ret;
}

// ============================================================================
// glob() Schema & Scanner
// ============================================================================
namespace
{

class ObGlobFileScanner final : public ObIFileScanner
{
public:
  ObGlobFileScanner(const ObFileTableDef &table_def, ObExecContext &exec_ctx)
    : table_def_(table_def),
      exec_ctx_(exec_ctx),
      row_alloc_("GlobScan"),
      files_(),
      idx_(0),
      opened_(false)
  {}

  ~ObGlobFileScanner() override
  {
    close();
  }

  int open() override
  {
    int ret = OB_SUCCESS;
    row_alloc_.reuse();
    files_.reuse();
    idx_ = 0;
    ObSQLSessionInfo *session = exec_ctx_.get_my_session();
    ObString secure_file_priv;
    if (OB_FAIL(ObFileScanUtils::check_file_sql_enabled(session))) {
    } else if (OB_FAIL(ObFileScanUtils::get_effective_secure_file_priv(
                   session, row_alloc_, secure_file_priv))) {
    } else if (OB_FAIL(ObFileScanUtils::validate_secure_glob_pattern(
                   secure_file_priv, table_def_.options_.file_path_, row_alloc_, files_))) {
    } else {
      opened_ = true;
    }
    return ret;
  }

  int get_next_row(const ObIArray<ObExpr *> &column_exprs, ObEvalCtx &eval_ctx) override
  {
    if (!opened_) {
      return OB_NOT_INIT;
    }
    if (idx_ >= files_.count()) {
      return OB_ITER_END;
    }
    const ObString &fpath = files_.at(idx_++);
    if (column_exprs.count() > 0 && OB_NOT_NULL(column_exprs.at(0))) {
      ObDatum &datum = column_exprs.at(0)->locate_datum_for_write(eval_ctx);
      datum.set_string(fpath);
      column_exprs.at(0)->set_evaluated_projected(eval_ctx);
    }
    return OB_SUCCESS;
  }

  int rescan() override
  {
    idx_ = 0;
    return OB_SUCCESS;
  }

  int close() override
  {
    files_.reset();
    row_alloc_.reset();
    idx_ = 0;
    opened_ = false;
    return OB_SUCCESS;
  }

private:
  const ObFileTableDef &table_def_;
  ObExecContext &exec_ctx_;
  ObArenaAllocator row_alloc_;
  ObSEArray<ObString, 16> files_;
  int64_t idx_;
  bool opened_;
};

} // anonymous namespace

int ObFileScanUtils::infer_glob_schema(ObIAllocator &allocator,
                                       const ObSQLSessionInfo *session,
                                       ObFileTableDef &table_def)
{
  int ret = OB_SUCCESS;
  table_def.columns_.reuse();
  ObString secure_file_priv;
  ObSEArray<ObString, 16> dummy_files;
  if (OB_FAIL(get_effective_secure_file_priv(session, allocator, secure_file_priv))) {
  } else if (OB_FAIL(validate_secure_glob_pattern(
                 secure_file_priv, table_def.options_.file_path_, allocator, dummy_files))) {
  } else {
    ObFileColumnDef col;
    if (OB_FAIL(ob_write_string(allocator, ObString::make_string("file"), col.name_, true))) {
    } else {
      col.logical_type_ = OB_FILE_TYPE_VARCHAR;
      col.is_nullable_ = false;
      logical_type_to_ob_meta(col.logical_type_, col.meta_, col.accuracy_);
      ret = table_def.columns_.push_back(col);
    }
  }
  return ret;
}

int ObFileScanUtils::infer_file_table_schema(ObIAllocator &allocator,
                                             const ObSQLSessionInfo *session,
                                             ObFileTableDef &table_def)
{
  int ret = OB_SUCCESS;
  if (table_def.options_.format_ == OB_FILE_FORMAT_GLOB) {
    return infer_glob_schema(allocator, session, table_def);
  }
  ObString secure_file_priv;
  if (OB_FAIL(get_effective_secure_file_priv(session, allocator, secure_file_priv))) {
    return ret;
  }
  if (OB_FAIL(validate_secure_read_path(secure_file_priv,
                                        table_def.options_.file_path_,
                                        allocator,
                                        table_def.options_.canonical_path_,
                                        table_def.options_.fingerprint_))) {
    return ret;
  }
  switch (table_def.options_.format_) {
    case OB_FILE_FORMAT_CSV:
      return infer_csv_schema(allocator, table_def);
    case OB_FILE_FORMAT_JSONL:
      return infer_jsonl_schema(allocator, table_def);
    case OB_FILE_FORMAT_PARQUET:
      return infer_parquet_schema(allocator, table_def);
    default:
      return OB_NOT_SUPPORTED;
  }
}

// ============================================================================
// Encoding / Decoding & Resolver / Executor Hooks
// ============================================================================

int ObFileScanUtils::encode_file_table_expr(ObIAllocator &allocator,
                                            ObRawExprFactory &expr_factory,
                                            ObFileTableDef *table_def,
                                            ObRawExpr *&out_expr)
{
  int ret = OB_SUCCESS;
  out_expr = NULL;
  if (OB_ISNULL(table_def)) {
    return OB_INVALID_ARGUMENT;
  }
  const int64_t total_len = FILE_TABLE_EXPR_PREFIX_LEN + static_cast<int64_t>(sizeof(uintptr_t));
  char *buf = static_cast<char *>(allocator.alloc(total_len));
  if (OB_ISNULL(buf)) {
    return OB_ALLOCATE_MEMORY_FAILED;
  }
  MEMCPY(buf, FILE_TABLE_EXPR_PREFIX, FILE_TABLE_EXPR_PREFIX_LEN);
  const uintptr_t ptr_val = reinterpret_cast<uintptr_t>(table_def);
  MEMCPY(buf + FILE_TABLE_EXPR_PREFIX_LEN, &ptr_val, sizeof(uintptr_t));

  ObConstRawExpr *c_expr = NULL;
  if (OB_FAIL(expr_factory.create_raw_expr(T_VARCHAR, c_expr)) || OB_ISNULL(c_expr)) {
    return OB_FAIL(ret) ? ret : OB_ALLOCATE_MEMORY_FAILED;
  }
  ObObj val;
  val.set_varchar(ObString(static_cast<int32_t>(total_len), buf));
  val.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  val.set_collation_level(CS_LEVEL_COERCIBLE);
  c_expr->set_value(val);
  out_expr = c_expr;
  return OB_SUCCESS;
}

bool ObFileScanUtils::is_file_table_expr(const ObRawExpr *expr, ObFileTableDef *&out_def)
{
  out_def = NULL;
  if (NULL == expr || expr->get_expr_type() != T_VARCHAR) {
    return false;
  }
  const ObConstRawExpr *c_expr = static_cast<const ObConstRawExpr *>(expr);
  const ObObj &val = c_expr->get_value();
  if (!val.is_varchar_or_char()) {
    return false;
  }
  const ObString s = val.get_varchar();
  const int64_t expected_len = FILE_TABLE_EXPR_PREFIX_LEN + static_cast<int64_t>(sizeof(uintptr_t));
  if (s.length() != expected_len || NULL == s.ptr() ||
      0 != MEMCMP(s.ptr(), FILE_TABLE_EXPR_PREFIX, FILE_TABLE_EXPR_PREFIX_LEN)) {
    return false;
  }
  uintptr_t ptr_val = 0;
  MEMCPY(&ptr_val, s.ptr() + FILE_TABLE_EXPR_PREFIX_LEN, sizeof(uintptr_t));
  ObFileTableDef *def = reinterpret_cast<ObFileTableDef *>(ptr_val);
  if (NULL == def || def->magic_ != ObFileTableDef::MAGIC_HEADER) {
    return false;
  }
  out_def = def;
  return true;
}

bool ObFileScanUtils::is_file_table_item(const TableItem &table_item, ObFileTableDef **out_def)
{
  if (NULL != out_def) {
    *out_def = NULL;
  }
  if (!table_item.is_function_table() || NULL == table_item.function_table_expr_) {
    return false;
  }
  ObFileTableDef *def = NULL;
  if (!is_file_table_expr(table_item.function_table_expr_, def)) {
    return false;
  }
  if (NULL != out_def) {
    *out_def = def;
  }
  return true;
}

bool ObFileScanUtils::extract_file_table_def_from_datum(const ObDatum &datum,
                                                        ObFileTableDef *&out_def)
{
  out_def = NULL;
  if (datum.is_null()) {
    return false;
  }
  const ObString s = datum.get_string();
  const int64_t expected_len = FILE_TABLE_EXPR_PREFIX_LEN + static_cast<int64_t>(sizeof(uintptr_t));
  if (s.length() != expected_len || NULL == s.ptr() ||
      0 != MEMCMP(s.ptr(), FILE_TABLE_EXPR_PREFIX, FILE_TABLE_EXPR_PREFIX_LEN)) {
    return false;
  }
  uintptr_t ptr_val = 0;
  MEMCPY(&ptr_val, s.ptr() + FILE_TABLE_EXPR_PREFIX_LEN, sizeof(uintptr_t));
  ObFileTableDef *def = reinterpret_cast<ObFileTableDef *>(ptr_val);
  if (NULL == def || def->magic_ != ObFileTableDef::MAGIC_HEADER) {
    return false;
  }
  out_def = def;
  return true;
}

int ObFileScanUtils::resolve_file_table_item(ObResolverParams &params,
                                             ObDMLResolver &resolver,
                                             const ParseNode &parse_tree,
                                             ObDMLStmt *stmt,
                                             TableItem *&tbl_item)
{
  int ret = OB_SUCCESS;
  tbl_item = NULL;
  if (OB_ISNULL(params.allocator_) || OB_ISNULL(params.expr_factory_) ||
      OB_ISNULL(params.session_info_) || OB_ISNULL(stmt)) {
    return OB_NOT_INIT;
  }
  if (OB_FAIL(check_file_sql_enabled(params.session_info_))) {
    return ret;
  }
  if (parse_tree.num_child_ < 1 || OB_ISNULL(parse_tree.children_[0])) {
    return OB_INVALID_ARGUMENT;
  }
  const ParseNode *func_node = parse_tree.children_[0];
  const ParseNode *alias_node = (parse_tree.num_child_ >= 2) ? parse_tree.children_[1] : NULL;

  void *def_mem = params.allocator_->alloc(sizeof(ObFileTableDef));
  if (OB_ISNULL(def_mem)) {
    return OB_ALLOCATE_MEMORY_FAILED;
  }
  ObFileTableDef *table_def = new (def_mem) ObFileTableDef();

  if (OB_FAIL(parse_file_table_node(*func_node, *params.allocator_, table_def->options_))) {
    return ret;
  }
  if (OB_FAIL(infer_file_table_schema(*params.allocator_, params.session_info_, *table_def))) {
    return ret;
  }

  ObRawExpr *encoded_expr = NULL;
  if (OB_FAIL(encode_file_table_expr(
          *params.allocator_, *params.expr_factory_, table_def, encoded_expr))) {
    return ret;
  }
  if (OB_FAIL(encoded_expr->deduce_type(params.session_info_))) {
    return ret;
  }

  TableItem *item = stmt->create_table_item(*params.allocator_);
  if (OB_ISNULL(item)) {
    return OB_ALLOCATE_MEMORY_FAILED;
  }
  ObString alias_name;
  if (NULL != alias_node && NULL != alias_node->str_value_ && alias_node->str_len_ > 0) {
    if (OB_FAIL(ob_write_string(
            *params.allocator_,
            ObString(static_cast<int32_t>(alias_node->str_len_), alias_node->str_value_),
            alias_name,
            true))) {
      return ret;
    }
    item->alias_name_ = alias_name;
  } else {
    if (OB_FAIL(stmt->generate_func_table_name(*params.allocator_, alias_name))) {
      return ret;
    }
  }
  item->table_name_ = alias_name;
  item->table_id_ = resolver.generate_table_id();
  item->type_ = TableItem::FUNCTION_TABLE;
  item->function_table_expr_ = encoded_expr;

  if (OB_FAIL(stmt->add_table_item(params.session_info_, item))) {
    return ret;
  }
  tbl_item = item;
  return OB_SUCCESS;
}

static bool select_stmt_uses_file_table(const ObSelectStmt *select_stmt)
{
  if (NULL == select_stmt) {
    return false;
  }
  for (int64_t i = 0; i < select_stmt->get_table_size(); ++i) {
    const TableItem *ti = select_stmt->get_table_item(i);
    if (NULL != ti) {
      if (ObFileScanUtils::is_file_table_item(*ti)) {
        return true;
      }
      if ((ti->is_generated_table() || ti->is_temp_table() || ti->is_lateral_table()) &&
          select_stmt_uses_file_table(ti->ref_query_)) {
        return true;
      }
    }
  }
  for (int64_t i = 0; i < select_stmt->get_set_query().count(); ++i) {
    if (select_stmt_uses_file_table(select_stmt->get_set_query().at(i))) {
      return true;
    }
  }
  return false;
}

static int append_sql_quoted_string(ObSqlString &sql, const ObString &str)
{
  int ret = sql.append("'");
  for (int64_t i = 0; OB_SUCC(ret) && i < str.length(); ++i) {
    const char c = str.ptr()[i];
    if (c == '\'' || c == '\\') {
      ret = sql.append_fmt("\\%c", c);
    } else {
      ret = sql.append_fmt("%c", c);
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("'");
  }
  return ret;
}

int ObFileScanUtils::resolve_describe_stmt(ObResolverParams &params,
                                           const ParseNode &parse_tree,
                                           ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  stmt = NULL;
  if (OB_ISNULL(params.allocator_) || OB_ISNULL(params.session_info_) ||
      OB_ISNULL(params.query_ctx_)) {
    return OB_NOT_INIT;
  }

  // Case 1: DESCRIBE read_csv('...') directly on a file table function
  if (NULL != parse_tree.str_value_ &&
      0 == strncmp(parse_tree.str_value_, "DESCRIBE_FILE_TABLE", parse_tree.str_len_)) {
    if (OB_FAIL(check_file_sql_enabled(params.session_info_))) {
      return ret;
    }
    if (parse_tree.num_child_ < 5 || OB_ISNULL(parse_tree.children_[4]) ||
        parse_tree.children_[4]->num_child_ < 1 ||
        OB_ISNULL(parse_tree.children_[4]->children_[0])) {
      return OB_INVALID_ARGUMENT;
    }
    ObFileTableDef table_def;
    if (OB_FAIL(parse_file_table_node(
            *parse_tree.children_[4]->children_[0], *params.allocator_, table_def.options_))) {
      return ret;
    }
    if (OB_FAIL(infer_file_table_schema(*params.allocator_, params.session_info_, table_def))) {
      return ret;
    }
    ObSqlString desc_sql;
    for (int64_t i = 0; OB_SUCC(ret) && i < table_def.columns_.count(); ++i) {
      const ObFileColumnDef &col = table_def.columns_.at(i);
      const char *type_str = logical_type_to_describe_name(col.logical_type_);
      const char *null_str = col.is_nullable_ ? "YES" : "NO";
      if (i > 0 && OB_FAIL(desc_sql.append(" UNION ALL "))) {
      } else if (OB_FAIL(desc_sql.append("SELECT "))) {
      } else if (OB_FAIL(append_sql_quoted_string(desc_sql, col.name_))) {
      } else if (OB_FAIL(desc_sql.append_fmt(
                     " AS `column_name`, '%s' AS `column_type`, '%s' AS `null`, "
                     "CAST(NULL AS CHAR) AS `key`, CAST(NULL AS CHAR) AS `default`, "
                     "CAST(NULL AS CHAR) AS `extra`",
                     type_str,
                     null_str))) {
      }
    }
    if (OB_SUCC(ret)) {
      ObParser parser(*params.allocator_,
                      params.session_info_->get_sql_mode(),
                      params.session_info_->get_charsets4parser());
      ParseResult parse_result;
      if (OB_FAIL(parser.parse(desc_sql.string(), parse_result))) {
      } else if (OB_ISNULL(parse_result.result_tree_) ||
                 parse_result.result_tree_->num_child_ < 1 ||
                 OB_ISNULL(parse_result.result_tree_->children_[0])) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        params.session_info_->set_stmt_type(stmt::T_SELECT);
        if (NULL != params.session_info_->get_cur_exec_ctx() &&
            NULL != params.session_info_->get_cur_exec_ctx()->get_sql_ctx()) {
          params.session_info_->get_cur_exec_ctx()->get_sql_ctx()->stmt_type_ = stmt::T_SELECT;
        }
        HEAP_VAR(ObSelectResolver, select_resolver, params) {
          select_resolver.set_calc_found_rows(true);
          select_resolver.set_has_top_limit(true);
          if (OB_FAIL(SMART_CALL(select_resolver.resolve(*parse_result.result_tree_->children_[0])))) {
          } else {
            stmt = select_resolver.get_basic_stmt();
          }
        }
      }
    }
    return ret;
  }

  // Case 2: DESCRIBE SELECT ...
  if (parse_tree.num_child_ < 5 || OB_ISNULL(parse_tree.children_[4]) ||
      parse_tree.children_[4]->type_ != T_SELECT) {
    return OB_ERR_UNEXPECTED;
  }
  const ParseNode &select_node = *parse_tree.children_[4];
  ObSelectStmt *resolved_select = NULL;
  {
    HEAP_VAR(ObSelectResolver, target_resolver, params) {
      target_resolver.set_calc_found_rows(true);
      target_resolver.set_has_top_limit(true);
      if (OB_FAIL(SMART_CALL(target_resolver.resolve(select_node)))) {
        return ret;
      }
      resolved_select = static_cast<ObSelectStmt *>(target_resolver.get_basic_stmt());
    }
  }
  if (OB_ISNULL(resolved_select)) {
    return OB_ERR_UNEXPECTED;
  }
  if (!select_stmt_uses_file_table(resolved_select) &&
      !is_file_sql_enabled(params.session_info_, false)) {
    // Fall back to standard EXPLAIN when enable_file_sql is OFF and query does not use file tables
    stmt = NULL;
    return OB_SUCCESS;
  }

  ObSqlString desc_sql;
  const int64_t item_cnt = resolved_select->get_select_item_size();
  if (item_cnt <= 0) {
    return OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < item_cnt; ++i) {
    const SelectItem &si = resolved_select->get_select_item(i);
    const ObString col_name = si.alias_name_.empty() ? si.expr_name_ : si.alias_name_;
    const char *type_str = "VARCHAR";
    bool is_nullable = true;
    if (OB_NOT_NULL(si.expr_)) {
      type_str = obj_meta_to_describe_name(si.expr_->get_result_meta(), si.expr_->get_accuracy());
      if (si.expr_->is_column_ref_expr()) {
        const ObColumnRefRawExpr *col_ref = static_cast<const ObColumnRefRawExpr *>(si.expr_);
        const TableItem *ti = resolved_select->get_table_item_by_id(col_ref->get_table_id());
        ObFileTableDef *fdef = NULL;
        if (NULL != ti && is_file_table_item(*ti, &fdef) && NULL != fdef) {
          const int64_t cidx = static_cast<int64_t>(col_ref->get_column_id()) - OB_APP_MIN_COLUMN_ID;
          if (cidx >= 0 && cidx < fdef->columns_.count()) {
            type_str = logical_type_to_describe_name(fdef->columns_.at(cidx).logical_type_);
            is_nullable = fdef->columns_.at(cidx).is_nullable_;
          }
        } else {
          is_nullable = !si.expr_->get_result_type().has_result_flag(NOT_NULL_FLAG);
        }
      } else {
        is_nullable = !si.expr_->get_result_type().has_result_flag(NOT_NULL_FLAG);
      }
    }
    const char *null_str = is_nullable ? "YES" : "NO";
    if (i > 0 && OB_FAIL(desc_sql.append(" UNION ALL "))) {
    } else if (OB_FAIL(desc_sql.append("SELECT "))) {
    } else if (OB_FAIL(append_sql_quoted_string(desc_sql, col_name))) {
    } else if (OB_FAIL(desc_sql.append_fmt(
                   " AS `column_name`, '%s' AS `column_type`, '%s' AS `null`, "
                   "CAST(NULL AS CHAR) AS `key`, CAST(NULL AS CHAR) AS `default`, "
                   "CAST(NULL AS CHAR) AS `extra`",
                   type_str,
                   null_str))) {
    }
  }

  if (OB_SUCC(ret)) {
    ObParser parser(*params.allocator_,
                    params.session_info_->get_sql_mode(),
                    params.session_info_->get_charsets4parser());
    ParseResult parse_result;
    if (OB_FAIL(parser.parse(desc_sql.string(), parse_result))) {
    } else if (OB_ISNULL(parse_result.result_tree_) ||
               parse_result.result_tree_->num_child_ < 1 ||
               OB_ISNULL(parse_result.result_tree_->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      params.session_info_->set_stmt_type(stmt::T_SELECT);
      if (NULL != params.session_info_->get_cur_exec_ctx() &&
          NULL != params.session_info_->get_cur_exec_ctx()->get_sql_ctx()) {
        params.session_info_->get_cur_exec_ctx()->get_sql_ctx()->stmt_type_ = stmt::T_SELECT;
      }
      HEAP_VAR(ObSelectResolver, select_resolver, params) {
        select_resolver.set_calc_found_rows(true);
        select_resolver.set_has_top_limit(true);
        if (OB_FAIL(SMART_CALL(select_resolver.resolve(*parse_result.result_tree_->children_[0])))) {
        } else {
          stmt = select_resolver.get_basic_stmt();
        }
      }
    }
  }
  return ret;
}

int ObFileScanUtils::resolve_copy_stmt(ObResolverParams &params,
                                       const ParseNode &parse_tree,
                                       ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  stmt = NULL;
  if (OB_ISNULL(params.allocator_) || OB_ISNULL(params.session_info_) ||
      OB_ISNULL(params.query_ctx_)) {
    return OB_NOT_INIT;
  }
  if (OB_FAIL(check_file_sql_enabled(params.session_info_))) {
    return ret;
  }
  if (parse_tree.num_child_ < 3 ||
      OB_ISNULL(parse_tree.children_[0]) ||
      OB_ISNULL(parse_tree.children_[1])) {
    return OB_INVALID_ARGUMENT;
  }

  const ParseNode *source_node = parse_tree.children_[0];
  const ParseNode *target_node = parse_tree.children_[1];
  const ParseNode *opt_root = parse_tree.children_[2];

  ObString raw_target_path;
  if (!extract_string_from_parse_node(target_node, false, raw_target_path) ||
      raw_target_path.empty()) {
    return OB_INVALID_ARGUMENT;
  }

  void *copy_mem = params.allocator_->alloc(sizeof(ObCopyCsvOptions));
  if (OB_ISNULL(copy_mem)) {
    return OB_ALLOCATE_MEMORY_FAILED;
  }
  ObCopyCsvOptions *copy_opts = new (copy_mem) ObCopyCsvOptions();

  ObString secure_file_priv;
  if (OB_FAIL(get_effective_secure_file_priv(
          params.session_info_, *params.allocator_, secure_file_priv))) {
    return ret;
  }
  if (OB_FAIL(validate_secure_write_path(
          secure_file_priv, raw_target_path, *params.allocator_, copy_opts->target_path_))) {
    return ret;
  }

  // Parse COPY options
  ObSEArray<const ParseNode *, 8> opt_nodes;
  collect_option_nodes(opt_root, opt_nodes);
  ObSEArray<ObString, 8> seen_keys;
  for (int64_t i = 0; OB_SUCC(ret) && i < opt_nodes.count(); ++i) {
    const ParseNode *kv = opt_nodes.at(i);
    if (OB_ISNULL(kv) || kv->num_child_ < 1 || OB_ISNULL(kv->children_[0])) {
      ret = OB_INVALID_ARGUMENT;
      break;
    }
    const ParseNode *key_node = kv->children_[0];
    const ParseNode *val_node = (kv->num_child_ >= 2) ? kv->children_[1] : NULL;
    ObString key;
    if (!extract_string_from_parse_node(key_node, true, key)) {
      ret = OB_INVALID_ARGUMENT;
      break;
    }
    key = key.trim();
    if (key.empty()) {
      ret = OB_INVALID_ARGUMENT;
      break;
    }
    ObString norm_key = key;
    if (0 == key.case_compare("delim")) {
      norm_key = ObString::make_string("delimiter");
    } else if (0 == key.case_compare("null")) {
      norm_key = ObString::make_string("nullstr");
    }
    for (int64_t j = 0; j < seen_keys.count(); ++j) {
      if (ObCharset::case_insensitive_equal(seen_keys.at(j), norm_key)) {
        ret = OB_INVALID_ARGUMENT;
        break;
      }
    }
    if (OB_FAIL(ret) || OB_FAIL(seen_keys.push_back(norm_key))) {
      break;
    }

    if (0 == norm_key.case_compare("format")) {
      ObString fmt_str;
      if (NULL == val_node || !extract_string_from_parse_node(val_node, true, fmt_str)) {
        ret = OB_INVALID_ARGUMENT;
      } else if (0 != fmt_str.case_compare("csv")) {
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY format other than CSV");
        ret = OB_NOT_SUPPORTED;
      }
    } else if (0 == norm_key.case_compare("header")) {
      if (NULL == val_node) {
        copy_opts->header_ = true;
      } else if (!extract_bool_from_parse_node(val_node, copy_opts->header_)) {
        ret = OB_INVALID_ARGUMENT;
      }
    } else if (0 == norm_key.case_compare("delimiter")) {
      ObString s;
      if (NULL == val_node || !extract_string_from_parse_node(val_node, false, s) ||
          s.length() != 1 || static_cast<uint8_t>(s.ptr()[0]) >= 0x80 ||
          s.ptr()[0] == '\0' || s.ptr()[0] == '\n' || s.ptr()[0] == '\r') {
        ret = OB_INVALID_ARGUMENT;
      } else {
        copy_opts->delimiter_ = s.ptr()[0];
      }
    } else if (0 == norm_key.case_compare("quote")) {
      ObString s;
      if (NULL == val_node || !extract_string_from_parse_node(val_node, false, s) ||
          s.length() != 1 || static_cast<uint8_t>(s.ptr()[0]) >= 0x80 ||
          s.ptr()[0] == '\0' || s.ptr()[0] == '\n' || s.ptr()[0] == '\r') {
        ret = OB_INVALID_ARGUMENT;
      } else {
        copy_opts->quote_ = s.ptr()[0];
      }
    } else if (0 == norm_key.case_compare("escape")) {
      ObString s;
      if (NULL == val_node || !extract_string_from_parse_node(val_node, false, s) ||
          s.length() != 1 || static_cast<uint8_t>(s.ptr()[0]) >= 0x80 ||
          s.ptr()[0] == '\0' || s.ptr()[0] == '\n' || s.ptr()[0] == '\r') {
        ret = OB_INVALID_ARGUMENT;
      } else {
        copy_opts->escape_ = s.ptr()[0];
      }
    } else if (0 == norm_key.case_compare("nullstr")) {
      ObString s;
      if (NULL == val_node || !extract_string_from_parse_node(val_node, false, s)) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        ret = ob_write_string(*params.allocator_, s, copy_opts->nullstr_, true);
      }
    } else {
      ret = OB_NOT_SUPPORTED;
    }
  }
  if (OB_FAIL(ret)) {
    return ret;
  }
  if (copy_opts->delimiter_ == copy_opts->quote_) {
    return OB_INVALID_ARGUMENT;
  }

  // Resolve source query (either T_SELECT or table relation_factor)
  const ParseNode *select_tree = source_node;
  ParseResult synth_result;
  if (source_node->type_ != T_SELECT) {
    // COPY table_name TO '...' -> synthesize SELECT * FROM table_name
    ObSqlString synth_sql;
    if (source_node->type_ == T_RELATION_FACTOR && source_node->num_child_ >= 2 &&
        NULL != source_node->children_[1]) {
      if (NULL != source_node->children_[0] && NULL != source_node->children_[0]->str_value_) {
        if (OB_FAIL(synth_sql.append_fmt("SELECT * FROM `%.*s`.`%.*s`",
                                         static_cast<int>(source_node->children_[0]->str_len_),
                                         source_node->children_[0]->str_value_,
                                         static_cast<int>(source_node->children_[1]->str_len_),
                                         source_node->children_[1]->str_value_))) {
          return ret;
        }
      } else if (NULL != source_node->children_[1]->str_value_) {
        if (OB_FAIL(synth_sql.append_fmt("SELECT * FROM `%.*s`",
                                         static_cast<int>(source_node->children_[1]->str_len_),
                                         source_node->children_[1]->str_value_))) {
          return ret;
        }
      } else {
        return OB_INVALID_ARGUMENT;
      }
    } else {
      return OB_INVALID_ARGUMENT;
    }
    ObParser parser(*params.allocator_,
                    params.session_info_->get_sql_mode(),
                    params.session_info_->get_charsets4parser());
    if (OB_FAIL(parser.parse(synth_sql.string(), synth_result))) {
      return ret;
    }
    if (OB_ISNULL(synth_result.result_tree_) || synth_result.result_tree_->num_child_ < 1 ||
        OB_ISNULL(synth_result.result_tree_->children_[0])) {
      return OB_ERR_UNEXPECTED;
    }
    select_tree = synth_result.result_tree_->children_[0];
  }

  params.session_info_->set_stmt_type(stmt::T_SELECT);
  if (NULL != params.session_info_->get_cur_exec_ctx() &&
      NULL != params.session_info_->get_cur_exec_ctx()->get_sql_ctx()) {
    params.session_info_->get_cur_exec_ctx()->get_sql_ctx()->stmt_type_ = stmt::T_SELECT;
  }

  ObSelectStmt *select_stmt = NULL;
  {
    HEAP_VAR(ObSelectResolver, select_resolver, params) {
      select_resolver.set_calc_found_rows(true);
      select_resolver.set_has_top_limit(true);
      if (OB_FAIL(SMART_CALL(select_resolver.resolve(*select_tree)))) {
        return ret;
      }
      select_stmt = static_cast<ObSelectStmt *>(select_resolver.get_basic_stmt());
    }
  }
  if (OB_ISNULL(select_stmt)) {
    return OB_ERR_UNEXPECTED;
  }
  if (NULL != select_stmt->get_select_into()) {
    return OB_NOT_SUPPORTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt->get_select_item_size(); ++i) {
    const SelectItem &si = select_stmt->get_select_item(i);
    ObString col_name = si.alias_name_.empty() ? si.expr_name_ : si.alias_name_;
    ObString copied_name;
    bool is_bool = false;
    if (OB_NOT_NULL(si.expr_) && si.expr_->get_result_meta().get_type() == ObTinyIntType &&
        si.expr_->get_accuracy().get_length() == 1) {
      is_bool = true;
    }
    if (OB_FAIL(ob_write_string(*params.allocator_, col_name, copied_name, true))) {
    } else if (OB_FAIL(copy_opts->header_names_.push_back(copied_name))) {
    } else if (OB_FAIL(copy_opts->is_bool_cols_.push_back(is_bool))) {
    }
  }
  if (OB_FAIL(ret)) {
    return ret;
  }

  void *into_mem = params.allocator_->alloc(sizeof(ObSelectIntoItem));
  if (OB_ISNULL(into_mem)) {
    return OB_ALLOCATE_MEMORY_FAILED;
  }
  ObSelectIntoItem *into_item = new (into_mem) ObSelectIntoItem();
  into_item->into_type_ = T_INTO_OUTFILE;
  into_item->is_single_ = true;

  const int64_t spec_len = COPY_CSV_SPEC_PREFIX_LEN + static_cast<int64_t>(sizeof(uintptr_t));
  char *spec_buf = static_cast<char *>(params.allocator_->alloc(spec_len));
  if (OB_ISNULL(spec_buf)) {
    return OB_ALLOCATE_MEMORY_FAILED;
  }
  MEMCPY(spec_buf, COPY_CSV_SPEC_PREFIX, COPY_CSV_SPEC_PREFIX_LEN);
  const uintptr_t ptr_val = reinterpret_cast<uintptr_t>(copy_opts);
  MEMCPY(spec_buf + COPY_CSV_SPEC_PREFIX_LEN, &ptr_val, sizeof(uintptr_t));

  into_item->outfile_name_.set_varchar(ObString(static_cast<int32_t>(spec_len), spec_buf));
  into_item->outfile_name_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  into_item->outfile_name_.set_collation_level(CS_LEVEL_COERCIBLE);
  select_stmt->set_select_into(into_item);

  stmt = select_stmt;
  return OB_SUCCESS;
}

int ObFileScanUtils::create_file_scanner(ObIAllocator &allocator,
                                         ObExecContext &exec_ctx,
                                         const ObFileTableDef &table_def,
                                         ObIFileScanner *&scanner)
{
  scanner = NULL;
  void *mem = NULL;
  switch (table_def.options_.format_) {
    case OB_FILE_FORMAT_CSV:
      if (OB_ISNULL(mem = allocator.alloc(sizeof(ObCsvFileScanner)))) {
        return OB_ALLOCATE_MEMORY_FAILED;
      }
      scanner = new (mem) ObCsvFileScanner(table_def, exec_ctx);
      return OB_SUCCESS;
    case OB_FILE_FORMAT_JSONL:
      if (OB_ISNULL(mem = allocator.alloc(sizeof(ObJsonlFileScanner)))) {
        return OB_ALLOCATE_MEMORY_FAILED;
      }
      scanner = new (mem) ObJsonlFileScanner(table_def, exec_ctx);
      return OB_SUCCESS;
    case OB_FILE_FORMAT_PARQUET:
      if (OB_ISNULL(mem = allocator.alloc(sizeof(ObParquetFileScanner)))) {
        return OB_ALLOCATE_MEMORY_FAILED;
      }
      scanner = new (mem) ObParquetFileScanner(table_def, exec_ctx);
      return OB_SUCCESS;
    case OB_FILE_FORMAT_GLOB:
      if (OB_ISNULL(mem = allocator.alloc(sizeof(ObGlobFileScanner)))) {
        return OB_ALLOCATE_MEMORY_FAILED;
      }
      scanner = new (mem) ObGlobFileScanner(table_def, exec_ctx);
      return OB_SUCCESS;
    default:
      return OB_NOT_SUPPORTED;
  }
}

bool ObFileScanUtils::is_copy_csv_spec(const ObObj &outfile_name)
{
  if (!outfile_name.is_varchar_or_char()) {
    return false;
  }
  const ObString s = outfile_name.get_varchar();
  const int64_t expected_len = COPY_CSV_SPEC_PREFIX_LEN + static_cast<int64_t>(sizeof(uintptr_t));
  return (s.length() == expected_len && NULL != s.ptr() &&
          0 == MEMCMP(s.ptr(), COPY_CSV_SPEC_PREFIX, COPY_CSV_SPEC_PREFIX_LEN));
}

static void append_csv_escaped_field(std::string &out,
                                     const char *data,
                                     size_t len,
                                     const ObCopyCsvOptions &opts)
{
  bool needs_quote = false;
  if (len == 0 && opts.nullstr_.empty()) {
    needs_quote = true;
  } else if (!opts.nullstr_.empty() &&
             len == static_cast<size_t>(opts.nullstr_.length()) &&
             0 == MEMCMP(data, opts.nullstr_.ptr(), len)) {
    needs_quote = true;
  }
  for (size_t i = 0; !needs_quote && i < len; ++i) {
    const char c = data[i];
    if (c == opts.delimiter_ || c == opts.quote_ || c == opts.escape_ ||
        c == '\n' || c == '\r') {
      needs_quote = true;
    }
  }
  if (!needs_quote) {
    out.append(data, len);
    return;
  }
  out.push_back(opts.quote_);
  for (size_t i = 0; i < len; ++i) {
    const char c = data[i];
    if (c == opts.quote_) {
      out.push_back(opts.escape_);
      out.push_back(opts.quote_);
    } else if (opts.escape_ != opts.quote_ && c == opts.escape_) {
      out.push_back(opts.escape_);
      out.push_back(opts.escape_);
    } else {
      out.push_back(c);
    }
  }
  out.push_back(opts.quote_);
}

static int format_expr_datum_for_csv(const ObExpr *expr,
                                     const ObDatum &datum,
                                     bool is_bool_col,
                                     const ObTimeZoneInfo *tz_info,
                                     const ObCopyCsvOptions &opts,
                                     std::string &line_buf)
{
  int ret = OB_SUCCESS;
  if (datum.is_null()) {
    if (!opts.nullstr_.empty()) {
      line_buf.append(opts.nullstr_.ptr(), static_cast<size_t>(opts.nullstr_.length()));
    }
    return OB_SUCCESS;
  }
  char num_buf[128];
  const ObObjType type = expr->datum_meta_.type_;
  switch (type) {
    case ObTinyIntType: {
      if (is_bool_col || expr->datum_meta_.precision_ == 1) {
        const char *bs = (datum.get_tinyint() != 0) ? "true" : "false";
        append_csv_escaped_field(line_buf, bs, strlen(bs), opts);
      } else {
        const int n = snprintf(num_buf, sizeof(num_buf), "%d", static_cast<int>(datum.get_tinyint()));
        append_csv_escaped_field(line_buf, num_buf, static_cast<size_t>(n), opts);
      }
      break;
    }
    case ObSmallIntType: {
      const int n = snprintf(num_buf, sizeof(num_buf), "%d", static_cast<int>(datum.get_smallint()));
      append_csv_escaped_field(line_buf, num_buf, static_cast<size_t>(n), opts);
      break;
    }
    case ObMediumIntType:
    case ObInt32Type: {
      const int n = snprintf(num_buf, sizeof(num_buf), "%d", datum.get_int32());
      append_csv_escaped_field(line_buf, num_buf, static_cast<size_t>(n), opts);
      break;
    }
    case ObIntType: {
      const int n = snprintf(num_buf, sizeof(num_buf), "%ld", static_cast<long>(datum.get_int()));
      append_csv_escaped_field(line_buf, num_buf, static_cast<size_t>(n), opts);
      break;
    }
    case ObUTinyIntType:
    case ObUSmallIntType:
    case ObUMediumIntType:
    case ObUInt32Type:
    case ObUInt64Type: {
      const int n = snprintf(num_buf, sizeof(num_buf), "%lu", static_cast<unsigned long>(datum.get_uint64()));
      append_csv_escaped_field(line_buf, num_buf, static_cast<size_t>(n), opts);
      break;
    }
    case ObFloatType:
    case ObUFloatType: {
      const int n = snprintf(num_buf, sizeof(num_buf), "%.7g", static_cast<double>(datum.get_float()));
      append_csv_escaped_field(line_buf, num_buf, static_cast<size_t>(n), opts);
      break;
    }
    case ObDoubleType:
    case ObUDoubleType: {
      const int n = snprintf(num_buf, sizeof(num_buf), "%.15g", datum.get_double());
      append_csv_escaped_field(line_buf, num_buf, static_cast<size_t>(n), opts);
      break;
    }
    case ObNumberType:
    case ObUNumberType: {
      number::ObNumber nmb(datum.get_number());
      int64_t pos = 0;
      if (OB_FAIL(nmb.format(num_buf, sizeof(num_buf), pos, -1))) {
        return ret;
      }
      append_csv_escaped_field(line_buf, num_buf, static_cast<size_t>(pos), opts);
      break;
    }
    case ObDateType: {
      int64_t pos = 0;
      if (OB_FAIL(ObTimeConverter::date_to_str(datum.get_date(), num_buf, sizeof(num_buf), pos))) {
        return ret;
      }
      append_csv_escaped_field(line_buf, num_buf, static_cast<size_t>(pos), opts);
      break;
    }
    case ObDateTimeType:
    case ObTimestampType: {
      int64_t pos = 0;
      const int16_t scale = expr->datum_meta_.scale_ >= 0 ? expr->datum_meta_.scale_ : 0;
      if (OB_FAIL(ObTimeConverter::datetime_to_str(
              datum.get_datetime(),
              (type == ObTimestampType) ? tz_info : NULL,
              scale,
              num_buf,
              sizeof(num_buf),
              pos))) {
        return ret;
      }
      append_csv_escaped_field(line_buf, num_buf, static_cast<size_t>(pos), opts);
      break;
    }
    default: {
      const ObString s = datum.get_string();
      append_csv_escaped_field(line_buf, s.ptr(), static_cast<size_t>(s.length()), opts);
      break;
    }
  }
  return OB_SUCCESS;
}

static int write_all_bytes(int fd, const char *data, size_t len)
{
  size_t written = 0;
  while (written < len) {
    const ssize_t n = ::write(fd, data + written, len - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return OB_IO_ERROR;
    }
    written += static_cast<size_t>(n);
  }
  return OB_SUCCESS;
}

int ObFileScanUtils::execute_copy_to_csv(ObExecContext &ctx,
                                         ObEvalCtx &eval_ctx,
                                         ObOperator *child,
                                         const ObIArray<ObExpr *> &select_exprs,
                                         const ObString &encoded_spec,
                                         int64_t top_limit_cnt,
                                         ObBatchRows &brs,
                                         bool is_batch)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(child) || OB_ISNULL(ctx.get_physical_plan_ctx())) {
    return OB_ERR_UNEXPECTED;
  }
  if (OB_FAIL(check_file_sql_enabled(ctx.get_my_session()))) {
    return ret;
  }

  const int64_t expected_len = COPY_CSV_SPEC_PREFIX_LEN + static_cast<int64_t>(sizeof(uintptr_t));
  if (encoded_spec.length() != expected_len || NULL == encoded_spec.ptr() ||
      0 != MEMCMP(encoded_spec.ptr(), COPY_CSV_SPEC_PREFIX, COPY_CSV_SPEC_PREFIX_LEN)) {
    return OB_ERR_UNEXPECTED;
  }
  uintptr_t ptr_val = 0;
  MEMCPY(&ptr_val, encoded_spec.ptr() + COPY_CSV_SPEC_PREFIX_LEN, sizeof(uintptr_t));
  const ObCopyCsvOptions *opts = reinterpret_cast<const ObCopyCsvOptions *>(ptr_val);
  if (OB_ISNULL(opts)) {
    return OB_ERR_UNEXPECTED;
  }

  ObString secure_file_priv;
  ObString resolved_target;
  if (OB_FAIL(get_effective_secure_file_priv(
          ctx.get_my_session(), ctx.get_allocator(), secure_file_priv))) {
    return ret;
  }
  if (OB_FAIL(validate_secure_write_path(
          secure_file_priv, opts->target_path_, ctx.get_allocator(), resolved_target))) {
    return ret;
  }

  char target_buf[PATH_MAX + 1] = {0};
  MEMCPY(target_buf, resolved_target.ptr(), resolved_target.length());

  char tmp_buf[PATH_MAX + 64] = {0};
  static std::atomic<uint64_t> tmp_seq(1);
  snprintf(tmp_buf,
           sizeof(tmp_buf),
           "%s.tmp.%d.%lu",
           target_buf,
           static_cast<int>(::getpid()),
           static_cast<unsigned long>(tmp_seq.fetch_add(1, std::memory_order_relaxed)));

  const int fd = ::open(tmp_buf, O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    return (errno == EACCES || errno == EPERM) ? OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS : OB_IO_ERROR;
  }

  std::string write_buf;
  write_buf.reserve(65536);

  if (opts->header_) {
    for (int64_t i = 0; i < opts->header_names_.count(); ++i) {
      if (i > 0) {
        write_buf.push_back(opts->delimiter_);
      }
      const ObString &hname = opts->header_names_.at(i);
      append_csv_escaped_field(
          write_buf, hname.ptr(), static_cast<size_t>(hname.length()), *opts);
    }
    write_buf.push_back('\n');
  }

  const ObTimeZoneInfo *tz_info =
      (NULL != ctx.get_my_session()) ? ctx.get_my_session()->get_timezone_info() : NULL;
  int64_t row_count = 0;

  if (!is_batch) {
    while (OB_SUCC(ret) && row_count < top_limit_cnt) {
      child->clear_evaluated_flag();
      ret = child->get_next_row();
      if (ret == OB_ITER_END) {
        ret = OB_SUCCESS;
        break;
      } else if (OB_FAIL(ret)) {
        break;
      }
      ++row_count;
      for (int64_t c = 0; OB_SUCC(ret) && c < select_exprs.count(); ++c) {
        if (c > 0) {
          write_buf.push_back(opts->delimiter_);
        }
        ObExpr *expr = select_exprs.at(c);
        ObDatum *datum = NULL;
        if (OB_ISNULL(expr) || OB_FAIL(expr->eval(eval_ctx, datum)) || OB_ISNULL(datum)) {
          ret = OB_FAIL(ret) ? ret : OB_ERR_UNEXPECTED;
          break;
        }
        const bool is_bool = (c < opts->is_bool_cols_.count()) ? opts->is_bool_cols_.at(c) : false;
        ret = format_expr_datum_for_csv(expr, *datum, is_bool, tz_info, *opts, write_buf);
      }
      if (OB_SUCC(ret)) {
        write_buf.push_back('\n');
        if (write_buf.size() >= 65536) {
          ret = write_all_bytes(fd, write_buf.data(), write_buf.size());
          write_buf.clear();
        }
      }
    }
  } else {
    const int64_t max_batch = 256;
    while (OB_SUCC(ret) && row_count < top_limit_cnt) {
      child->clear_evaluated_flag();
      const ObBatchRows *child_brs = NULL;
      const int64_t req_cnt = std::min<int64_t>(max_batch, top_limit_cnt - row_count);
      if (OB_FAIL(child->get_next_batch(req_cnt, child_brs)) || OB_ISNULL(child_brs)) {
        ret = OB_FAIL(ret) ? ret : OB_ERR_UNEXPECTED;
        break;
      }
      if (child_brs->size_ > 0) {
        ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx);
        guard.set_batch_size(child_brs->size_);
        for (int64_t b = 0; OB_SUCC(ret) && b < child_brs->size_; ++b) {
          if (child_brs->skip_->contain(b)) {
            continue;
          }
          guard.set_batch_idx(b);
          ++row_count;
          for (int64_t c = 0; OB_SUCC(ret) && c < select_exprs.count(); ++c) {
            if (c > 0) {
              write_buf.push_back(opts->delimiter_);
            }
            ObExpr *expr = select_exprs.at(c);
            ObDatum *datum = NULL;
            if (OB_ISNULL(expr) || OB_FAIL(expr->eval(eval_ctx, datum)) || OB_ISNULL(datum)) {
              ret = OB_FAIL(ret) ? ret : OB_ERR_UNEXPECTED;
              break;
            }
            const bool is_bool = (c < opts->is_bool_cols_.count()) ? opts->is_bool_cols_.at(c) : false;
            ret = format_expr_datum_for_csv(expr, *datum, is_bool, tz_info, *opts, write_buf);
          }
          if (OB_SUCC(ret)) {
            write_buf.push_back('\n');
            if (write_buf.size() >= 65536) {
              ret = write_all_bytes(fd, write_buf.data(), write_buf.size());
              write_buf.clear();
            }
          }
        }
      }
      if (child_brs->end_ || row_count >= top_limit_cnt) {
        break;
      }
    }
  }

  if (OB_SUCC(ret) && !write_buf.empty()) {
    ret = write_all_bytes(fd, write_buf.data(), write_buf.size());
    write_buf.clear();
  }
  if (OB_SUCC(ret)) {
    if (0 != ::fsync(fd)) {
      ret = OB_IO_ERROR;
    }
  }
  ::close(fd);

  if (OB_SUCC(ret)) {
    struct stat exist_st;
    if (0 == ::lstat(target_buf, &exist_st)) {
      ::unlink(tmp_buf);
      LOG_USER_ERROR(OB_FILE_ALREADY_EXIST, target_buf);
      return OB_FILE_ALREADY_EXIST;
    }
    if (0 != ::rename(tmp_buf, target_buf)) {
      ::unlink(tmp_buf);
      return OB_IO_ERROR;
    }
    ctx.get_physical_plan_ctx()->set_affected_rows(row_count);
    if (is_batch) {
      brs.size_ = 0;
      brs.end_ = true;
      return OB_SUCCESS;
    }
    return OB_ITER_END;
  } else {
    ::unlink(tmp_buf);
    return ret;
  }
}

} // namespace sql
} // namespace oceanbase
