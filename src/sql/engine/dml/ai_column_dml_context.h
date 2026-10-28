#pragma once
#include <cstdint>

namespace oceanbase {
namespace sql {
// Bit 4 of ObDASCtx::flags_ is allocated to native AI-column nested writes.
// Keep the existing FK/scan bits unchanged; the context is statement-local.
inline constexpr uint64_t AI_COLUMN_NESTED_DAS_FLAG = uint64_t{1} << 4;
} // namespace sql
} // namespace oceanbase
