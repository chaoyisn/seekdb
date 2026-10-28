#define USING_LOG_PREFIX SERVER
#include "observer/virtual_table/ob_ai_column_metadata_table.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase {
namespace observer {
using namespace common;
using namespace share::schema;

void AiColumnMetadataTable::reset()
{
  tables_.reset();
  table_idx_ = column_idx_ = 0;
  ObVirtualTableIterator::reset();
}

int AiColumnMetadataTable::inner_open()
{
  tables_.reuse();
  table_idx_ = column_idx_ = 0;
  return schema_guard_ == nullptr || allocator_ == nullptr || session_ == nullptr
      ? OB_NOT_INIT : schema_guard_->get_table_schemas_in_runtime(tables_);
}

int AiColumnMetadataTable::inner_get_next_row(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  row = nullptr;
  if (schema_guard_ == nullptr || allocator_ == nullptr || session_ == nullptr) {
    ret = OB_NOT_INIT;
  }
  while (OB_SUCC(ret) && row == nullptr && table_idx_ < tables_.count()) {
    const ObTableSchema *table = tables_.at(table_idx_);
    if (table == nullptr) {
      ret = OB_ERR_UNEXPECTED;
    } else if (table->is_in_recyclebin() || column_idx_ >= table->get_column_count()) {
      ++table_idx_;
      column_idx_ = 0;
    } else {
      const ObColumnSchemaV2 *column = table->get_column_schema_by_idx(column_idx_++);
      if (column == nullptr) {
        ret = OB_ERR_UNEXPECTED;
      } else if (column->is_ai_column()) {
        const ObDatabaseSchema *database = nullptr;
        bool allowed = false;
        if (OB_FAIL(schema_guard_->get_database_schema(table->get_database_id(), database))) {
        } else if (database == nullptr) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(check_priv(ObString::make_string("table_acc"),
                                     database->get_database_name_str(),
                                     table->get_table_name_str(), allowed))) {
        }
        if (OB_FAIL(ret)) { break; }
        if (!allowed) { continue; }
        row_calc_buf_.reuse();
        if (OB_FAIL(project_column(table->get_table_id(), *column,
                                   output_column_ids_, row_calc_buf_, cur_row_))) {
        } else {
          row = &cur_row_;
        }
      }
    }
  }
  if (OB_SUCC(ret) && row == nullptr) { ret = OB_ITER_END; }
  return ret;
}

int AiColumnMetadataTable::project_column(uint64_t table_id, const ObColumnSchemaV2 &column,
                                         const ObIArray<uint64_t> &projection,
                                         ObIAllocator &allocator, ObNewRow &row)
{
  int ret = OB_SUCCESS;
  const AiColumnSchema *definition = column.get_ai_column_schema();
  if (definition == nullptr || !definition->is_valid()
      || definition->get_column_id() != column.get_column_id()
      || row.count_ != projection.count() || (row.count_ > 0 && row.cells_ == nullptr)) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < projection.count(); ++i) {
    ObObj &cell = row.cells_[i];
    switch (projection.at(i)) {
      case TABLE_ID: cell.set_int(table_id); break;
      case COLUMN_ID: cell.set_int(column.get_column_id()); break;
      case COLUMN_NAME: cell.set_varchar(column.get_column_name_str()); break;
      case DEFINITION_VERSION: cell.set_int(definition->get_definition_version()); break;
      case EXPRESSION: cell.set_varchar(definition->get_expression()); break;
      case NORMALIZED_EXPR: cell.set_varchar(definition->get_normalized_expr()); break;
      case SYNC_MODE: cell.set_varchar(ObString::make_string("async")); break;
      case MAX_RETRIES: cell.set_int(definition->get_max_retries()); break;
      case STATE_COLUMN_ID: cell.set_int(definition->get_state_column_id()); break;
      case REQUEST_COLUMN_ID: cell.set_int(definition->get_request_column_id()); break;
      case RESULT_SPEC_COLUMN_ID: cell.set_int(definition->get_result_spec_column_id()); break;
      case DEPENDENCY_COLUMN_IDS: {
        ObSqlString json;
        ObString value;
        if (OB_FAIL(json.append("["))) {
        }
        const ObIArray<uint64_t> &ids = definition->get_dependencies();
        for (int64_t j = 0; OB_SUCC(ret) && j < ids.count(); ++j) {
          ret = json.append_fmt("%s%lu", j == 0 ? "" : ",", ids.at(j));
        }
        if (OB_SUCC(ret) && OB_FAIL(json.append("]"))) {
        } else if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator, json.string(), value))) {
        } else if (OB_SUCC(ret)) {
          cell.set_varchar(value);
        }
        break;
      }
      default: ret = OB_ERR_UNEXPECTED; break;
    }
    if (OB_SUCC(ret) && cell.is_varchar()) {
      cell.set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
