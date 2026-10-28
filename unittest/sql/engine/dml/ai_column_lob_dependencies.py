"""Real-server LOB dependency regressions; no application AI implementation."""
import argparse,json,re,time
from pathlib import Path
import pymysql
p=argparse.ArgumentParser();p.add_argument('--port',type=int,required=True);p.add_argument('--namespace',required=True);p.add_argument('--report',type=Path,required=True);a=p.parse_args()
if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]{0,48}',a.namespace):raise ValueError('Invalid isolated namespace')
db=a.namespace;model=db+'_model';endpoint=db+'_endpoint';checks=[];exit_code=0
conn=pymysql.connect(host='127.0.0.1',port=a.port,user='root',autocommit=True,read_timeout=40)
def sql(query,params=None):
 with conn.cursor() as c:c.execute(query,params);return c.fetchall()
def check(name,actual,expected):
 ok=actual==expected;checks.append({'name':name,'passed':ok});assert ok,(name,actual,expected)
try:
 sql(f'CREATE DATABASE {db}')
 sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',(model,json.dumps({'type':'completion','model_name':'mock-complete'})))
 sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',(endpoint,json.dumps({'ai_model_name':model,'url':'http://127.0.0.1:1/v1/chat/completions','access_key':'supplemental','provider':'openai','request_model_name':'mock-complete'})))
 sql(f"CREATE TABLE {db}.docs(id BIGINT PRIMARY KEY,content LONGTEXT,title VARCHAR(40),result VARCHAR(400) AI COLUMN(AI_COMPLETE('{model}',content)) WITH(max_retries=0))")
 request_name=sql('''SELECT c.column_name FROM oceanbase.__all_ai_column a
 JOIN oceanbase.__all_column c ON c.table_id=a.table_id AND c.column_id=a.request_column_id
 JOIN oceanbase.__all_table t ON t.table_id=a.table_id
 JOIN oceanbase.__all_database d ON d.database_id=t.database_id
 WHERE d.database_name=%s AND t.table_name='docs' AND a.column_name='result' ''',(db,))[0][0]
 request='`'+request_name.replace('`','``')+'`'
 def token():return sql(f'SELECT {request} FROM {db}.docs WHERE id=1')[0][0]
 sql(f'INSERT INTO {db}.docs(id,content,title) VALUES(1,NULL,\'initial\')');first=token()
 sql(f'UPDATE {db}.docs SET content=NULL WHERE id=1');check('NULL to NULL keeps request',token(),first)
 payload='x'*65535+'A';sql(f'UPDATE {db}.docs SET content=%s WHERE id=1',(payload,));large=token()
 check('NULL to out-of-row LOB schedules',large!=first,True)
 sql(f'UPDATE {db}.docs SET content=CONCAT(SUBSTRING(content,1,32768),SUBSTRING(content,32769)) WHERE id=1')
 check('equal reconstructed LOB keeps request',token(),large)
 sql(f'UPDATE {db}.docs SET title=\'other\' WHERE id=1');check('unrelated update keeps LOB request',token(),large)
 sql('START TRANSACTION');sql(f'UPDATE {db}.docs SET content=%s WHERE id=1',('x'*65535+'B',));changed=token()
 check('same-size changed LOB schedules',changed!=large,True)
 sql('ROLLBACK');check('rollback restores LOB request',token(),large)
 check('rollback restores LOB content',sql(f'SELECT LENGTH(content),RIGHT(content,1) FROM {db}.docs WHERE id=1'),((65536,'A'),))
 sql(f'UPDATE {db}.docs SET content=\'\' WHERE id=1');empty=token();check('LOB to empty schedules',empty!=large,True)
 sql(f'UPDATE {db}.docs SET content=NULL WHERE id=1');check('empty to NULL schedules',token()!=empty,True)
 sql(f'DROP DATABASE {db}')
 sql('CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)',(endpoint,));sql('CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)',(model,))
except Exception as exc:
 exit_code=1;checks.append({'name':'exception','passed':False,'error':repr(exc)})
 try:conn.rollback()
 except Exception:pass
finally:
 conn.close();result={'kind':'supplemental actual-server LOB dependency test','exit_code':exit_code,'namespace':db,'passed':sum(x['passed'] for x in checks),'checks':checks,'unrun_until_report_exists':False}
 a.report.write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
raise SystemExit(exit_code)
