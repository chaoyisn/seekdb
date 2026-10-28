#include "observer/ai_column_worker.h"
#include "share/schema/ob_column_schema.h"
#include "common/json_type/ob_json_parse.h"
#include "share/json/ob_json_helper.h"
#include "query/vector/ob_vector_index_util.h"
#include "lib/charset/ob_charset.h"
#include <charconv>
#include <cmath>
#include <limits>

namespace oceanbase {
namespace observer {
using namespace common;
using namespace share::schema;

int AiColumnWorker::validate_result(const ObColumnSchemaV2 &column, bool is_null,
                                    ObString value, ObIAllocator &allocator)
{
  if (is_null) { return OB_SUCCESS; }
  const char *first = value.ptr(), *last = first + value.length();
  if (column.is_json()) {
    return ObJsonParser::check_json_syntax(value, &allocator);
  }
  if (column.is_collection()) {
    int ret = OB_SUCCESS;
    share::ObJsonReaderHelper reader(allocator);
    ObJsonNode *root = nullptr;
    int64_t dimension = 0;
    OZ(reader.parse(value.ptr(), value.length(), root));
    OZ(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
        column.get_extended_type_info(), dimension));
    if (OB_SUCC(ret) && (root == nullptr || !share::ObJsonHelper::is_array_type(root)
        || reader.get_array_size(root) != dimension)) { ret = OB_ERR_INVALID_VECTOR_DIM; }
    for (int64_t i = 0; OB_SUCC(ret) && i < dimension; ++i) {
      ObIJsonBase *item = nullptr;
      float number = 0;
      OZ(reader.get_array_element(root, i, item));
      if (OB_SUCC(ret) && !share::ObJsonHelper::is_number_type(item)) { ret = OB_INVALID_ARGUMENT; }
      OZ(reader.get_float_value(item, number));
      if (OB_SUCC(ret) && !std::isfinite(number)) { ret = OB_INVALID_ARGUMENT; }
    }
    return ret;
  }
  if (column.is_string_type() || ob_is_text_tc(column.get_data_type())) {
    int64_t limit = column.get_data_length();
    if (column.get_data_type() == ObTinyTextType) { limit = 255; }
    if (column.get_data_type() == ObTextType) { limit = 65535; }
    if (column.get_data_type() == ObMediumTextType) { limit = 16777215; }
    if (column.get_data_type() == ObLongTextType) { limit = 4294967295LL; }
    int ret = OB_SUCCESS;
    int64_t valid_bytes = 0;
    OZ(ObCharset::well_formed_len(CS_TYPE_UTF8MB4_BIN, value.ptr(), value.length(), valid_bytes));
    if (OB_SUCC(ret) && valid_bytes != value.length()) { ret = OB_INVALID_ARGUMENT; }
    const ObCollationType target = column.get_collation_type();
    int64_t max_bytes = 0;
    OZ(ObCharset::get_mbmaxlen_by_coll(target, max_bytes));
    const int64_t capacity = MAX(1L, value.length() * max_bytes);
    if (OB_SUCC(ret) && capacity > UINT32_MAX) { ret = OB_SIZE_OVERFLOW; }
    char *converted = OB_SUCC(ret) ? static_cast<char *>(allocator.alloc(capacity)) : nullptr;
    if (OB_SUCC(ret) && converted == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    uint32_t converted_bytes = 0;
    OZ(ObCharset::charset_convert(CS_TYPE_UTF8MB4_BIN, value.ptr(), value.length(), target,
                                 converted, capacity, converted_bytes, false, true));
    const int64_t length = OB_FAIL(ret) ? 0 : ob_is_text_tc(column.get_data_type())
        || target == CS_TYPE_BINARY ? converted_bytes
        : ObCharset::strlen_char(target, converted, converted_bytes);
    if (OB_SUCC(ret) && length > limit) { ret = OB_ERR_DATA_TOO_LONG; }
    return ret;
  }
  while (first < last && (*first == ' ' || *first == '\n' || *first == '\r' || *first == '\t')) { ++first; }
  while (last > first && (last[-1] == ' ' || last[-1] == '\n' || last[-1] == '\r' || last[-1] == '\t')) { --last; }
  if (first == last) { return OB_INVALID_ARGUMENT; }
  const ObObjType type = column.get_data_type();
  if (type >= ObTinyIntType && type <= ObIntType) {
    int64_t number = 0;
    const auto parsed = std::from_chars(first, last, number);
    const int bits = type == ObIntType ? 64 : static_cast<int>(type) * 8;
    if (parsed.ec != std::errc() || parsed.ptr != last) { return OB_INVALID_ARGUMENT; }
    if (bits < 64 && (number < -(1LL << (bits-1)) || number >= (1LL << (bits-1)))) { return OB_DATA_OUT_OF_RANGE; }
    return OB_SUCCESS;
  }
  if (type >= ObUTinyIntType && type <= ObUInt64Type) {
    uint64_t number = 0;
    const auto parsed = std::from_chars(first, last, number);
    const int bits = type == ObUInt64Type ? 64 : (static_cast<int>(type)-5) * 8;
    if (parsed.ec != std::errc() || parsed.ptr != last) { return OB_INVALID_ARGUMENT; }
    return bits < 64 && number >= (1ULL << bits) ? OB_DATA_OUT_OF_RANGE : OB_SUCCESS;
  }
  if (type == ObFloatType || type == ObDoubleType || type == ObUFloatType || type == ObUDoubleType) {
    double number = 0;
    const auto parsed = std::from_chars(first, last, number);
    if (parsed.ec != std::errc() || parsed.ptr != last || !std::isfinite(number)) { return OB_INVALID_ARGUMENT; }
    if ((type == ObUFloatType || type == ObUDoubleType) && number < 0) { return OB_DATA_OUT_OF_RANGE; }
    if ((type == ObFloatType || type == ObUFloatType) && std::abs(number) > std::numeric_limits<float>::max()) { return OB_DATA_OUT_OF_RANGE; }
    return OB_SUCCESS;
  }
  if (column.get_meta_type().is_numeric_type()) {
    if ((*first < '0' || *first > '9') && *first != '-') { return OB_INVALID_ARGUMENT; }
    return ObJsonParser::check_json_syntax(ObString(last-first, first), &allocator);
  }
  return OB_NOT_SUPPORTED;
}

} // namespace observer
} // namespace oceanbase
