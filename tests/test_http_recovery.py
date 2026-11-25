"""Inject HTTP, SSE and TLS faults at the RPC and tool-admission boundaries."""
from collections import defaultdict
from email.utils import formatdate
from http.server import BaseHTTPRequestHandler
import json
import math
import os
from pathlib import Path
import socket
import socketserver
import ssl
import subprocess
from tempfile import TemporaryDirectory
import threading
import time

from loopback import LoopbackServer
from test_http import DELTA, EMPTY_TERMINAL, event
from test_rpc import Client


def call(name, call_id, arguments):
    return event({"type": "response.completed", "response": {"status": "completed", "output": [
        {"type": "function_call", "namespace": "local", "name": name,
         "call_id": call_id, "arguments": json.dumps(arguments)}
    ]}})


class Service(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        requests = self.server.requests[self.path]
        requests.append((body, self.headers.get("Authorization"), time.monotonic()))
        attempt = len(requests)
        mode = self.path[1:]
        if mode in ("date", "numeric", "zero", "invalid", "huge") and attempt == 1:
            hint = {"date": formatdate(math.ceil(time.time()) + 1, usegmt=True),
                    "numeric": "1", "zero": "0", "invalid": "2seconds", "huge": "9" * 30}[mode]
            return self.output(503, b"{}", retry_after=hint)
        if mode in ("quota", "permission", "permanent", "same-token") or (
                mode in ("rotate", "rotate-rejected") and (attempt == 1 or mode == "rotate-rejected")):
            status = {"quota": 429, "permission": 403, "permanent": 501}.get(mode, 401)
            if mode in ("rotate", "rotate-rejected") and attempt == 1:
                replacement = self.server.token_file.with_suffix(".next")
                replacement.touch(mode=0o600)
                replacement.write_text('{"access_token":"fixture-new-bearer"}')
                os.replace(replacement, self.server.token_file)
            code = {"quota": "insufficient_quota", "permanent": "server_error"}.get(mode, "unknown")
            return self.output(status, json.dumps({"error": {"code": code, "message": "private-body"}}).encode())
        if mode == "steer":
            if attempt == 1:
                return self.output(200, call("read_file", "approved-read", {"path": "source.txt"}))
            if attempt == 2:
                return self.output(429, b"{}", retry_after="3")
        if mode in ("stale-tool", "stale-open"):
            stale = call("ask", "must-not-commit", {"question": "Stale output"})
            if mode == "stale-open":
                return self.output(200, stale, pieces=[b": heartbeat\n\n"] * 10)
            return self.output(200, stale, incomplete=True)
        if mode == "early-eof" and attempt == 1:
            return self.output(200, b": heartbeat\n\n")
        if mode == "first-output":
            return self.output(200, b"", pieces=[b": heartbeat\n\n"] * 10)
        if mode == "healthy":
            return self.output(200, DELTA, pieces=[b": heartbeat\n\n"] * 4 + [EMPTY_TERMINAL])
        if mode == "idle":
            return self.output(200, DELTA, pieces=[EMPTY_TERMINAL], pause=0.3)
        self.output(200, EMPTY_TERMINAL)

    def output(self, status, data, retry_after=None, incomplete=False, pieces=(), pause=0.035):
        self.send_response(status)
        self.send_header("Content-Type", "text/event-stream" if status == 200 else "application/json")
        self.send_header("Content-Length", str(len(data) + sum(map(len, pieces)) + (128 if incomplete else 0)))
        if retry_after is not None:
            self.send_header("Retry-After", retry_after)
        self.end_headers()
        self.wfile.write(data)
        self.wfile.flush()
        for piece in pieces:
            time.sleep(pause)
            self.wfile.write(piece)
            self.wfile.flush()
        if incomplete:
            self.close_connection = True


class UnresponsiveTLS(socketserver.BaseRequestHandler):
    def handle(self):
        self.server.release.wait(3)


class TLSProxy(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(3)
        assert self.rfile.readline().startswith(b"CONNECT origin.fixture.invalid:443 ")
        while self.rfile.readline() not in (b"\r\n", b""):
            pass
        self.server.connections.append(self.server.mode)
        mode = self.server.mode
        if mode == "proxy-eof":
            return
        if mode == "proxy-rejected":
            self.wfile.write(b"HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0\r\n\r\n")
            return
        self.wfile.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
        if mode == "certificate":
            try:
                with self.server.tls.wrap_socket(self.connection, server_side=True):
                    raise AssertionError("client trusted the private fixture certificate")
            except ssl.SSLError:
                pass
            return
        record = self.rfile.read(5)
        if len(record) == 5:
            self.rfile.read(int.from_bytes(record[3:], "big"))
        self.connection.shutdown(socket.SHUT_WR)


def run(server, root, mode, *, options=(), token_file=False, steer=False, endpoint=None, proxy=None):
    endpoint = endpoint or f"http://127.0.0.1:{server.server_port}/{mode}"
    extra = ["--workspace", str(root), *options]
    if token_file:
        server.token_file.write_text("fixture-old-bearer")
        server.token_file.chmod(0o600)
        extra.extend(("--token-file", str(server.token_file)))
    client = Client(root / f"{mode}.db", endpoint, extra=extra, proxy=proxy)
    try:
        sid = client.request("new", "create", model="fixture")["result"]["id"]
        assert client.request("submit", "submit", session=sid, command="first", text="Start")["status"] == "ok"
        client.send(dict(id="run", method="run", session=sid))
        if steer:
            backoff = client.receive(lambda m: m.get("event") == "network.state"
                                     and m["data"]["phase"] == "backoff")
            assert backoff["data"]["retry_in_ms"] >= 2900, backoff
            assert client.request("new-input", "steer", session=sid, command="correction",
                                  expected_turn=1, text="Follow the correction")["status"] == "ok"
        result = client.reply("run")
        states = []
        for message in map(json.loads, client.output):
            if message.get("event") == "network.state":
                assert message["seq"] == 0 and message["session"] == sid, message
                assert message["data"]["turn"] == 1, message
                states.append(message["data"])
        assert states and states[-1]["phase"] in ("completed", "failed"), states
        assert all(secret not in "".join(client.output)
                   for secret in ("fixture-old-bearer", "fixture-new-bearer", "private-body"))
        if mode in ("stale-tool", "stale-open"):
            inspected = client.request("inspect", "inspect", session=sid)["result"]
            assert not inspected.get("pending"), inspected
            assert not any("input.question_opened" in line for line in client.output)
        return result, states
    finally:
        client.close()


def exercise_tls(root):
    cert, key = root / "tls.pem", root / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-nodes", "-newkey", "rsa:2048", "-days", "1",
                    "-subj", "/CN=origin.fixture.invalid", "-out", cert, "-keyout", key],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    with socketserver.ThreadingTCPServer(("127.0.0.1", 0), TLSProxy) as proxy:
        proxy.connections = []
        proxy.tls = ctx
        endpoint = "https://origin.fixture.invalid/responses"
        proxy_url = f"http://127.0.0.1:{proxy.server_address[1]}"
        worker = threading.Thread(target=proxy.serve_forever, daemon=True)
        worker.start()
        try:
            for mode, reason, connect_status, attempts in (
                    ("proxy-eof", "proxy_connect_disconnected", 0, 3),
                    ("tunnel-eof", "tls_handshake_disconnected", 200, 3),
                    ("proxy-rejected", "proxy_connect_rejected", 407, 1),
                    ("certificate", "tls_certificate", 200, 1)):
                proxy.mode = mode
                before = len(proxy.connections)
                _, events = run(None, root, mode, endpoint=endpoint, proxy=proxy_url,
                                options=("--transport", "http"))
                terminal = [s for s in events if s["phase"] in ("backoff", "failed")]
                assert len(proxy.connections) - before == attempts, (mode, proxy.connections, events)
                assert [(s["reason"], s["proxy_connect_status"], s["uploaded_bytes"])
                        for s in terminal] == [(reason, connect_status, 0)] * attempts, events
                assert all(s["proxy_used"] and s["curl_code"] for s in terminal), events
                assert [s["phase"] for s in terminal] == ["backoff"] * (attempts - 1) + ["failed"], events
            proxy.mode = "tunnel-eof"
            before = len(proxy.connections)
            _, events = run(None, root, "tls-retry-budget", endpoint=endpoint, proxy=proxy_url,
                            options=("--transport", "http", "--request-timeout-ms", "170"))
            assert len(proxy.connections) - before == 1, events
            assert not any(s["phase"] == "backoff" for s in events), events
        finally:
            proxy.shutdown()
            worker.join()


def exercise(server, root):
    for mode in ("date", "numeric", "zero", "invalid"):
        result, states = run(server, root, mode)
        assert result["result"]["state"] == "completed", (mode, result, states)
        requests = server.requests["/" + mode]
        assert len(requests) == 2 and requests[0][0] == requests[1][0], requests
        delay = requests[1][2] - requests[0][2]
        hint = next(s["retry_in_ms"] for s in states if s["phase"] == "backoff")
        if mode in ("date", "numeric"):
            assert delay >= 0.95 and hint >= 950, (mode, delay, hint)
        else:
            assert hint == 0 if mode == "zero" else 150 <= hint <= 300, (mode, delay, hint)
    for mode, reason in (("quota", "quota"), ("permission", "permission"),
                         ("permanent", "http"), ("huge", "server")):
        _, states = run(server, root, mode, options=("--request-timeout-ms", "800"),
                        token_file=mode == "permission")
        assert states[-1]["reason"] == reason and len(server.requests["/" + mode]) == 1, states
        assert all(s["phase"] not in ("backoff", "refreshing") for s in states), states
    for mode, reason, count in (("rotate", "", 2), ("rotate-rejected", "authentication", 2),
                                ("same-token", "refresh_failed", 1)):
        _, states = run(server, root, mode, token_file=True)
        assert states[-1]["reason"] == reason, states
        assert sum(s["phase"] == "refreshing" for s in states) == 1, states
        requests = server.requests["/" + mode]
        assert [req[1] for req in requests] == ["Bearer fixture-old-bearer"] + (
            ["Bearer fixture-new-bearer"] if count == 2 else []), requests
        if count == 2:
            assert requests[0][0] == requests[1][0], requests

    phase_options = ("--first-output-timeout-ms", "115", "--stream-idle-timeout-ms", "105",
                     "--request-timeout-ms", "270")
    for mode, reason in (("first-output", "first_output_timeout"), ("idle", "stream_idle_timeout"),
                         ("healthy", ""), ("stale-tool", "transport"), ("stale-open", "drain_timeout")):
        _, states = run(server, root, mode, options=phase_options)
        assert states[-1]["reason"] == reason and len(server.requests["/" + mode]) == 1, states
        if mode in ("stale-tool", "stale-open", "healthy"):
            assert any(s["phase"] == "draining" for s in states), states
    result, states = run(server, root, "early-eof")
    assert result["result"]["state"] == "completed", result
    assert any(s["phase"] == "backoff" and s["reason"] == "stream_interrupted" for s in states), states
    result, states = run(server, root, "steer", steer=True)
    assert result["result"]["state"] == "completed", result
    assert sum(s["phase"] == "cancelled" for s in states) == 1, states
    first, second, replacement = [req[0]["input"] for req in server.requests["/steer"]]
    assert second[:len(first)] == first and replacement[:-1] == second
    assert [item["call_id"] for item in second[-2:]] == ["approved-read", "approved-read"]
    assert "local evidence" in second[-1]["output"]
    assert replacement[-1] == {"role": "user", "content": "Follow the correction"}


def main():
    with TemporaryDirectory() as tmp, LoopbackServer(("127.0.0.1", 0), Service) as server:
        root = Path(tmp)
        (root / "source.txt").write_text("local evidence")
        server.token_file = root / "private-token"
        server.requests = defaultdict(list)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            exercise(server, root)
        finally:
            server.shutdown()
            worker.join()
        exercise_tls(root)
        with socketserver.ThreadingTCPServer(("127.0.0.1", 0), UnresponsiveTLS) as tls:
            tls.release = threading.Event()
            worker = threading.Thread(target=tls.serve_forever, daemon=True)
            worker.start()
            try:
                _, states = run(server, root, "connect", options=("--connect-timeout-ms", "120",
                                                                "--request-timeout-ms", "270"),
                                endpoint=f"https://127.0.0.1:{tls.server_address[1]}/responses")
                assert states[-1]["reason"] == "connect_timeout", states
            finally:
                tls.release.set()
                tls.shutdown()
                worker.join()


if __name__ == "__main__":
    main()
