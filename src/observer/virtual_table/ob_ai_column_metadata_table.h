#pragma once

#include "observer/virtual_table/ob_virtual_table_iterator.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase {
namespace observer {

// Read-only SQL projection of the authoritative, versioned column catalog.
class AiColumnMetadataTable : public common::ObVirtualTableIterator
{
public:
  enum ColumnId {
    TABLE_ID = common::OB_APP_MIN_COLUMN_ID,
    COLUMN_ID, COLUMN_NAME, DEFINITION_VERSION, EXPRESSION, NORMALIZED_EXPR,
    SYNC_MODE, MAX_RETRIES, STATE_COLUMN_ID, REQUEST_COLUMN_ID,
    RESULT_SPEC_COLUMN_ID, DEPENDENCY_COLUMN_IDS
  };
  void reset() override;
  int inner_open() override;
  int inner_get_next_row(common::ObNewRow *&row) override;
  static int project_column(uint64_t table_id, const share::schema::ObColumnSchemaV2 &column,
                            const common::ObIArray<uint64_t> &projection,
                            common::ObIAllocator &allocator, common::ObNewRow &row);
private:
  common::ObSEArray<const share::schema::ObTableSchema *, 16> tables_;
  int64_t table_idx_ = 0;
  int64_t column_idx_ = 0;
};

} // namespace observer
} // namespace oceanbase
