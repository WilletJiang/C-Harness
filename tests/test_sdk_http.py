"""Installed SDK transport, image tools and Ask with synchronous/async providers."""
import base64
import http.server
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from loopback import LoopbackServer

PNG = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aXioAAAAASUVORK5CYII='
requests = []
failures = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            index = len(requests)
            requests.append(body)
            assert self.headers['Authorization'] == ('Bearer renewed' if index == 2 else 'Bearer initial')
            tools = body['tools'][0]['tools'] if 'tools' in body['tools'][0] else body['tools'][0]['functions']
            names = {t['name'] for t in tools}
            assert 'view_image' in names and not names.intersection({'exec','mcp','skill'})
            outputs = [item for item in body['input'] if item.get('type') == 'function_call_output']
            if index == 0:
                assert not outputs
                output = [{'type':'function_call','name':'view_image','call_id':f'image-{n}',
                           'arguments':json.dumps({'path':'image.bin','detail':detail})}
                          for n,detail in enumerate(('auto','high'))]
            else:
                assert len(outputs) == (2 if index == 1 else 3)
                for item, detail in zip(outputs[:2], ('auto','high')):
                    assert isinstance(item['output'],list)
                    image = next(p for p in item['output'] if p['type']=='input_image')
                    assert image['image_url'] == 'data:image/png;base64,'+PNG
                    assert image['detail']==detail
                    assert 'artifact_id' not in image
                if index == 1:
                    output = [{'type':'function_call','name':'ask','call_id':'ask-1',
                               'arguments':'{"question":"Continue?"}'}]
                else:
                    assert index == 2
                    assert json.loads(outputs[-1]['output'])['result'] is True
                    output = []
            for i,item in enumerate(output):
                item.update(id=f'tool-{index}-{i}',namespace='local',status='completed')
            rows = [{'type':'response.output_item.done','output_index':i,'item':item}
                    for i,item in enumerate(output)]
            rows.append({'type':'response.completed','response':{'id':f'response-{index}',
                          'status':'completed','output':[]}})
            wire = ''.join('data: '+json.dumps(row)+'\n\n' for row in rows).encode()
            self.send_response(200)
            self.send_header('Content-Type','text/event-stream')
            self.send_header('Content-Length',str(len(wire)))
            self.end_headers()
            self.wfile.write(wire)
        except Exception as error:
            failures.append(repr(error))
            self.send_error(500)


def main(binary):
    server = LoopbackServer(('127.0.0.1',0),Handler)
    thread = threading.Thread(target=server.serve_forever,daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='ah-sdk-http-') as temporary:
            workspace = Path(temporary)
            (workspace/'image.bin').write_bytes(base64.b64decode(PNG))
            for mode in ('sync','async'):
                requests.clear()
                result = subprocess.run([binary,f'http://127.0.0.1:{server.server_port}/responses',
                                         str(workspace),mode],capture_output=True,text=True,timeout=20)
                assert result.returncode==0, (mode,result.stderr,failures)
                assert len(requests)==3 and not failures
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__=='__main__':
    main(sys.argv[1])
