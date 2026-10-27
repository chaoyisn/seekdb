#pragma once

#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "common/mysqlclient/ob_isql_connection.h"

namespace oceanbase {
namespace sql {
class ObTableModifyOp;

class AiColumnDmlService
{
public:
  static int execute_result_write(ObSQLSessionInfo &session,
                                  common::sqlclient::ObISQLConnection &connection,
                                  const common::ObString &sql, int64_t &affected_rows);
  static int after_row(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef,
                       ObDmlEventType event);
};

} // namespace sql
} // namespace oceanbase
