"""Supplemental SQL/protocol regressions against an explicitly selected isolated DB.

Requires PyMySQL and the companion protocol Mock. Does not implement AI functions.
Never point this at the original contest database on port 2881.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
import urllib.request
from urllib.parse import urlparse
import ipaddress

import pymysql

sys.stdout.reconfigure(encoding='utf-8')
parser = argparse.ArgumentParser()
parser.add_argument('--host', default='127.0.0.1')
parser.add_argument('--port', type=int, required=True)
parser.add_argument('--mock', default='http://127.0.0.1:8000')
parser.add_argument('--report', type=Path, required=True)
args = parser.parse_args()
if args.port == 2881:
    parser.error('Refusing original contest port 2881')
mock_url = urlparse(args.mock)
if mock_url.scheme != 'http' or not mock_url.hostname:
    parser.error('Mock must be a local HTTP service')
if mock_url.hostname != 'localhost':
    try:
        if not ipaddress.ip_address(mock_url.hostname).is_private:
            parser.error('Refusing non-local Mock destination')
    except ValueError:
        parser.error('Mock must use localhost or a private IP address')
args.mock = args.mock.rstrip('/')
mock_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
connection = pymysql.connect(host=args.host, port=args.port, user='root', charset='utf8mb4',
                             autocommit=True, connect_timeout=10, read_timeout=35)
records = []
prefix = 'contest_ai_validation_'
created = []
created_tables = []

def sql(statement, params=None):
    with connection.cursor() as cursor:
        cursor.execute(statement, params)
        return cursor.fetchall() if cursor.description else ()

def value(statement, params=None):
    return sql(statement, params)[0][0]

def events():
    return json.load(mock_opener.open(args.mock + '/__stats', timeout=10))['requests']

def embedding(text, dimension):
    digest = hashlib.sha256(text.encode('utf-8')).digest()
    return [round((digest[i % len(digest)] - 128) / 128.0, 6) for i in range(dimension)]

def model(kind, provider, path):
    name = prefix + kind
    endpoint = name + '_ep'
    sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)', (name, json.dumps({'type': kind if kind != 'embedding' else 'dense_embedding', 'model_name': 'protocol-fixture-model'})))
    created.append((name, endpoint))
    sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)', (endpoint, json.dumps({
        'ai_model_name': name, 'url': args.mock + path, 'provider': provider,
        'access_key': '' if provider.lower() == 'ollama' else 'dummy-fixture-key'})))
    return name, endpoint

def endpoint_url(endpoint, path):
    sql('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)', (endpoint, json.dumps({'url': args.mock + path})))

def check(name, function):
    start = time.monotonic()
    try:
        function()
        item = {'name': name, 'status': 'PASS'}
    except Exception as error:
        item = {'name': name, 'status': 'FAIL', 'error': repr(error)}
    item['seconds'] = round(time.monotonic() - start, 4)
    records.append(item)
    print(item, flush=True)

def error_expected(statement, params=None):
    try:
        sql(statement, params)
    except pymysql.MySQLError:
        assert value('SELECT 1') == 1, 'DB must remain healthy after error'
    else:
        raise AssertionError('Expected SQL error, got success: ' + statement)

try:
    sql('CREATE DATABASE IF NOT EXISTS contest_ai_validation')
    sql('USE contest_ai_validation')
    version = value('SELECT VERSION()')
    complete, complete_ep = model('completion', 'openai', '/v1/chat/completions')
    embed, embed_ep = model('embedding', 'openai', '/v1/embeddings')
    rerank, rerank_ep = model('rerank', 'siliconflow', '/v1/rerank')

    def prompt_json_lob():
        sql('CREATE TABLE prompt_lob(template LONGTEXT, argument TEXT)')
        created_tables.append('prompt_lob')
        sql('INSERT INTO prompt_lob VALUES (%s,%s)', ('{0}/{0}', '雪"\n\\'))
        output = json.loads(value('SELECT AI_PROMPT(template, argument) FROM prompt_lob'))
        assert output['args'] == ['雪"\n\\']
        output = json.loads(value("SELECT AI_PROMPT('{0}/{1}', JSON_ARRAY(1,TRUE), JSON_OBJECT('k',2))"))
        assert output['args'] == [[1, True], {'k': 2}]
    check('Prompt: LOB, JSON containers, Unicode/escaping', prompt_json_lob)

    def repeated_nested_prompt():
        text = '长文本"\n\\' * 4096
        begin = len(events())
        sql("SELECT AI_COMPLETE(%s,AI_PROMPT('{0}/{0}/{0}',AI_PROMPT('<{0}>',%s)),NULL)", (complete, text))
        request = events()[begin:][-1]['body']
        assert request['messages'][-1]['content'] == '/'.join(['<' + text + '>'] * 3)
        error_expected("SELECT AI_COMPLETE(%s,AI_PROMPT('{999999999999999999999999999}', 'x'))", (complete,))
        error_expected("SELECT AI_COMPLETE(%s,AI_PROMPT('{1}', 'x'))", (complete,))
    check('Prompt: long repeated/nested expansion and invalid indices', repeated_nested_prompt)

    def complete_config():
        begin = len(events())
        assert value('SELECT LENGTH(AI_COMPLETE(%s,%s,CAST(%s AS JSON)))', (complete, 'hello', '{"temperature":0.25,"max_tokens":17}')) > 0
        request = events()[begin:][-1]['body']
        assert request['temperature'] == 0.25 and request['max_tokens'] == 17
        error_expected('SELECT AI_COMPLETE(%s,%s,%s)', (complete, 'hello', '[]'))
        error_expected('SELECT AI_COMPLETE(%s,%s,%s)', (complete, 'hello', '{broken'))
    check('Complete: typed JSON config, forwarded options, invalid config', complete_config)

    def partial_response_retry():
        endpoint_url(complete_ep, '/partial_retry/v1/chat/completions')
        begin = len(events())
        try:
            assert value('SELECT LENGTH(AI_COMPLETE(%s,%s))', (complete, 'partial transport retry')) > 0
            requests = [item for item in events()[begin:] if '/partial_retry/' in item['path']]
            assert len(requests) == 2, requests
        finally:
            endpoint_url(complete_ep, '/v1/chat/completions')
    check('HTTP partial response retry discards incomplete first response', partial_response_retry)

    def single_embedding():
        assert len(json.loads(value('SELECT AI_EMBED(%s,%s)', (embed, 'default')))) == 768
        assert json.loads(value('SELECT AI_EMBED(%s,%s,7)', (embed, 'specific'))) == embedding('specific', 7)
        assert value('SELECT AI_EMBED(%s,NULL)', (embed,)) is None
        for dimension in ['0', '-1', '1.5', 'NULL', "'bad'"]:
            error_expected('SELECT AI_EMBED(%s,%s,' + dimension + ')', (embed, 'invalid'))
    check('Embed: inferred/explicit dimension, invalid dimension and NULL', single_embedding)

    rows = [(i, None if i % 13 == 0 else 'row-' + str(i)) for i in range(1, 181)]
    sql('CREATE TABLE embed_rows(id INT PRIMARY KEY, content TEXT)')
    created_tables.append('embed_rows')
    with connection.cursor() as cursor:
        cursor.executemany('INSERT INTO embed_rows VALUES (%s,%s)', rows)

    def batch_embedding():
        begin = len(events())
        output = sql('SELECT id, AI_EMBED(%s,content,7) FROM embed_rows ORDER BY id', (embed,))
        for (key, vector), (expected_key, text) in zip(output, rows):
            assert key == expected_key
            assert (None if vector is None else json.loads(vector)) == (None if text is None else embedding(text, 7))
        assert len(output) == len(rows)
        requests = [event['body']['input'] for event in events()[begin:] if event['path'].endswith('/embeddings')]
        assert requests and any(len(batch) > 1 for batch in requests), 'No real multi-text request observed'
        assert all(isinstance(text, str) for batch in requests for text in batch)
        assert sorted(text for batch in requests for text in batch) == sorted(text for _, text in rows if text is not None)
        assert all(len(batch) <= 64 for batch in requests)
    check('Embed: true cross-row requests, reversed response mapping, NULL and request splitting', batch_embedding)

    def filtered_limit():
        output = sql('SELECT id, AI_EMBED(%s,content,7) FROM embed_rows WHERE id>=50 AND id<90 ORDER BY id LIMIT 17', (embed,))
        assert [row[0] for row in output] == list(range(50, 67))
        for key, vector in output:
            text = dict(rows)[key]
            assert (None if vector is None else json.loads(vector)) == (None if text is None else embedding(text, 7))
    check('Embed: WHERE/LIMIT preserve SQL row results', filtered_limit)

    def rerank_batches():
        documents = ['apple' if i % 3 == 0 else 'banana' for i in range(47)]
        wanted = sorted(range(len(documents)), key=lambda i: (-(i % 3 == 0), i))
        result = json.loads(value('SELECT AI_RERANK(%s,%s,%s,NULL)', (rerank, 'apple', json.dumps(documents))))
        assert [item['index'] for item in result] == wanted
        objects = [{'id': 'original-' + str(i), 'text': text, 'extra': i} for i, text in enumerate(documents)]
        result = json.loads(value('SELECT AI_RERANK(%s,%s,%s,%s)', (rerank, 'apple', json.dumps(objects), 'text')))
        assert [item['id'] for item in result] == ['original-' + str(i) for i in wanted]
        assert [item['extra'] for item in result] == wanted
        error_expected('SELECT AI_RERANK(%s,%s,%s,%s)', (rerank, 'apple', '[{"id":1}]', 'text'))
    check('Rerank: 47-document unequal batches, stable ties and original IDs', rerank_batches)

    for category, name, endpoint, suffix, faults in [
        ('Complete', complete, complete_ep, '/v1/chat/completions', ['http_error', 'invalid_json', 'empty_response', 'wrong_shape']),
        ('Embed', embed, embed_ep, '/v1/embeddings', ['http_error', 'invalid_json', 'nonnumeric', 'wrong_dimension', 'missing_result', 'empty_vector', 'wrong_shape']),
        ('Rerank', rerank, rerank_ep, '/v1/rerank', ['http_error', 'invalid_json', 'duplicate_index', 'missing_result', 'bad_score', 'out_of_range', 'wrong_shape']),
    ]:
        for fault in faults:
            def injected_failure(name=name, endpoint=endpoint, suffix=suffix, fault=fault, category=category):
                endpoint_url(endpoint, '/' + fault + suffix)
                try:
                    before = len(events())
                    if category == 'Complete':
                        error_expected('SELECT AI_COMPLETE(%s,%s)', (name, 'fault'))
                    elif category == 'Embed':
                        error_expected('SELECT AI_EMBED(%s,%s,7)', (name, 'fault'))
                    else:
                        error_expected('SELECT AI_RERANK(%s,%s,%s,NULL)', (name, 'apple', '["apple","banana"]'))
                    assert len(events()) > before, 'Fault case never reached Mock service'
                finally:
                    endpoint_url(endpoint, suffix)
            check(category + ': rejects ' + fault, injected_failure)

    for fault in ['duplicate_index', 'missing_index', 'mixed_dimension']:
        def batch_fault(fault=fault):
            endpoint_url(embed_ep, '/' + fault + '/v1/embeddings')
            try:
                error_expected('SELECT AI_EMBED(%s,content,7) FROM embed_rows ORDER BY id', (embed,))
            finally:
                endpoint_url(embed_ep, '/v1/embeddings')
        check('Embed batch: rejects ' + fault + ' and remains healthy', batch_fault)

    def network_failure():
        sql('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)', (complete_ep, '{"url":"http://127.0.0.1:1/v1/chat/completions"}'))
        try:
            error_expected('SELECT AI_COMPLETE(%s,%s)', (complete, 'connection failure'))
        finally:
            endpoint_url(complete_ep, '/v1/chat/completions')
    check('Network failure propagates and database remains healthy', network_failure)

    def ollama():
        endpoint_url(complete_ep, '/api/generate')
        sql('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)', (complete_ep, '{"provider":"oLlAmA","access_key":""}'))
        begin = len(events())
        sql('SELECT AI_COMPLETE(%s,%s,%s)', (complete, 'native completion', '{"temperature":0.3,"max_tokens":21}'))
        request = events()[begin:][-1]
        assert request['body']['stream'] is False and 'messages' not in request['body']
        assert request['body']['options']['temperature'] == 0.3
        assert request['body']['options']['num_predict'] == 21
        assert not request['authorization_present']
        endpoint_url(embed_ep, '/api/embed')
        sql('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)', (embed_ep, '{"provider":"OLLAMA","access_key":""}'))
        assert json.loads(value('SELECT AI_EMBED(%s,%s,7)', (embed, 'native'))) == embedding('native', 7)
        error_expected('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)', (embed_ep, '{"provider":"OPENAI"}'))
    check('Ollama: mixed-case provider, native protocols/options and empty key', ollama)

    def ordinary_sql():
        assert value('SELECT COUNT(*) FROM embed_rows') == 180
        assert value('SELECT SUM(id) FROM embed_rows') == sum(range(1, 181))
        assert value("SELECT JSON_UNQUOTE(JSON_EXTRACT(JSON_OBJECT('x','ok'),'$.x'))") == 'ok'
        assert sql('SELECT 2+3, NULL IS NULL') == ((5, 1),)
    check('Ordinary SQL: scan/aggregate/JSON/NULL remain correct', ordinary_sql)
finally:
    for table in created_tables:
        try:
            sql('DROP TABLE ' + table)
        except pymysql.MySQLError:
            pass
    for name, endpoint in reversed(created):
        for procedure, parameter in [('DROP_AI_MODEL_ENDPOINT', endpoint), ('DROP_AI_MODEL', name)]:
            try:
                sql('CALL DBMS_AI_SERVICE.' + procedure + '(%s)', (parameter,))
            except pymysql.MySQLError:
                pass
    connection.close()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    report = {'runner': 'supplemental SQL/protocol checks', 'pass': sum(r['status']=='PASS' for r in records),
              'fail': sum(r['status']=='FAIL' for r in records), 'cases': records}
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print('SUMMARY:', report['pass'], 'PASS;', report['fail'], 'FAIL', flush=True)
sys.exit(bool(report['fail']))
