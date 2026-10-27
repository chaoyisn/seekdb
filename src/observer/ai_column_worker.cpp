#define USING_LOG_PREFIX SERVER
#include "observer/ai_column_worker.h"
#include "sql/engine/ob_exec_context.h"
#include "observer/ob_inner_sql_connection.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_column_schema.h"
#include "sql/engine/dml/ai_column_dml_service.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "common/json_type/ob_json_parse.h"
#include "lib/thread/ob_thread_name.h"
#include <charconv>
#include <cmath>
#include <limits>
#include <unistd.h>

namespace oceanbase {
namespace observer {
using namespace common;
using namespace share::schema;
using namespace sql;

namespace {
constexpr int PENDING = 0, RUNNING = 1, SUCCEEDED = 2, RETRY_WAIT = 3, FAILED = 4;
constexpr const char *TASKS = "oceanbase.__all_ai_column_task";
struct Task {
  uint64_t table_id = 0, column_id = 0;
  int64_t version = 0, state = 0, retries = 0, max_retries = 0;
  bool active = false, due = false, null_result = true;
  ObString request, payload;
};

int ident(ObSqlString &sql, ObString name)
{
  int ret = OB_SUCCESS;
  OZ(sql.append("`"));
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { OZ(sql.append("`")); }
    OZ(sql.append(name.ptr() + i, 1));
  }
  OZ(sql.append("`"));
  return ret;
}

int text(ObSqlString &sql, ObString value)
{
  int ret = OB_SUCCESS;
  OZ(sql.append("CONVERT(X'"));
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    OZ(sql.append_fmt("%02x", static_cast<unsigned char>(value.ptr()[i])));
  }
  OZ(sql.append("' USING utf8mb4)"));
  return ret;
}

int key(ObSqlString &sql, uint64_t table_id, ObString request)
{
  int ret = OB_SUCCESS;
  OZ(sql.append_fmt("table_id=%lu AND request_id=", table_id));
  OZ(text(sql, request));
  return ret;
}

int write(ObInnerSQLConnection &conn, ObSqlString &sql, int64_t *count = nullptr)
{
  int64_t affected = 0;
  const int ret = conn.execute_write(sql.string(), affected);
  if (count != nullptr) { *count = affected; }
  return ret;
}

int read_tasks(ObInnerSQLConnection &conn, uint64_t table_id, ObString request,
               bool lock, ObIAllocator &alloc, ObIArray<Task> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  OZ(sql.append("SELECT column_id,definition_version,state,retry_count,max_retries,"
                "lease_expire_ts>NOW(6) AS active,eligible_ts<=NOW(6) AS due,"
                "CAST(result_payload AS CHAR) AS payload FROM "));
  OZ(sql.append(TASKS)); OZ(sql.append(" WHERE ")); OZ(key(sql, table_id, request));
  OZ(sql.append(" ORDER BY column_id"));
  if (lock) { OZ(sql.append(" FOR UPDATE")); }
  ObISQLClient::ReadResult result;
  OZ(conn.execute_read(sql.string(), result));
  if (OB_SUCC(ret) && result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
  while (OB_SUCC(ret)) {
    const int next_ret = result.get_result()->next();
    if (next_ret == OB_ITER_END) { break; }
    if (next_ret != OB_SUCCESS) { ret = next_ret; break; }
    Task task;
    task.table_id = table_id;
    OZ(ob_write_string(alloc, request, task.request));
    int64_t column_id = 0;
    OZ(result.get_result()->get_int("column_id", column_id));
    if (OB_SUCC(ret) && column_id <= 0) { ret = OB_INVALID_ARGUMENT; }
    task.column_id = static_cast<uint64_t>(column_id);
    OZ(result.get_result()->get_int("definition_version", task.version));
    OZ(result.get_result()->get_int("state", task.state));
    OZ(result.get_result()->get_int("retry_count", task.retries));
    OZ(result.get_result()->get_int("max_retries", task.max_retries));
    int active_ret = result.get_result()->get_bool("active", task.active);
    if (active_ret != OB_SUCCESS && active_ret != OB_ERR_NULL_VALUE) { ret = active_ret; }
    OZ(result.get_result()->get_bool("due", task.due));
    ObObj payload;
    OZ(result.get_result()->get_obj("payload", payload));
    if (OB_SUCC(ret) && !payload.is_null()) {
      task.null_result = false;
      OZ(ob_write_string(alloc, payload.get_string(), task.payload));
    }
    OZ(tasks.push_back(task));
  }
  return ret;
}

int source_table(ObSqlString &sql, const ObTableSchema &table, ObSchemaGetterGuard &guard)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *database = nullptr;
  OZ(guard.get_database_schema(table.get_database_id(), database));
  if (OB_SUCC(ret) && database == nullptr) { ret = OB_SCHEMA_EAGAIN; }
  if (OB_SUCC(ret)) { OZ(ident(sql, database->get_database_name_str())); }
  OZ(sql.append(".")); OZ(ident(sql, table.get_table_name_str()));
  return ret;
}

int request_predicate(ObSqlString &sql, const ObTableSchema &table, const Task &task)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *column = table.get_column_schema(task.column_id);
  const AiColumnSchema *ai = column == nullptr ? nullptr : column->get_ai_column_schema();
  const ObColumnSchemaV2 *request = ai == nullptr ? nullptr : table.get_column_schema(ai->get_request_column_id());
  if (ai == nullptr || request == nullptr || ai->get_definition_version() != task.version) {
    ret = OB_SCHEMA_EAGAIN;
  } else {
    OZ(ident(sql, request->get_column_name_str())); OZ(sql.append("=")); OZ(text(sql, task.request));
  }
  return ret;
}

int stage(ObInnerSQLConnection &conn, const Task &task, ObString owner,
          int model_error, bool is_null, ObString payload)
{
  int ret = OB_SUCCESS;
  const bool retry = model_error != OB_SUCCESS && task.retries < task.max_retries;
  const int state = model_error == OB_SUCCESS ? SUCCEEDED : retry ? RETRY_WAIT : FAILED;
  ObSqlString sql;
  OZ(sql.append_fmt("UPDATE /*+ USE_PLAN_CACHE(NONE) */ %s SET state=%d,last_error=%d,"
                    "retry_count=%ld,result_payload=", TASKS, state, model_error,
                    task.retries + (retry ? 1 : 0)));
  if (model_error != OB_SUCCESS || is_null) { OZ(sql.append("NULL")); }
  else { OZ(text(sql, payload)); }
  if (retry) {
    // Bounded exponential backoff, visible as committed RETRY_WAIT state.
    const int64_t delay = 1LL << MIN(task.retries, 5L);
    OZ(sql.append_fmt(",eligible_ts=DATE_ADD(NOW(6),INTERVAL %ld SECOND)", delay));
  }
  OZ(sql.append(" WHERE ")); OZ(key(sql, task.table_id, task.request));
  OZ(sql.append_fmt(" AND column_id=%lu AND definition_version=%ld AND state=1 AND owner_id=",
                    task.column_id, task.version));
  OZ(text(sql, owner)); OZ(sql.append(" AND lease_expire_ts>NOW(6)"));
  int64_t affected = 0;
  OZ(write(conn, sql, &affected));
  if (OB_SUCC(ret) && affected != 1) { ret = OB_STATE_NOT_MATCH; }
  return ret;
}

int evaluate(ObInnerSQLConnection &conn, const Task &task, ObSchemaGetterGuard &guard,
             const ObTableSchema &table, ObIAllocator &alloc, bool &is_null, ObString &payload)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *column = table.get_column_schema(task.column_id);
  const AiColumnSchema *ai = column == nullptr ? nullptr : column->get_ai_column_schema();
  if (ai == nullptr || ai->get_definition_version() != task.version) { return OB_SCHEMA_EAGAIN; }
  ObSqlString sql;
  OZ(sql.append("SELECT /*+ USE_PLAN_CACHE(NONE) NO_PARALLEL */ "));
  if (column->is_collection()) { OZ(sql.append("CAST(CONCAT('[',ARRAY_TO_STRING(")); }
  OZ(sql.append(ai->get_normalized_expr()));
  if (column->is_collection()) { OZ(sql.append(",','),']') AS CHAR)")); }
  OZ(sql.append(" AS ai_value FROM ")); OZ(source_table(sql, table, guard));
  OZ(sql.append(" WHERE ")); OZ(request_predicate(sql, table, task));
  ObISQLClient::ReadResult result;
  OZ(conn.execute_read(sql.string(), result));
  if (OB_SUCC(ret) && result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) { ret = result.get_result()->next(); }
  if (OB_SUCC(ret)) {
    ObObj value;
    OZ(result.get_result()->get_obj("ai_value", value));
    is_null = value.is_null();
    if (OB_SUCC(ret) && !is_null) { OZ(ob_write_string(alloc, value.get_string(), payload)); }
    if (OB_SUCC(ret) && conn.get_session().get_warnings_buffer().get_readable_warning_count() > 0) {
      ret = OB_INVALID_ARGUMENT;
    }
    OZ(AiColumnWorker::validate_result(*column, is_null, payload, alloc));
  }
  return ret;
}
} // namespace

int AiColumnWorker::start()
{
  int ret = set_thread_count(2);
  if (OB_SUCC(ret)) { ret = share::ObThreadPool::start(); }
  return ret;
}

void AiColumnWorker::run1()
{
  lib::set_thread_name("AiColWorker");
  sqlclient::ObISQLConnectionGuard guard;
  while (!has_set_stop()) {
    int ret = OB_SUCCESS;
    if (guard.get_ptr() == nullptr) {
      ret = ObInnerSQLConnection::create_connection_with_owned_session(false, 0, guard);
    }
    auto *conn = dynamic_cast<ObInnerSQLConnection *>(guard.get_ptr());
    if (OB_SUCC(ret) && conn != nullptr) {
      ret = process_one(*conn);
      if (ret != OB_SUCCESS && ret != OB_ITER_END && conn->rollback() != OB_SUCCESS) {
        guard.reset();
      }
    }
    if (ret != OB_SUCCESS && ret != OB_ITER_END && ret != OB_STATE_NOT_MATCH && REACH_TIME_INTERVAL(1000000)) {
      LOG_WARN("AI column worker iteration failed", K(ret));
    }
    ::usleep(100000);
  }
}

int AiColumnWorker::process_one(ObInnerSQLConnection &conn)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("AiColumnTask");
  uint64_t table_id = 0;
  ObString request;
  // Discover without locks; lock the entire group in primary-key order below.
  {
    ObISQLClient::ReadResult result;
    const char *query = "SELECT table_id,request_id FROM oceanbase.__all_ai_column_task "
        "WHERE (state IN (0,3) AND eligible_ts<=NOW(6)) "
        "OR (state IN (1,2,4) AND lease_expire_ts<=NOW(6)) "
        "ORDER BY eligible_ts,table_id,column_id LIMIT 1";
    OZ(conn.execute_read(ObString::make_string(query), result));
    if (OB_SUCC(ret) && result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    if (OB_SUCC(ret)) { ret = result.get_result()->next(); }
    int64_t signed_table_id = 0;
    OZ(result.get_result()->get_int("table_id", signed_table_id));
    if (OB_SUCC(ret) && signed_table_id <= 0) { ret = OB_INVALID_ARGUMENT; }
    table_id = static_cast<uint64_t>(signed_table_id);
    ObString value;
    OZ(result.get_result()->get_varchar("request_id", value));
    OZ(ob_write_string(alloc, value, request));
  }
  if (OB_FAIL(ret)) { return ret; }
  char owner_buffer[UuidCommon::LENGTH_UUID];
  OZ(ObExprUuid::gen_server_uuid(owner_buffer, sizeof(owner_buffer)));
  ObString owner(sizeof(owner_buffer), owner_buffer);
  ObSEArray<Task, 4> tasks;
  OZ(conn.start_transaction());
  if (OB_FAIL(ret)) { return ret; }
  OZ(read_tasks(conn, table_id, request, true, alloc, tasks));
  if (OB_SUCC(ret) && tasks.empty()) { ret = OB_ITER_END; }
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
    if (tasks.at(i).state == RUNNING && tasks.at(i).active) { ret = OB_STATE_NOT_MATCH; }
  }
  ObSqlString claim;
  OZ(claim.append_fmt("UPDATE /*+ USE_PLAN_CACHE(NONE) */ %s SET owner_id=", TASKS));
  OZ(text(claim, owner));
  OZ(claim.append(",lease_expire_ts=DATE_ADD(NOW(6),INTERVAL 60 SECOND),state="
                  "CASE WHEN state IN (0,1) OR (state=3 AND eligible_ts<=NOW(6)) THEN 1 ELSE state END WHERE "));
  OZ(key(claim, table_id, request)); OZ(write(conn, claim));
  if (OB_SUCC(ret)) { ret = conn.commit(); }
  if (OB_FAIL(ret)) { (void)conn.rollback(); }
  if (OB_FAIL(ret)) { return ret; }

  ObSchemaGetterGuard schema;
  const ObTableSchema *table = nullptr;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(schema));
  OZ(schema.get_table_schema(table_id, table));
  if (OB_SUCC(ret) && table == nullptr) {
    ObSqlString remove;
    OZ(remove.append_fmt("DELETE FROM %s WHERE ", TASKS)); OZ(key(remove, table_id, request));
    OZ(remove.append(" AND owner_id=")); OZ(text(remove, owner)); OZ(write(conn, remove));
    return ret;
  }
  for (int64_t i = 0; OB_SUCC(ret) && !has_set_stop() && i < tasks.count(); ++i) {
    Task &task = tasks.at(i);
    const ObColumnSchemaV2 *column = table->get_column_schema(task.column_id);
    const AiColumnSchema *definition = column == nullptr ? nullptr : column->get_ai_column_schema();
    const bool obsolete = definition == nullptr || definition->get_definition_version() != task.version;
    if (!obsolete && (task.state == SUCCEEDED || task.state == FAILED || (task.state == RETRY_WAIT && !task.due))) { continue; }
    bool is_null = true;
    ObString payload;
    const int model_error = obsolete ? OB_SCHEMA_EAGAIN : evaluate(conn, task, schema, *table, alloc, is_null, payload);
    if (model_error == OB_ITER_END || model_error == OB_SCHEMA_EAGAIN) {
      ObSqlString remove;
      OZ(remove.append_fmt("DELETE FROM %s WHERE ", TASKS)); OZ(key(remove, table_id, request));
      OZ(remove.append_fmt(" AND column_id=%lu AND owner_id=", task.column_id)); OZ(text(remove, owner)); OZ(write(conn, remove));
    } else {
      OZ(stage(conn, task, owner, model_error, is_null, payload));
    }
  }
  if (OB_FAIL(ret) || has_set_stop()) { return ret; }
  // Re-read survivors: a T1 dependency update may have removed some members.
  tasks.reuse(); OZ(read_tasks(conn, table_id, request, false, alloc, tasks));
  bool terminal = !tasks.empty();
  for (int64_t i = 0; i < tasks.count(); ++i) { terminal = terminal && (tasks.at(i).state == SUCCEEDED || tasks.at(i).state == FAILED); }
  if (OB_FAIL(ret) || !terminal) { return ret; }
  // The schema pinned before HTTP can be obsolete after the response.
  ObSchemaGetterGuard publish_schema;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(publish_schema));
  OZ(publish_schema.get_table_schema(table_id, table));
  if (OB_SUCC(ret) && table == nullptr) { ret = OB_SCHEMA_EAGAIN; }
  OZ(conn.start_transaction());
  if (OB_FAIL(ret)) { return ret; }
  // Lock business row first, matching the T1 business-row -> task-row order.
  ObSqlString lock_row;
  OZ(lock_row.append("SELECT 1 AS found FROM ")); OZ(source_table(lock_row, *table, publish_schema));
  OZ(lock_row.append(" WHERE "));
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
    if (i > 0) { OZ(lock_row.append(" AND ")); }
    OZ(request_predicate(lock_row, *table, tasks.at(i)));
  }
  OZ(lock_row.append(" FOR UPDATE"));
  {
    ObISQLClient::ReadResult result;
    OZ(conn.execute_read(lock_row.string(), result));
    if (OB_SUCC(ret) && result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    if (OB_SUCC(ret)) { ret = result.get_result()->next(); }
  }
  ObSEArray<Task, 4> locked;
  OZ(read_tasks(conn, table_id, request, true, alloc, locked));
  if (OB_SUCC(ret) && locked.count() != tasks.count()) { ret = OB_STATE_NOT_MATCH; }
  // The SQL ownership predicate below must cover every member, not just count.
  ObSqlString verify;
  OZ(verify.append_fmt("SELECT count(*) AS cnt FROM %s WHERE ", TASKS)); OZ(key(verify, table_id, request));
  OZ(verify.append(" AND owner_id=")); OZ(text(verify, owner));
  OZ(verify.append(" AND lease_expire_ts>NOW(6) AND state IN (2,4)"));
  {
    ObISQLClient::ReadResult result;
    OZ(conn.execute_read(verify.string(), result));
    if (OB_SUCC(ret) && result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    if (OB_SUCC(ret)) { ret = result.get_result()->next(); }
    int64_t count = 0;
    OZ(result.get_result()->get_int("cnt", count));
    if (OB_SUCC(ret) && count != tasks.count()) { ret = OB_STATE_NOT_MATCH; }
  }
  ObSqlString publish;
  OZ(publish.append("UPDATE /*+ USE_PLAN_CACHE(NONE) NO_PARALLEL */ "));
  OZ(source_table(publish, *table, publish_schema)); OZ(publish.append(" SET "));
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
    const Task &task = tasks.at(i);
    const ObColumnSchemaV2 *column = table->get_column_schema(task.column_id);
    const AiColumnSchema *ai = column == nullptr ? nullptr : column->get_ai_column_schema();
    if (ai == nullptr || ai->get_definition_version() != task.version) { ret = OB_SCHEMA_EAGAIN; break; }
    const ObColumnSchemaV2 *state = table->get_column_schema(ai->get_state_column_id());
    if (state == nullptr) { ret = OB_SCHEMA_EAGAIN; break; }
    if (i > 0) { OZ(publish.append(",")); }
    OZ(ident(publish, column->get_column_name_str())); OZ(publish.append("="));
    if (task.state == FAILED || task.null_result) { OZ(publish.append("NULL")); }
    else { OZ(text(publish, task.payload)); }
    OZ(publish.append(",")); OZ(ident(publish, state->get_column_name_str()));
    OZ(publish.append_fmt("=%d", task.state == FAILED ? 2 : 1));
  }
  OZ(publish.append(" WHERE "));
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
    if (i > 0) { OZ(publish.append(" AND ")); }
    OZ(request_predicate(publish, *table, tasks.at(i)));
  }
  int64_t affected = 0;
  OZ(AiColumnDmlService::execute_result_write(conn.get_session(), conn, publish.string(), affected));
  if (OB_SUCC(ret) && affected != 1) { ret = OB_STATE_NOT_MATCH; }
  ObSqlString remove;
  OZ(remove.append_fmt("DELETE FROM %s WHERE ", TASKS)); OZ(key(remove, table_id, request));
  OZ(remove.append(" AND owner_id=")); OZ(text(remove, owner)); OZ(write(conn, remove));
  if (OB_SUCC(ret)) { ret = conn.commit(); }
  if (OB_FAIL(ret)) { (void)conn.rollback(); }
  return ret;
}

} // namespace observer
} // namespace oceanbase
