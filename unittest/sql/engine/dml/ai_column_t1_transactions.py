"""Supplemental real-server T1 checks; does not implement AI COLUMN behavior.

Run only against an isolated instance built with the T1 kernel changes.
The endpoint is deliberately offline and retries are bounded, so a future
worker cannot drain these tasks before the short transactional assertions.
"""
import argparse
import json
import time
import re
from pathlib import Path
import pymysql

parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True, type=int)
parser.add_argument('--report', required=True, type=Path)
parser.add_argument('--namespace', default='ai_column_t1_supplemental')
args = parser.parse_args()
if not re.fullmatch(r'[a-zA-Z_][a-zA-Z_0-9]{0,60}', args.namespace):
    raise ValueError('Invalid isolated test namespace')
db = args.namespace
model = args.namespace + '_model'
endpoint = args.namespace + '_endpoint'
checks = []
started = time.time()
conn = pymysql.connect(host='127.0.0.1', port=args.port, user='root',
                       autocommit=True, read_timeout=30, write_timeout=30)
peer = pymysql.connect(host='127.0.0.1', port=args.port, user='root',
                       autocommit=True, read_timeout=30, write_timeout=30)

def sql(query, connection=conn):
    print('SQL:', query, flush=True)
    with connection.cursor() as cur:
        cur.execute(query)
        return cur.fetchall()

def eq(name, query, expected, connection=conn):
    actual = sql(query, connection)
    ok = actual == expected
    checks.append({'name': name, 'passed': ok, 'actual': actual, 'expected': expected})
    if not ok:
        raise AssertionError((name, actual, expected))

def denied(name, query):
    try:
        sql(query)
    except pymysql.MySQLError as exc:
        ok = exc.args[0] == 1235
        checks.append({'name': name, 'passed': ok, 'error': exc.args})
        if not ok: raise
    else:
        checks.append({'name': name, 'passed': False, 'error': 'unexpected success'})
        raise AssertionError(name)

task_query = ''
exit_code = 0
try:
    # A second run must inspect/clean its own failed test instance first.
    sql('CREATE DATABASE ' + db)
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('%s',"
        "'{\"type\":\"completion\",\"model_name\":\"mock-complete\"}')" % model)
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('%s',"
        "'{\"ai_model_name\":\"%s\",\"url\":\"http://127.0.0.1:1/v1/chat/completions\","
        "\"access_key\":\"supplemental\",\"provider\":\"openai\","
        "\"request_model_name\":\"mock-complete\"}')" % (endpoint, model))
    sql(f'''CREATE TABLE {db}.docs(
        id BIGINT PRIMARY KEY, content VARCHAR(200), title VARCHAR(200),
        tags JSON AI COLUMN(AI_COMPLETE('{model}',content)) WITH(max_retries=100),
        score BIGINT AI COLUMN(AI_COMPLETE('{model}',content)) WITH(max_retries=100))''')
    table_id = sql(f"SELECT t.table_id FROM oceanbase.__all_table t JOIN oceanbase.__all_database d "
                   f"ON d.database_id=t.database_id WHERE d.database_name='{db}' AND t.table_name='docs'")[0][0]
    task_query = f'oceanbase.__all_ai_column_task WHERE table_id={table_id}'
    sql('START TRANSACTION')
    sql(f"INSERT INTO {db}.docs(id,content,title) VALUES(1,'initial','title')")
    eq('insert results invalidated', f'SELECT tags IS NULL,score IS NULL FROM {db}.docs', ((1,1),))
    eq('same transaction sees grouped tasks', f'SELECT count(*),count(DISTINCT request_id),min(eligible_ts<=NOW(6)) FROM {task_query}', ((2,1,1),))
    eq('peer cannot see uncommitted tasks', f'SELECT count(*) FROM {task_query}', ((0,),), peer)
    eq('peer cannot see uncommitted business row', f'SELECT count(*) FROM {db}.docs', ((0,),), peer)
    sql('ROLLBACK')
    eq('rollback removes tasks', f'SELECT count(*) FROM {task_query}', ((0,),), peer)
    eq('rollback removes business row', f'SELECT count(*) FROM {db}.docs', ((0,),))
    sql(f"INSERT INTO {db}.docs(id,content,title) VALUES(1,'initial','title')")
    eq('commit publishes tasks to peer', f'SELECT count(*) FROM {task_query}', ((2,),), peer)
    original = sql(f'SELECT DISTINCT request_id FROM {task_query}')
    sql(f"UPDATE {db}.docs SET title='unrelated' WHERE id=1")
    eq('unrelated field keeps request', f'SELECT DISTINCT request_id FROM {task_query}', original)
    sql(f"UPDATE {db}.docs SET content=content WHERE id=1")
    eq('equal dependency keeps request', f'SELECT DISTINCT request_id FROM {task_query}', original)
    sql('START TRANSACTION')
    sql(f"UPDATE {db}.docs SET content='rolled back' WHERE id=1")
    changed = sql(f'SELECT DISTINCT request_id FROM {task_query}')
    checks.append({'name':'dependency change replaces request','passed':len(changed)==1 and changed!=original})
    assert checks[-1]['passed'], checks[-1]
    eq('peer still sees old tasks before rollback', f'SELECT DISTINCT request_id FROM {task_query}', original, peer)
    sql('ROLLBACK')
    eq('rollback restores request', f'SELECT DISTINCT request_id FROM {task_query}', original)
    eq('rollback restores input', f'SELECT content FROM {db}.docs', (('initial',),))
    sql(f"UPDATE {db}.docs SET content='updated' WHERE id=1")
    eq('dependency change has exactly two tasks', f'SELECT count(*),count(DISTINCT request_id) FROM {task_query}', ((2,1),))
    sql(f'UPDATE {db}.docs SET id=2 WHERE id=1')
    eq('primary key task predicate', f'SELECT DISTINCT rowkey_predicate FROM {task_query}', (('`id` = 2',),))
    eq('old primary key absent', f'SELECT count(*) FROM {db}.docs WHERE id=1', ((0,),))
    sql('START TRANSACTION')
    sql(f'DELETE FROM {db}.docs WHERE id=2')
    eq('delete cleans tasks in transaction', f'SELECT count(*) FROM {task_query}', ((0,),))
    sql('ROLLBACK')
    eq('rollback restores deleted tasks', f'SELECT count(*) FROM {task_query}', ((2,),))
    sql(f'DELETE FROM {db}.docs WHERE id=2')
    eq('committed delete cleans tasks', f'SELECT count(*) FROM {task_query}', ((0,),), peer)
    sql('START TRANSACTION')
    sql(f"INSERT INTO {db}.docs(id,content) VALUES(10,'ten'),(11,'eleven')")
    eq('batch tasks and row groups', f'SELECT count(*),count(DISTINCT request_id) FROM {task_query}', ((4,2),))
    sql('ROLLBACK')
    eq('batch rollback removes all tasks', f'SELECT count(*) FROM {task_query}', ((0,),))
    denied('explicit INSERT result assignment', f'INSERT INTO {db}.docs(id,content,tags) VALUES(20,\'x\',NULL)')
    denied('direct UPDATE result assignment', f'UPDATE {db}.docs SET tags=NULL')
    denied('INSERT SELECT result assignment', f'INSERT INTO {db}.docs(id,tags) SELECT 20,NULL')
except Exception as exc:
    exit_code = 1
    checks.append({'name':'execution failure','passed':False,'error':repr(exc)})
finally:
    try: sql('ROLLBACK')
    except Exception: pass
    conn.close()
    peer.close()
    payload = {'kind':'supplemental real-server T1 integration','exit_code':exit_code,
               'passed':sum(c['passed'] for c in checks),'checks':checks,
               'elapsed_seconds':time.time()-started,'namespace':db,
               'cleanup':'Inspect owned database/models and preserve server logs before rerun.'}
    args.report.write_text(json.dumps(payload,indent=2,ensure_ascii=False))
    print(json.dumps(payload,indent=2,ensure_ascii=False))
raise SystemExit(exit_code)
