"""Supplemental 500-once HTTP test, absent from the official 400-error mock."""
import argparse,json,os,subprocess,threading
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('--binary',required=True);p.add_argument('--report',type=Path,required=True)
args=p.parse_args();counts={};lock=threading.Lock()
class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length','0')))
        with lock:
            counts[self.path]=counts.get(self.path,0)+1
            attempt=counts[self.path]
        payload=json.dumps({'attempt':attempt}).encode()
        self.send_response(500 if attempt==1 else 200)
        self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(payload)))
        self.end_headers();self.wfile.write(payload)
    def log_message(self,*unused):pass
server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
env=dict(os.environ);env['AI_COLUMN_TRANSPORT_TEST_PORT']=str(server.server_port)
try:
    run=subprocess.run([args.binary,'--gtest_filter=AiColumnTransport.*','--gtest_color=no'],
                       env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=40)
    expected={'/guarded':1,'/normal':2,'/nested_a':1,'/nested_b':1,'/after_scope':2}
    ok=run.returncode==0 and counts==expected
    result={'kind':'supplemental native-client HTTP 500 test','exit_code':0 if ok else 1,
            'native_test_exit_code':run.returncode,'actual_requests':counts,'expected_requests':expected,
            'stdout':run.stdout,'official_mock_modified':False}
    args.report.write_text(json.dumps(result,indent=2));print(run.stdout);print(json.dumps(result,indent=2))
finally:
    server.shutdown();server.server_close();thread.join()
raise SystemExit(result['exit_code'])
