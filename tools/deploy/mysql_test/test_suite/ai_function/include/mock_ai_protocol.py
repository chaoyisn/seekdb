#!/usr/bin/env python3
"""Supplemental protocol fixture, NOT the unavailable official AI function Mock.

No model is loaded. Responses depend on ordinary protocol fields, never on a
competition model name. Run beside the test database on port 8000.
"""
import argparse
import hashlib
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import threading
import socket
from urllib.parse import urlparse

parser = argparse.ArgumentParser()
parser.add_argument('--host', default='0.0.0.0')
parser.add_argument('--port', type=int, default=8000)
parser.add_argument('--dimension', type=int, default=768)
parser.add_argument('--log', default='/tmp/ai-protocol-events.jsonl')
args = parser.parse_args()
events = []
lock = threading.Lock()

def embedding(text, dimension):
    seed = hashlib.sha256(text.encode('utf-8')).digest()
    return [round((seed[i % len(seed)] - 128) / 128.0, 6) for i in range(dimension)]

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *unused):
        pass
    def reply(self, status, value):
        payload = json.dumps(value, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)
    def do_GET(self):
        if self.path == '/__stats':
            with lock:
                self.reply(200, {'requests': list(events)})
        else:
            self.reply(404, {'error': 'unknown endpoint'})
    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', '0'))))
            path = urlparse(self.path).path
            event = {'path': path, 'body': body, 'authorization_present': bool(self.headers.get('Authorization'))}
            with lock:
                events.append(event)
                with open(args.log, 'a', encoding='utf-8') as stream:
                    stream.write(json.dumps(event, ensure_ascii=False) + '\n')
            if '/partial_retry/' in path:
                with lock:
                    request_count = sum(item['path'] == path for item in events)
                if request_count % 2:
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', '10000')
                    self.end_headers()
                    self.wfile.write(b'{"incomplete":')
                    self.wfile.flush()
                    self.close_connection = True
                    self.connection.shutdown(socket.SHUT_RDWR)
                    self.connection.close()
                    return
            if '/wrong_shape/' in path:
                self.reply(200, [])
                return
            if '/http_error/' in path:
                self.reply(400, {'error': {'message': 'injected HTTP failure'}})
                return
            if '/invalid_json/' in path:
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{invalid')
                return
            if path.endswith('/chat/completions') or path.endswith('/api/generate'):
                ollama = path.endswith('/api/generate')
                if ollama:
                    if body.get('stream') is not False:
                        raise ValueError('Ollama completion must explicitly disable streaming')
                    content = body['prompt']
                else:
                    content = '\n'.join(m['content'] for m in body['messages'])
                if not isinstance(content, str) or not content:
                    raise ValueError('prompt must be a nonempty string')
                result = '' if '/empty_response/' in path else 'mock:' + content
                self.reply(200, {'response': result, 'done': True} if ollama else {'choices': [{'message': {'content': result}}]})
            elif path.endswith('/embeddings') or path.endswith('/api/embed'):
                values = body['input']
                if isinstance(values, str):
                    values = [values]
                if not values or any(not isinstance(v, str) for v in values):
                    raise ValueError('input must contain strings')
                dimension = body.get('dimensions', args.dimension)
                if not isinstance(dimension, int) or isinstance(dimension, bool) or not 0 < dimension <= 65536:
                    raise ValueError('invalid embedding dimension')
                vectors = [embedding(v, dimension) for v in values]
                if '/wrong_dimension/' in path:
                    vectors[0] = vectors[0][:-1]
                if '/empty_vector/' in path:
                    vectors[0] = []
                if '/mixed_dimension/' in path and len(vectors) > 1:
                    vectors[1] = vectors[1][:-1]
                if '/nonnumeric/' in path:
                    vectors[0][0] = 'invalid'
                if path.endswith('/api/embed'):
                    self.reply(200, {'embeddings': vectors})
                else:
                    data = [{'index': i, 'embedding': v} for i, v in enumerate(vectors)]
                    if '/duplicate_index/' in path and len(data) > 1:
                        data[1]['index'] = 0
                    if '/missing_index/' in path:
                        data[0].pop('index')
                    if '/missing_result/' in path:
                        data.pop()
                    self.reply(200, {'data': list(reversed(data))})
            elif path.endswith('/rerank'):
                documents = body['documents']
                query = set(body['query'].lower().split())
                data = [{'index': i, 'relevance_score': float(len(query & set(v.lower().split())))} for i, v in enumerate(documents)]
                data.sort(key=lambda item: (-item['relevance_score'], item['index']))
                if '/bad_score/' in path:
                    data[0]['relevance_score'] = 'not-a-number'
                if '/out_of_range/' in path:
                    data[0]['index'] = len(data)
                if '/duplicate_index/' in path and len(data) > 1:
                    data[1]['index'] = data[0]['index']
                if '/missing_result/' in path:
                    data.pop()
                self.reply(200, {'results': list(reversed(data))})
            else:
                self.reply(404, {'error': 'unknown protocol endpoint'})
        except (ValueError, KeyError, TypeError) as error:
            self.reply(400, {'error': {'message': str(error)}})

Path(args.log).parent.mkdir(parents=True, exist_ok=True)
ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
