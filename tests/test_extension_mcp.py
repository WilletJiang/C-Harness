"""MCP catalog/dispatch/recovery through the public CLI, using only loopback services."""
import http.server
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading

CLI = sys.argv[1]
wire = []
model_requests = []
effects = []
SCHEMA_MARKER = "ONLY_SELECTED_SCHEMA_193"


class Service(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def respond(self, body, content_type="application/json"):
        data = body.encode() if isinstance(body, str) else json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        if self.path == "/mcp":
            self.send_header("Mcp-Session-Id", "fixture-session")
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == "/responses":
            self.model(request)
            return
        assert self.path == "/mcp"
        assert self.headers["X-Fixture"] == "private-config-sentinel"
        method = request["method"]
        wire.append(method)
        if method == "notifications/initialized":
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if method == "initialize":
            result = {"protocolVersion": "2025-11-25", "capabilities": {"tools": {}},
                      "serverInfo": {"name": "fixture", "version": "1"}}
        elif method == "tools/list":
            result = {"tools": [
                {"name": "echo", "description": "Echo a fixture value",
                 "inputSchema": {"type": "object", "properties": {
                     "value": {"type": "string", "description": SCHEMA_MARKER}}}},
                {"name": "blocked", "description": "Disabled by local configuration",
                 "inputSchema": {"type": "object"}}]}
        elif method == "tools/call":
            assert request["params"]["name"] == "echo", request
            value = request["params"]["arguments"]["value"]
            effects.append(value)
            if value == "uncertain":
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                return
            result = {"content": [{"type": "text", "text": value}], "isError": False}
        else:
            raise AssertionError(method)
        self.respond({"jsonrpc": "2.0", "id": request["id"], "result": result})

    def model(self, request):
        model_requests.append(request)
        assert "private-config-sentinel" not in json.dumps(request)
        receipts = [json.loads(item["output"]) for item in request["input"]
                    if item.get("type") == "function_call_output"]
        step = len(receipts)
        mode = request["model"]
        server = receipts[0]["result"]["items"][0]["id"] if receipts else None
        if step == 0:
            tool, args = "discover", {"kind": "mcp", "query": "fixture"}
        elif step == 1:
            tool, args = "mcp", {"server": server, "action": "tools"}
        elif step == 2:
            assert SCHEMA_MARKER not in json.dumps(request)
            assert [x["name"] for x in receipts[1]["result"]["items"]] == ["echo"]
            tool, args = "mcp", {"server": server, "action": "describe", "name": "echo"}
        elif step == 3:
            assert SCHEMA_MARKER in json.dumps(request)
            tool, args = "mcp", {"server": server, "action": "tools"}
        elif step == 4:
            assert receipts[3]["result"]["reused"] is True
            tool, args = "mcp_call", {"server": server,
                                      "name": "blocked" if mode == "blocked" else "echo",
                                      "arguments": {"value": mode}}
        else:
            assert step == 5
            assert receipts[-1]["is_error"] is (mode == "blocked")
            tool = None
        output = ([{"type": "function_call", "name": tool, "namespace": "local",
                    "call_id": f"call-{step}", "arguments": json.dumps(args)}] if tool else
                  [{"type": "message", "role": "assistant", "content": [
                      {"type": "output_text", "text": "verified"}]}])
        event = {"type": "response.completed", "response": {"status": "completed", "output": output}}
        self.respond("data: " + json.dumps(event) + "\n\n", "text/event-stream")


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Service)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix="ah-extension-mcp-") as directory:
        root = pathlib.Path(directory)
        home = root / "codex"
        home.mkdir()
        (home / "config.toml").write_text(
            '[mcp_servers.fixture]\n'
            f'url = "http://127.0.0.1:{server.server_port}/mcp"\n'
            'disabled_tools = ["blocked"]\n'
            '[mcp_servers.fixture.http_headers]\n'
            'X-Fixture = "private-config-sentinel"\n')
        env = dict(os.environ, AH_ACCESS_TOKEN="fixture-model-token")
        base = ["--db", str(root / "state.db"), "--mcp", "--codex-home", str(home),
                "--endpoint", f"http://127.0.0.1:{server.server_port}/responses",
                "--allow-http-loopback"]

        def run(command, *args, success=True):
            result = subprocess.run([CLI, command, *base, *args], env=env,
                                    text=True, capture_output=True, timeout=15)
            assert (result.returncode == 0) == success, (result.stdout, result.stderr)
            assert "private-config-sentinel" not in result.stdout + result.stderr
            records = [json.loads(line) for line in result.stdout.splitlines()]
            return records[-1] if records else None

        for mode in ("normal", "blocked", "uncertain"):
            before = len(wire)
            sid = run("new", "--model", mode)["id"]
            assert len(wire) == before, "Enable/new must not connect to any MCP server"
            state = run("run", "--session", sid, "--command-id", "task", "--prompt", "Use fixture")
            assert state["state"] == "waiting_approval"
            assert wire[before:].count("tools/list") == 1, "Metadata cache must avoid serial refetches"
            assert mode not in effects, "External call must wait for approval"
            operation = state["pending"]["operation_id"]
            run("approve", "--session", sid, "--operation", operation)
            state = run("run", "--session", sid, success=mode != "uncertain")
            if mode != "uncertain":
                assert state["state"] == "completed"
                assert effects.count(mode) == (0 if mode == "blocked" else 1)
            else:
                state = run("inspect", "--session", sid)
                assert state["state"] == "outcome_unknown"
                count = len(effects)
                run("run", "--session", sid, success=False)
                assert len(effects) == count, "Uncertain request must never be replayed"
                run("resolve", "--session", sid, "--operation", operation,
                    "--value", '{"reconciled":true}')
                assert run("run", "--session", sid)["state"] == "completed"
                assert effects.count("uncertain") == 1
        assert all("private-config-sentinel" not in json.dumps(x) for x in model_requests)
finally:
    server.shutdown()
    server.server_close()
print("Extension MCP integration passed: lazy connection, schema paging, cache, approval, policy, unknown recovery")
