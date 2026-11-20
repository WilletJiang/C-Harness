"""Public executable contracts against a deterministic Responses service, no secrets/network API."""
import http.server
from loopback import LoopbackServer
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading

CLI = sys.argv[1]
requests = []


class Service(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append(body)
        assert body['store'] is False and body['stream'] is True
        assert body['tools'][0]['type'] == 'namespace'
        items = body['input']
        outputs = [x for x in items if x.get('type') == 'function_call_output']
        model = body['model']
        if not outputs and model in ('read', 'write', 'ask'):
            name, args = {
                'read': ('read_file', {'path': 'source.txt'}),
                'write': ('write_file', {'path': 'created.txt', 'content': 'persisted result'}),
                'ask': ('ask', {'question': 'Which output?', 'options': ['short', 'long']}),
            }[model]
            output = [{'type': 'function_call', 'id': 'fc_1', 'call_id': 'call_1',
                       'namespace': 'local', 'name': name, 'arguments': json.dumps(args), 'status': 'completed'}]
        else:
            output = [{'type': 'message', 'id': 'msg_1', 'role': 'assistant', 'status': 'completed',
                       'content': [{'type': 'output_text', 'text': 'Verified result', 'annotations': []}]}]
        response = {'id': 'resp_1', 'status': 'completed', 'output': output,
                    'usage': {'input_tokens': 10, 'output_tokens': 5, 'total_tokens': 15}}
        event = {'type': 'response.completed', 'response': response}
        data = ('data: ' + json.dumps(event) + '\n\n').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


server = LoopbackServer(('127.0.0.1', 0), Service)
threading.Thread(target=server.serve_forever, daemon=True).start()
endpoint = f'http://127.0.0.1:{server.server_port}/responses'
env = dict(os.environ, AH_ACCESS_TOKEN='local-fixture-not-a-credential')

with tempfile.TemporaryDirectory(prefix='ah-cli-') as tmp:
    workspace = pathlib.Path(tmp) / 'workspace'
    workspace.mkdir()
    (workspace / 'source.txt').write_text('hello 世界')
    codex_home = pathlib.Path(tmp) / 'codex home'
    codex_home.mkdir()
    (codex_home / 'config.toml').write_text('Malformed configuration must remain unread: [')
    skill = codex_home / 'skills' / 'fixture'
    skill.mkdir(parents=True)
    skill_body = 'PRIVATE_SKILL_BODY_MUST_ONLY_LOAD_ON_DEMAND'
    (skill / 'SKILL.md').write_text(
        '---\nname: fixture\ndescription: Offline fixture\n---\n' + skill_body)
    env['CODEX_HOME'] = str(codex_home)
    base = ['--db', tmp + '/state.db', '--workspace', str(workspace),
            '--endpoint', endpoint, '--allow-http-loopback']

    def run(command, *args, ok=True):
        result = subprocess.run([CLI, command, *base, *args], env=env,
                                capture_output=True, text=True, timeout=10)
        if ok:
            assert result.returncode == 0, (command, result.stdout, result.stderr)
        else:
            assert result.returncode != 0, (command, result.stdout)
        assert 'local-fixture-not-a-credential' not in result.stdout + result.stderr
        lines = [json.loads(x) for x in result.stdout.splitlines() if x.strip()]
        return lines[-1] if lines else None

    plain = run('new', '--model', 'simple')['id']
    state = run('run', '--session', plain, '--prompt', 'Hello', '--command-id', 'u1')
    assert state['state'] == 'completed'
    names = {tool['name'] for tool in requests[-1]['tools'][0]['tools']}
    assert {'apply_changes', 'find', 'flow', 'test'} <= names
    assert not names.intersection({'list_directory', 'edit_file', 'search'})
    extension_names = {'discover', 'skill', 'mcp', 'mcp_call'}
    assert not names.intersection(extension_names), 'Installed extensions must be opt-in'
    before = len(requests)
    run('run', '--session', plain, '--prompt', 'Hello', '--command-id', 'u1')
    assert len(requests) == before, 'Duplicate command must not execute again'
    run('run', '--session', plain, '--prompt', 'Changed', '--command-id', 'u1', ok=False)
    read = run('new', '--model', 'read')['id']
    assert run('run', '--session', read, '--prompt', 'Read source', '--command-id', 'r1', '--workflows')['state'] == 'completed'
    names = {tool['name'] for tool in requests[-1]['tools'][0]['tools']}
    assert 'find' in names and 'apply_changes' in names
    assert not names.intersection({'list_directory', 'edit_file', 'search'})
    assert 'hello' in json.dumps(requests[-1]['input'])
    write = run('new', '--model', 'write')['id']
    state = run('run', '--session', write, '--prompt', 'Write file', '--command-id', 'w1')
    assert state['state'] == 'waiting_approval' and not (workspace / 'created.txt').exists()
    operation = state['pending']['operation_id']
    run('approve', '--session', write, '--operation', operation)
    assert run('run', '--session', write)['state'] == 'completed'
    assert (workspace / 'created.txt').read_text() == 'persisted result'
    ask = run('new', '--model', 'ask')['id']
    state = run('run', '--session', ask, '--prompt', 'Ask me', '--command-id', 'a1')
    assert state['state'] == 'waiting_input'
    operation = state['pending']['operation_id']
    run('reply', '--session', ask, '--operation', operation, '--value', '"short"')
    assert run('run', '--session', ask)['state'] == 'completed'
    forked = run('fork', '--session', plain)['id']
    assert forked != plain
    run('compact', '--session', plain, '--summary', 'A prior greeting completed.', '--keep-turns', '0')
    assert run('inspect', '--session', plain)['state'] == 'completed'
    run('events', '--session', plain)
    assert len(run('list')) >= 5
    # RPC framing, IDs, unknown methods, and complete requests without a model call.
    script = '\n'.join(json.dumps(x) for x in [
        {'id': 'c', 'method': 'create', 'model': 'simple'},
        {'id': 2, 'method': 'list'}, {'id': 3, 'method': 'unknown'}]) + '\n'
    p = subprocess.run([CLI, 'rpc', *base], input=script, capture_output=True, text=True, env=env, timeout=10)
    assert p.returncode == 0, p.stderr
    replies = {x['id']: x for x in map(json.loads, p.stdout.splitlines()) if 'id' in x}
    assert replies['c']['status'] == 'ok' and replies[2]['status'] == 'ok'
    assert replies[3]['status'] == 'invalid'

    # Configuration paths alone do not enable features; absent paths stay inert.
    missing_home = str(pathlib.Path(tmp) / 'does not exist')
    disabled = run('new', '--model', 'simple', '--codex-home', missing_home)['id']
    run('run', '--session', disabled, '--prompt', 'No extensions', '--command-id', 'd1',
        '--codex-home', missing_home, '--extension-context-bytes', '1024')
    names = {tool['name'] for tool in requests[-1]['tools'][0]['tools']}
    assert not names.intersection(extension_names)

    # Each feature registers only its small entrypoints, without parsing config
    # or loading installed skill bodies. Opt-in is required again on reopen.
    cases = [
        (['--skills'], {'discover', 'skill'}),
        (['--mcp'], {'discover', 'mcp', 'mcp_call'}),
        (['--plugins'], {'discover'}),
        (['--skills', '--mcp'], extension_names),
        (['--plugins', '--skills'], {'discover', 'skill'}),
        (['--plugins', '--mcp'], {'discover', 'mcp', 'mcp_call'}),
        (['--skills', '--mcp', '--plugins'], extension_names),
    ]
    for flags, expected in cases:
        enabled = run('new', '--model', 'simple', '--codex-home', str(codex_home), *flags)['id']
        run('run', '--session', enabled, '--prompt', 'Enabled explicitly', '--command-id', 'e1',
            '--codex-home', str(codex_home), '--extension-context-bytes', '1024', *flags)
        names = {tool['name'] for tool in requests[-1]['tools'][0]['tools']}
        assert names.intersection(extension_names) == expected, (flags, names)
        assert skill_body not in json.dumps(requests[-1]), 'Skill bodies must load on demand'
        run('run', '--session', enabled, '--prompt', 'Reopened without opt-in', '--command-id', 'e2')
        names = {tool['name'] for tool in requests[-1]['tools'][0]['tools']}
        assert not names.intersection(extension_names), 'Flags must not persist in the database'

    for invalid in ('', '0', '1023', '-1024', ' -1024', '+1024', ' 1024', 'bad', '1024x', str(2**128)):
        run('new', '--model', 'simple', '--extension-context-bytes', invalid, ok=False)
    run('new', '--model', 'simple', '--extension-context-bytes', ok=False)
    run('new', '--model', 'simple', '--codex-home', ok=False)

server.shutdown()
server.server_close()
print('CLI integration passed: HTTP tools, approval/restart, input, dedupe, fork, compaction, RPC, extension opt-in')
