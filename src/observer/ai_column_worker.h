#pragma once

#include "share/ob_thread_pool.h"
#include "lib/string/ob_string.h"

namespace oceanbase {
namespace common { class ObIAllocator; }
namespace share { namespace schema { class ObColumnSchemaV2; } }
namespace observer {
class ObInnerSQLConnection;

class AiColumnWorker : public share::ObThreadPool
{
public:
  int start();
  void run1() override;
  static int validate_result(const share::schema::ObColumnSchemaV2 &column,
                             bool is_null, common::ObString value,
                             common::ObIAllocator &allocator);
private:
  int process_one(ObInnerSQLConnection &connection);
};

} // namespace observer
} // namespace oceanbase
