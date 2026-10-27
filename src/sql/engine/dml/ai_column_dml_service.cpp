#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ai_column_dml_service.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase {
namespace sql {
using namespace common;
using namespace share::schema;

namespace {
int append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`' && OB_FAIL(sql.append("`"))) {
    } else if (OB_FAIL(sql.append(name.ptr() + i, 1))) {
    }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

// Binary literals are independent of NO_BACKSLASH_ESCAPES and cannot turn a
// business value or a generated row predicate into executable SQL text.
int append_bytes(ObSqlString &sql, const ObString &value)
{
  int ret = sql.append("X'");
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    ret = sql.append_fmt("%02x", static_cast<unsigned char>(value.ptr()[i]));
  }
  if (OB_SUCC(ret)) { ret = sql.append("'"); }
  return ret;
}

int find_datum(const ObDMLBaseCtDef &ctdef, const ExprFixedArray &row,
               uint64_t column_id, ObEvalCtx &ctx, ObDatum *&datum, ObExpr *&expr)
{
  int ret = OB_SUCCESS;
  datum = nullptr;
  expr = nullptr;
  int64_t index = -1;
  for (int64_t i = 0; index < 0 && i < ctdef.column_ids_.count(); ++i) {
    if (ctdef.column_ids_.at(i) == column_id) { index = i; }
  }
  if (index < 0 || index >= row.count() || row.at(index) == nullptr) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("AI task requires a complete DML row", K(ret), K(column_id), K(index));
  } else {
    expr = row.at(index);
    ret = expr->eval(ctx, datum);
  }
  return ret;
}

int column_changed(const ObDMLBaseCtDef &ctdef, uint64_t id,
                   ObEvalCtx &ctx, bool &changed)
{
  int ret = OB_SUCCESS;
  ObDatum *old_value = nullptr;
  ObDatum *new_value = nullptr;
  ObExpr *expr = nullptr;
  changed = false;
  if (OB_FAIL(find_datum(ctdef, ctdef.old_row_, id, ctx, old_value, expr))) {
  } else if (OB_FAIL(find_datum(ctdef, ctdef.new_row_, id, ctx, new_value, expr))) {
  } else {
    changed = !ObDatum::binary_equal(*old_value, *new_value);
  }
  return ret;
}

int make_predicate(const ObTableSchema &table, const ObDMLBaseCtDef &ctdef,
                   const ExprFixedArray &row, ObTableModifyOp &op, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  ObSEArray<uint64_t, 4> ids;
  if (OB_FAIL(table.get_rowkey_info().get_column_ids(ids))) {
  } else if (ids.empty()) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < ids.count(); ++i) {
    const ObColumnSchemaV2 *column = table.get_column_schema(ids.at(i));
    ObDatum *datum = nullptr;
    ObExpr *expr = nullptr;
    ObObj value;
    char *buffer = nullptr;
    int64_t length = 0;
    int64_t pos = 0;
    ObObjPrintParams print = op.get_obj_print_params();
    print.character_hex_safe_represent_ = true;
    print.binary_string_print_hex_ = true;
    if (column == nullptr) {
      ret = OB_ERR_UNEXPECTED;
    } else if (i > 0 && OB_FAIL(sql.append(" AND "))) {
    } else if (OB_FAIL(append_identifier(sql, column->get_column_name_str()))) {
    } else if (OB_FAIL(find_datum(ctdef, row, ids.at(i), op.get_eval_ctx(), datum, expr))) {
    } else if (datum->is_null()) {
      ret = sql.append(" IS NULL");
    } else if (OB_FAIL(sql.append(" = "))) {
    } else if (OB_FAIL(datum->to_obj(value, expr->obj_meta_))) {
    } else if (OB_FAIL(value.print_sql_literal(buffer, length, pos,
                                             op.get_exec_ctx().get_allocator(), print))) {
    } else {
      ret = sql.append(buffer, pos);
    }
  }
  return ret;
}
} // namespace

int AiColumnDmlService::execute_result_write(ObSQLSessionInfo &session,
                                            common::sqlclient::ObISQLConnection &connection,
                                            const ObString &sql, int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  if (session.ai_column_write_once_) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    session.ai_column_write_once_ = true;
    ret = connection.execute_write(sql, affected_rows);
    session.ai_column_write_once_ = false;
  }
  return ret;
}

int AiColumnDmlService::after_row(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef,
                                 ObDmlEventType event)
{
  int ret = OB_SUCCESS;
  if (!ctdef.has_ai_columns_) { return ret; }
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  ObSchemaGetterGuard *guard = op.get_exec_ctx().get_sql_ctx()->schema_guard_;
  ObSQLSessionInfo *session = op.get_exec_ctx().get_my_session();
  const bool deleting = event == ObDmlEventType::DE_DELETING;
  const bool inserting = event == ObDmlEventType::DE_INSERTING;
  ObSEArray<const AiColumnSchema *, 4> affected;
  bool key_changed = false;
  ObSEArray<uint64_t, 4> keys;
  if (guard == nullptr || session == nullptr || op.inner_conn_ == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(guard->get_table_schema(ctdef.das_base_ctdef_.index_tid_, table))) {
  } else if (table == nullptr) {
    ret = OB_SCHEMA_EAGAIN;
  } else if (!inserting && !deleting && event != ObDmlEventType::DE_UPDATING) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(table->get_rowkey_info().get_column_ids(keys))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && !inserting && !deleting
                      && !key_changed && i < keys.count(); ++i) {
    ret = column_changed(ctdef, keys.at(i), op.get_eval_ctx(), key_changed);
  }
  if (OB_SUCC(ret)) {
    for (auto iter = table->column_begin(); OB_SUCC(ret) && iter != table->column_end(); ++iter) {
      const AiColumnSchema *ai = (*iter)->get_ai_column_schema();
      if (ai == nullptr) { continue; }
      bool changed = inserting || deleting || key_changed;
      for (int64_t i = 0; OB_SUCC(ret) && !changed && i < ai->get_dependencies().count(); ++i) {
        ret = column_changed(ctdef, ai->get_dependencies().at(i), op.get_eval_ctx(), changed);
      }
      if (OB_SUCC(ret) && changed) { ret = affected.push_back(ai); }
    }
  }
  if (OB_SUCC(ret) && !affected.empty()) {
    char request[UuidCommon::LENGTH_UUID];
    ObString token(sizeof(request), request);
    ObSqlString predicate;
    ObSqlString invalidate;
    ObSqlString enqueue;
    ObSEArray<ObSqlString, 4> cleanup;
    if (!deleting && OB_FAIL(ObExprUuid::gen_server_uuid(request, sizeof(request)))) {
    } else if (!deleting && OB_FAIL(make_predicate(*table, ctdef, ctdef.new_row_, op, predicate))) {
    } else if (!deleting && OB_FAIL(guard->get_database_schema(table->get_database_id(), database))) {
    } else if (!deleting && database == nullptr) {
      ret = OB_SCHEMA_EAGAIN;
    }
    if (OB_SUCC(ret) && !deleting) {
      if (OB_FAIL(invalidate.append("UPDATE /*+ USE_PLAN_CACHE(NONE) NO_PARALLEL */ "))) {
      } else if (OB_FAIL(append_identifier(invalidate, database->get_database_name_str()))) {
      } else if (OB_FAIL(invalidate.append("."))) {
      } else if (OB_FAIL(append_identifier(invalidate, table->get_table_name_str()))) {
      } else { ret = invalidate.append(" SET "); }
      if (OB_SUCC(ret)) {
        ret = enqueue.append("INSERT /*+ USE_PLAN_CACHE(NONE) */ INTO oceanbase.__all_ai_column_task "
            "(table_id,column_id,request_id,definition_version,rowkey_predicate,max_retries,eligible_ts) VALUES ");
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < affected.count(); ++i) {
      const AiColumnSchema &ai = *affected.at(i);
      if (!inserting) {
        ObDatum *old_request = nullptr;
        ObExpr *expr = nullptr;
        ObSqlString sql;
        if (OB_FAIL(find_datum(ctdef, ctdef.old_row_, ai.get_request_column_id(), op.get_eval_ctx(), old_request, expr))) {
        } else if (!old_request->is_null()) {
          if (OB_FAIL(sql.append_fmt("DELETE /*+ USE_PLAN_CACHE(NONE) */ FROM oceanbase.__all_ai_column_task "
                                     "WHERE table_id=%lu AND column_id=%lu AND request_id=",
                                     table->get_table_id(), ai.get_column_id()))) {
          } else if (OB_FAIL(append_bytes(sql, old_request->get_string()))) {
          } else { ret = cleanup.push_back(sql); }
        }
      }
      if (OB_SUCC(ret) && !deleting) {
        const uint64_t ids[] = {ai.get_column_id(), ai.get_state_column_id(),
                                ai.get_request_column_id(), ai.get_result_spec_column_id()};
        for (int j = 0; OB_SUCC(ret) && j < 4; ++j) {
          const ObColumnSchemaV2 *column = table->get_column_schema(ids[j]);
          if (column == nullptr) {
            ret = OB_SCHEMA_EAGAIN;
          } else if ((i > 0 || j > 0) && OB_FAIL(invalidate.append(","))) {
          } else if (OB_FAIL(append_identifier(invalidate, column->get_column_name_str()))) {
          } else if (OB_FAIL(invalidate.append("="))) {
          } else if (j == 2) {
            ret = append_bytes(invalidate, token);
          } else { ret = invalidate.append(j == 1 ? "0" : "NULL"); }
        }
        if (OB_SUCC(ret) && i > 0) { ret = enqueue.append(","); }
        if (OB_SUCC(ret)) {
          ret = enqueue.append_fmt("(%lu,%lu,", table->get_table_id(), ai.get_column_id());
        }
        if (OB_SUCC(ret) && OB_FAIL(append_bytes(enqueue, token))) {
        } else if (OB_SUCC(ret) && OB_FAIL(enqueue.append_fmt(",%ld,", ai.get_definition_version()))) {
        } else if (OB_SUCC(ret) && OB_FAIL(append_bytes(enqueue, predicate.string()))) {
        } else if (OB_SUCC(ret)) {
          ret = enqueue.append_fmt(",%ld,NOW(6))", ai.get_max_retries());
        }
      }
    }
    if (OB_SUCC(ret) && !deleting) {
      if (OB_FAIL(invalidate.append(" WHERE "))) {
      } else { ret = invalidate.append(predicate.string()); }
    }
    if (OB_SUCC(ret) && OB_FAIL(op.begin_nested_session(true))) {
    } else if (OB_SUCC(ret)) {
      // All writes use the original business session and transaction. No
      // model call is made here. A failure rolls back the enclosing statement.
      for (int64_t i = 0; OB_SUCC(ret) && i < cleanup.count(); ++i) {
        ret = op.execute_write(cleanup.at(i).ptr());
      }
      if (OB_SUCC(ret) && !deleting) {
        int64_t affected_rows = 0;
        ret = execute_result_write(*session, *op.inner_conn_, invalidate.string(), affected_rows);
        if (OB_SUCC(ret) && affected_rows != 1) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("AI invalidation must address exactly one business row", K(ret), K(affected_rows));
        }
        if (OB_SUCC(ret)) { ret = op.execute_write(enqueue.ptr()); }
      }
      const int end_ret = op.end_nested_session();
      if (OB_SUCC(ret)) { ret = end_ret; }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
