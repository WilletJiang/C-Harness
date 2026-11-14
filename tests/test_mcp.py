"""Local-only protocol fixtures. Also acts as the subprocess MCP peer."""
import http.server
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path


def initialization(version="2025-11-25", capabilities=None):
    return {"protocolVersion": version,
            "capabilities": capabilities if capabilities is not None else {
                "tools": {}, "resources": {}, "prompts": {}},
            "serverInfo": {"name": "fixture", "version": "1"}}


def response(request, result):
    return {"jsonrpc": "2.0", "id": request["id"], "result": result}


def stdio_peer(mode, marker):
    count = 0
    if mode == "timeout_once" and Path(marker + ".once").exists():
        mode = "echo"
    Path(marker).write_text(str(os.getpid()))
    if mode == "descendant":
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        Path(marker + ".child").write_text(str(child.pid))

        def stop_group(signum, frame):
            child.wait(timeout=1)
            sys.exit(0)

        signal.signal(signal.SIGTERM, stop_group)

    def send(value, split=False):
        data = (json.dumps(value) + "\n").encode()
        if split:
            for start in range(0, len(data), 13):
                os.write(1, data[start:start + 13])
        else:
            os.write(1, data)

    for line in sys.stdin:
        request = json.loads(line)
        method = request.get("method")
        if method == "initialize":
            if mode == "startup_timeout":
                time.sleep(10)
            if mode == "stderr":
                os.write(2, b"SECRET_DO_NOT_PRINT" * 20000)
            version = "2099-01-01" if mode == "version" else "2025-11-25"
            send(response(request, initialization(version, {} if mode == "capability" else None)), True)
        elif method == "notifications/initialized":
            continue
        elif method:
            count += 1
            if mode == "timeout_once" and count > 1:
                Path(marker + ".once").touch()
                time.sleep(10)
            if mode == "timeout" or (mode == "timeout_after_first" and count > 1):
                time.sleep(10)
            if mode == "exit":
                return
            if mode == "large":
                send(response(request, {"text": "x" * 10000}))
            elif mode == "mismatch":
                send({"jsonrpc": "2.0", "id": request["id"] + 1, "result": {}})
            elif mode == "malformed":
                os.write(1, b"not-json\n")
            elif mode == "error":
                send({"jsonrpc": "2.0", "id": request["id"],
                      "error": {"code": -32602, "message": "SECRET_DO_NOT_PRINT"}})
            else:
                send({"jsonrpc": "2.0", "method": "notifications/progress", "params": {}})
                if mode == "request":
                    send({"jsonrpc": "2.0", "id": "unsupported", "method": "sampling/createMessage"})
                    answer = json.loads(sys.stdin.readline())
                    assert answer["id"] == "unsupported" and answer["error"]["code"] == -32601
                    send({"jsonrpc": "2.0", "id": 42, "method": "ping"})
                    answer = json.loads(sys.stdin.readline())
                    assert answer["id"] == 42 and answer["result"] == {}
                send(response(request, {"count": count, "pid": os.getpid(),
                                        "env": os.getenv("MCP_FIXTURE_SECRET"),
                                        "cwd": os.getcwd()}), True)


if len(sys.argv) > 1 and sys.argv[1] == "--stdio":
    stdio_peer(sys.argv[2], sys.argv[3])
    sys.exit(0)


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        pass  # Broken connections are intentional cancellation cases.


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_POST(self):
        server = self.server
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        method = request.get("method")
        with server.lock:
            server.requests.append((request, dict(self.headers)))
            server.ports.append(self.client_address[1])
        if server.mode == "auth":
            self.send_response(401)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if server.mode == "redirect":
            self.send_response(307)
            self.send_header("Location", server.endpoint + "/redirected")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if method == "initialize":
            version = {"legacy": "2025-03-26", "legacy_june": "2025-06-18"}.get(server.mode, "2025-11-25")
            payload = response(request, initialization(version))
        elif method == "notifications/initialized" or not method:
            if not method:
                assert request["error"]["code"] == -32601
                server.replied.set()
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        else:
            with server.lock:
                server.calls += 1
                count = server.calls
            if server.mode == "timeout" or (server.mode == "timeout_after_first" and count > 1):
                time.sleep(2)
            if server.mode == "drop":
                self.connection.shutdown(socket.SHUT_RDWR)
                self.close_connection = True
                return
            payload = response(request, {"count": count})
            if server.mode == "large":
                payload["result"]["text"] = "x" * 10000
            if server.mode == "mismatch" or (server.mode == "mismatch_once" and count == 1):
                payload["id"] += 1
            if server.mode == "error":
                payload = {"jsonrpc": "2.0", "id": request["id"],
                           "error": {"code": -32602, "message": "SECRET_DO_NOT_PRINT"}}
        if server.mode in {"sse", "sse_open", "server_request"} and method != "initialize":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b": keepalive\r\nevent: message\r\n")
            self.wfile.write(b"id: priming\r\ndata:\r\n\r\n")
            if server.mode == "server_request":
                self.wfile.write(b'data: {"jsonrpc":"2.0","id":"remote","method":"sampling/createMessage"}\n\n')
                self.wfile.flush()
                assert server.replied.wait(2)
            notice = {"jsonrpc": "2.0", "method": "notifications/progress", "params": {}}
            stream = ("data: " + json.dumps(notice) + "\r\n\r\n" +
                      "data: " + json.dumps(payload) + "\n\n").encode()
            for i in range(0, len(stream), 7):
                self.wfile.write(stream[i:i + 7])
                self.wfile.flush()
            if server.mode == "sse_open":
                time.sleep(2)
            self.close_connection = True
        else:
            data = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            if method == "initialize":
                self.send_header("MCP-Session-Id", "fixture-session")
            self.end_headers()
            self.wfile.write(data)


class Transport(unittest.TestCase):
    binary = None

    def run_client(self, config, requests=None, limit=1024 * 1024, env=None):
        plan = {"config": config, "requests": requests if requests is not None else [
            {"method": "tools/list"}], "limit": limit}
        run = subprocess.run([self.binary], input=json.dumps(plan) + "\n", text=True,
                             capture_output=True, timeout=8, env=env)
        self.assertEqual(run.returncode, 0, run.stderr + run.stdout)
        self.assertNotIn("SECRET_DO_NOT_PRINT", run.stdout + run.stderr)
        self.assertEqual(run.stderr, "")
        return [json.loads(line) for line in run.stdout.splitlines()]

    def stdio(self, mode="echo", requests=None, limit=1024 * 1024, extra=None):
        with tempfile.TemporaryDirectory(prefix="ah-mcp-test-") as tmp:
            marker = Path(tmp) / "pid"
            config = {"command": sys.executable, "args": [__file__, "--stdio", mode, str(marker)]}
            config.update(extra or {})
            result = self.run_client(config, requests, limit)
            if marker.exists():
                pid = int(marker.read_text())
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
            child_marker = Path(str(marker) + ".child")
            if child_marker.exists():
                pid = int(child_marker.read_text())
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
            return result

    def http(self, mode, requests=None, limit=1024 * 1024, extra=None, env=None):
        server = Server(("127.0.0.1", 0), Handler)
        server.mode, server.requests, server.calls, server.ports = mode, [], 0, []
        server.lock, server.replied = threading.Lock(), threading.Event()
        server.endpoint = f"http://127.0.0.1:{server.server_port}/mcp"
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            result = self.run_client({"url": server.endpoint, **(extra or {})}, requests, limit, env)
            if mode == "json" and len(server.ports) > 1:
                self.assertEqual(len(set(server.ports)), 1, "HTTP connections must be reused")
            return result, server.requests
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_inert_construction(self):
        result = self.stdio(requests=[])
        self.assertEqual(result, [{"open": 0}])
        result, seen = self.http("json", requests=[])
        self.assertEqual(result, [{"open": 0}])
        self.assertEqual(seen, [])
        result = self.stdio(requests=[{"method": "tools/list", "cancelled": True}])
        self.assertEqual((result[1]["code"], result[1]["sent"]), (5, False))
        result, seen = self.http("json", [{"method": "tools/list", "cancelled": True}])
        self.assertEqual((result[1]["code"], result[1]["sent"]), (5, False))
        self.assertEqual(seen, [])

    def test_stdio_persistent_split_notifications_and_server_requests(self):
        for mode in ["echo", "stderr", "request", "descendant"]:
            with self.subTest(mode=mode):
                result = self.stdio(mode, [{"method": "tools/list"}, {"method": "resources/list"},
                                           {"method": "prompts/list"}])
                self.assertTrue(all(row["code"] == 0 and row["sent"] for row in result[1:]), result)
                self.assertEqual([row["result"]["count"] for row in result[1:]], [1, 2, 3])
                self.assertEqual(len({row["result"]["pid"] for row in result[1:]}), 1)

    def test_stdio_errors_and_bounds(self):
        for mode, code in [("large", 4), ("mismatch", 3), ("malformed", 3), ("exit", 2)]:
            with self.subTest(mode=mode):
                result = self.stdio(mode, limit=2048)[1]
                self.assertEqual(result["code"], code, result)
                self.assertTrue(result["sent"])
        result = self.stdio("error")[1]
        self.assertEqual(result["code"], 0)
        self.assertTrue(result["result"]["isError"])
        for mode in ["version", "capability"]:
            result = self.stdio(mode)[1]
            self.assertEqual(result["code"], 3)
            self.assertFalse(result["sent"])

    def test_stdio_deadline_cancel_and_reconnect(self):
        for mode, request, expected, sent in [
                ("timeout_after_first", {"deadline_ms": 100}, 4, True),
                ("timeout_after_first", {"cancel_after_ms": 80}, 5, True),
                ("startup_timeout", {"deadline_ms": 100}, 4, False)]:
            with self.subTest(mode=mode, request=request):
                requests = [{"method": "tools/list"}] if mode == "timeout_after_first" else []
                result = self.stdio(mode, requests + [{"method": "tools/list", **request}])[-1]
                self.assertEqual((result["code"], result["sent"]), (expected, sent), result)
                self.assertLess(result["elapsed"], 1000)
                self.assertGreater(result["yields"], 0)
        result = self.stdio("mismatch", [{"method": "tools/list"}, {"method": "tools/list"}])
        self.assertEqual([row["code"] for row in result[1:]], [3, 3])
        result = self.stdio("timeout_once", [{"method": "tools/list"},
                                             {"method": "tools/list", "deadline_ms": 100},
                                             {"method": "tools/list"}])
        self.assertEqual([row["code"] for row in result[1:]], [0, 4, 0])
        self.assertEqual(result[3]["result"]["count"], 1)

    def test_process_configuration(self):
        result = self.stdio(extra={"cwd": "/tmp", "env": {"MCP_FIXTURE_SECRET": "configured"}})[1]
        self.assertEqual(result["result"]["env"], "configured")
        self.assertEqual(os.path.realpath(result["result"]["cwd"]), os.path.realpath("/tmp"))
        result = self.stdio(extra={"env_vars": ["AH_NONEXISTENT_MCP_TEST_VARIABLE"]})[1]
        self.assertEqual((result["code"], result["sent"]), (9, False))

    def test_http_json_sse_persistent_session(self):
        for mode in ["json", "sse", "sse_open", "server_request"]:
            with self.subTest(mode=mode):
                result, seen = self.http(mode, [{"method": "tools/list"}, {"method": "resources/list"}])
                self.assertEqual([row["code"] for row in result[1:]], [0, 0], result)
                self.assertEqual([row["result"]["count"] for row in result[1:]], [1, 2])
                self.assertEqual(sum(row[0].get("method") == "initialize" for row in seen), 1)
                self.assertTrue(all(headers["MCP-Session-Id"] == "fixture-session" for _, headers in seen[1:]))
                self.assertTrue(all(headers["MCP-Protocol-Version"] == "2025-11-25" for _, headers in seen))
                self.assertLess(result[1]["elapsed"], 1000)

    def test_http_failures(self):
        for mode, code, sent in [("large", 4, True), ("mismatch", 3, True),
                                 ("auth", 9, False), ("redirect", 10, False)]:
            with self.subTest(mode=mode):
                result, seen = self.http(mode, limit=2048)
                self.assertEqual((result[1]["code"], result[1]["sent"]), (code, sent), result)
                if mode == "redirect":
                    self.assertEqual(len(seen), 1)
        result, _ = self.http("error")
        self.assertEqual(result[1]["code"], 0)
        self.assertTrue(result[1]["result"]["isError"])
        result, seen = self.http("mismatch_once", [{"method": "tools/list"}, {"method": "tools/list"}])
        self.assertEqual([row["code"] for row in result[1:]], [3, 0], result)
        self.assertEqual(sum(row[0].get("method") == "tools/list" for row in seen), 2)
        self.assertEqual(sum(row[0].get("method") == "initialize" for row in seen), 2)
        result, seen = self.http("drop", [{"method": "tools/call", "params": {"name": "effect"}}])
        self.assertEqual((result[1]["code"], result[1]["sent"]), (10, True), result)
        self.assertEqual(sum(row[0].get("method") == "tools/call" for row in seen), 1)

    def test_legacy_version_negotiation(self):
        for mode, version in [("legacy", "2025-03-26"), ("legacy_june", "2025-06-18")]:
            result, seen = self.http(mode)
            self.assertEqual(result[1]["code"], 0)
            self.assertEqual(seen[0][1]["MCP-Protocol-Version"], "2025-11-25")
            self.assertTrue(all(headers["MCP-Protocol-Version"] == version for _, headers in seen[1:]))

    def test_http_deadline_and_cancellation(self):
        for request, expected in [({"deadline_ms": 120}, 4), ({"cancel_after_ms": 80}, 5)]:
            result, seen = self.http("timeout_after_first", [{"method": "tools/list"},
                                                             {"method": "tools/list", **request}])
            self.assertEqual(result[-1]["code"], expected, result)
            self.assertTrue(result[-1]["sent"])
            self.assertLess(result[-1]["elapsed"], 500)
            self.assertGreater(result[-1]["yields"], 0)
            self.assertEqual(sum(row[0].get("method") == "tools/list" for row in seen), 2)

    def test_http_credentials_and_invalid_configuration(self):
        env = {**os.environ, "MCP_TEST_TOKEN": "SECRET_DO_NOT_PRINT", "MCP_TEST_HEADER": "custom"}
        result, seen = self.http("json", extra={"bearer_token_env_var": "MCP_TEST_TOKEN",
            "http_headers": {"X-Static": "static"}, "env_http_headers": {"X-Env": "MCP_TEST_HEADER"}}, env=env)
        self.assertEqual(result[1]["code"], 0)
        self.assertTrue(all(headers["Authorization"] == "Bearer SECRET_DO_NOT_PRINT" for _, headers in seen))
        self.assertEqual(seen[-1][1]["X-Static"], "static")
        self.assertEqual(seen[-1][1]["X-Env"], "custom")
        for config, code in [
            ({}, 1), ([], 1), ({"command": 3, "url": "https://example.com"}, 1),
            ({"url": "http://example.com/mcp"}, 1),
            ({"url": "https://user:password@example.com/mcp"}, 1),
            ({"url": "https://example.com/mcp", "http_headers": {"Host": "evil"}}, 1),
            ({"url": "https://example.com/mcp", "http_headers": {"X-Key": "bad\r\nvalue"}}, 1),
            ({"url": "https://example.com/mcp", "protocol_version": "2026-07-28"}, 3),
            ({"url": "https://example.com/mcp", "requires_openai_auth": True}, 9)]:
            with self.subTest(config=config):
                self.assertEqual(self.run_client(config, [])[0]["open"], code)


if __name__ == "__main__":
    Transport.binary = sys.argv.pop(1)
    unittest.main()
