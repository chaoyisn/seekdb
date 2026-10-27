#pragma once

#include "sql/engine/dml/ob_dml_ctx_define.h"

namespace oceanbase {
namespace sql {
class ObTableModifyOp;

class AiColumnDmlService
{
public:
  static int after_row(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef,
                       ObDmlEventType event);
};

} // namespace sql
} // namespace oceanbase
