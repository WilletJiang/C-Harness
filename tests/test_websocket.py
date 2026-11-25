"""Responses WebSocket integration, using a loopback HTTP/WebSocket peer."""
import base64
from collections import defaultdict
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler
import json
import os
from pathlib import Path
from queue import Empty
import socket
import socketserver
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
import threading
import time

from loopback import LoopbackServer
from test_http import EMPTY_TERMINAL
from test_rpc import Client, SECRET

IDLE_MODES = ("steer-idle", "steer-idle-close", "steer-idle-eof")
STEER_TOOLS = ("steer-tools", "steer-duplicate", "steer-tools-lost", "steer-probe-timeout",
               "steer-continuation-lost", *IDLE_MODES)
ACK_FAULTS = ("steer-ack-first", "steer-ack-eof", "steer-ack-tools-eof")
SLOW_EXEC = {"argv": ["/bin/sleep", "1"], "timeout_ms": 5000}


def function_call(name, call_id, arguments):
    return {"type": "function_call", "namespace": "local", "name": name, "call_id": call_id,
            "arguments": json.dumps(arguments)}


class Peer(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def handle(self):
        try:
            super().handle()
        except (EOFError, BrokenPipeError, ConnectionResetError, socket.timeout):
            pass
        except BaseException as exc:
            self.server.errors.append(exc)

    def do_POST(self):
        assert self.headers["Authorization"] == f"Bearer {SECRET}"
        self.server.http[self.path].append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(EMPTY_TERMINAL)))
        self.end_headers()
        self.wfile.write(EMPTY_TERMINAL)

    def do_GET(self):
        self.connection.settimeout(5)
        self.close_connection = True
        mode = self.path[1:]
        bearer = self.headers["Authorization"]
        connection = len(self.server.connections[self.path])
        self.server.connections[self.path].append(bearer)
        if mode == "auth" and bearer == "Bearer fixture-old":
            replacement = self.server.token.with_suffix(".next")
            replacement.touch(mode=0o600)
            replacement.write_text("fixture-new")
            os.replace(replacement, self.server.token)
            return self.reject(401)
        if mode == "permission":
            return self.reject(403)
        if mode == "upgrade":
            return self.reject(426)
        if mode == "upgrade-timeout":
            time.sleep(0.35)
            return self.reject(426)
        expected = "fixture-new" if mode == "auth" else "ask-fixture" if mode == "v1/responses" else SECRET
        assert bearer == f"Bearer {expected}"
        if mode == "v1/responses":
            assert self.server.tui_start.wait(5)
        assert self.headers["Upgrade"].lower() == "websocket"
        key = self.headers["Sec-WebSocket-Key"]
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept.decode())
        if mode == "size":
            self.send_header("x-codex-websocket-max-message-bytes", "1")
        self.end_headers()
        self.wfile.flush()
        while True:
            request = self.read_request()
            records = self.server.ws[self.path]
            attempt = len(records)
            records.append((connection, request))
            if mode == "unknown":
                return
            if mode == "steer-continuation-lost" and attempt == 1:
                return
            if mode == "early-close":
                self.frame(8, struct.pack("!H", 1009))
                return
            if mode == "rate" and attempt == 0:
                self.event({"type": "error", "status_code": 429, "error": {
                    "code": "rate_limit_exceeded", "headers": {"Retry-After": "0"}}})
                return
            if mode == "missing" and attempt == 1:
                self.event({"type": "error", "status": 400,
                            "error": {"code": "previous_response_not_found"}})
                return
            rid = f"response-{attempt}"
            self.event({"type": "codex.response.metadata", "headers": {"test": "metadata"}})
            self.event({"type": "response.created", "response": {"id": rid, "status": "in_progress"}})
            if mode == "late-close":
                self.frame(8, struct.pack("!H", 1009))
                return
            if mode.startswith("steer") and attempt == 0:
                self.event({"type": "response.output_text.delta", "response_id": rid, "delta": "old"})
                control = self.read_request()
                assert control == {"type": "response.steer", "previous_response_id": rid,
                                   "input": "correction"}, control
                self.server.steers[self.path].append(connection)
                acknowledgement = {"id": "steer-1", "previous_response_id": rid}
                if mode in ACK_FAULTS:
                    if not self.steer_ack_first(mode, rid, acknowledgement):
                        return
                    continue
                if mode == "steer-rejected":
                    self.event({"type": "response.steer.failed", "steer": acknowledgement,
                                "error": {"code": "steering_not_supported"}})
                    return
                if mode in STEER_TOOLS:
                    tool = function_call("exec", "once", SLOW_EXEC) if mode in IDLE_MODES else \
                        function_call("read_file", "once", {"path": "source.txt"})
                    self.complete(rid, [tool])
                self.event({"type": "response.steer.accepted", "steer": acknowledgement})
                if mode == "steer-disconnect":
                    return
                if mode in STEER_TOOLS:
                    self.event({"type": "response.steer.pending", "steer": acknowledgement,
                                "reason": "waiting_for_required_input",
                                "required_input": [{"type": "function_call_output", "call_id": "once"}]},
                               fragmented=True)
                    if mode == "steer-tools-lost":
                        return
                    if mode in IDLE_MODES and not self.probe_parked(mode):
                        return
                    continue
                self.event({"type": "response.incomplete", "response": {"id": rid, "status": "incomplete",
                            "incomplete_details": {"reason": "steered"}}})
                self.event({"type": "response.created", "response": {"id": "successor", "status": "in_progress"}})
                self.complete("successor", [])
                continue
            output = []
            if mode in ("chain", "missing") and attempt == 0:
                output = [function_call("read_file", "once", {"path": "source.txt"})]
            elif mode in ("chain", "v1/responses"):
                text = "TUI WebSocket ready." if mode == "v1/responses" else "Done."
                output = [{"type": "message", "role": "assistant",
                           "content": [{"type": "output_text", "text": text}]}]
            if mode == "wrong-id":
                rid = "previous-response"
                output = [function_call("ask", "stale", {"question": "Stale?"})]
            if mode == "steer-duplicate":
                output = [function_call("read_file", "once", {"path": "source.txt"})]
            if mode == "steer-ack-eof" and attempt == 1:
                output = [function_call("read_file", "once", {"path": "source.txt"})]
            self.complete(rid, output)

    def steer_ack_first(self, mode, rid, acknowledgement):
        self.event({"type": "response.steer.accepted", "steer": acknowledgement})
        assert self.server.received_steer_ack.wait(3), "Client did not consume the early steer ACK"
        if mode != "steer-ack-eof":
            self.complete(rid, [function_call("read_file", "once", {"path": "source.txt"})])
        if mode == "steer-ack-first":
            return True
        self.connection.shutdown(socket.SHUT_WR)
        return False

    def reject(self, code):
        self.send_response(code)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def complete(self, response_id, output):
        response = {"id": response_id, "status": "completed", "output": output}
        if self.path == "/large-event":
            response["fixture_padding"] = "résultat" * 12000
        self.event({"type": "response.completed", "response": response}, fragmented=True)

    def exact(self, size):
        data = self.rfile.read(size)
        if len(data) != size:
            raise EOFError
        return data

    def read_frame(self):
        first, mask_length = self.exact(2)
        length = mask_length & 127
        if length in (126, 127):
            length = int.from_bytes(self.exact(2 if length == 126 else 8), "big")
        assert mask_length & 128 and length < 4 * 1024 * 1024
        mask = self.exact(4)
        return first, bytes(value ^ mask[index % 4] for index, value in enumerate(self.exact(length)))

    def read_request(self):
        parts = bytearray()
        fragments = 0
        while True:
            opcode, data = self.read_frame()
            if opcode == 0x88:
                raise EOFError
            if opcode == 0x89:
                self.server.client_pings[self.path].append(data)
                self.frame(10, b"unmatched-pong" if self.path == "/steer-probe-timeout" else data)
                continue
            if opcode == 0x8A:
                continue
            assert (opcode & 0x0F) == (0 if parts else 1), opcode
            parts.extend(data)
            fragments += 1
            if opcode & 0x80:
                break
        if self.path == "/large-upload":
            assert fragments >= 3
        message = json.loads(parts)
        assert message["type"] in ("response.create", "response.steer") and "stream" not in message
        return message

    def expect_pong(self, payload):
        self.frame(9, payload)
        while True:
            opcode, data = self.read_frame()
            assert opcode == 0x8A, (opcode, data[:40])
            if data == payload:
                return
            assert data == b"heartbeat", data

    def probe_parked(self, mode):
        probe, acknowledged, running, ran = self.server.idle_probe
        assert probe.wait(4), "RPC did not park for approval"
        self.event({"type": "codex.rate_limits", "rate_limits": {}})
        self.expect_pong(bytes(range(125)))
        if mode == "steer-idle-close":
            close = struct.pack("!H", 1001)
            self.frame(8, close)
            assert self.read_frame() == (0x88, close)
        elif mode == "steer-idle-eof":
            self.connection.shutdown(socket.SHUT_WR)
        acknowledged.set()
        if mode != "steer-idle":
            return False
        assert running.wait(4), "Approved process did not start"
        self.expect_pong(b"during-local-exec")
        ran.set()
        return True

    @staticmethod
    def wire_frame(opcode, data, final=True):
        length = len(data)
        size = (bytes([length]) if length < 126 else bytes([126]) + struct.pack("!H", length)
                if length < 65536 else bytes([127]) + struct.pack("!Q", length))
        return bytes([(128 if final else 0) | opcode]) + size + data

    def frame(self, opcode, data):
        self.wfile.write(self.wire_frame(opcode, data))
        self.wfile.flush()

    def event(self, value, fragmented=False):
        data = json.dumps(value).encode()
        if fragmented:
            self.wfile.write(self.wire_frame(1, data[:29], final=False) +
                             self.wire_frame(9, b"heartbeat") + self.wire_frame(0, data[29:]))
            self.wfile.flush()
        else:
            self.frame(1, data)


@contextmanager
def client_for(server, root, mode, transport="websocket", text="start", env=None):
    extra = ["--transport", transport, "--workspace", str(root), "--no-web-search"]
    if mode == "upgrade-timeout":
        extra += ["--connect-timeout-ms", "120"]
    if mode in IDLE_MODES:
        extra += ["--enable-exec"]
    if mode == "auth":
        server.token = root / "private-token"
        server.token.touch(mode=0o600)
        server.token.write_text("fixture-old")
        extra += ["--token-file", str(server.token)]
    client = Client(root / f"{mode}.db", f"http://127.0.0.1:{server.server_port}/{mode}", extra=extra,
                    env_overrides=env)
    try:
        sid = client.request("create", "create", model="fixture")["result"]["id"]
        assert client.request("submit", "submit", session=sid, command="start", text=text)["status"] == "ok"
        client.send(dict(id="run", method="run", session=sid))
        yield client, sid
    finally:
        client.close()


def states(client):
    return [event["data"] for event in map(json.loads, client.output)
            if event.get("event") == "network.state"]


def steer_completion_window(server, root, mode):
    observed = server.received_steer_ack = threading.Event()
    release = root / f"{mode}.release"
    env = {"AH_TEST_WS_RELEASE_STEER_COMPLETION": str(release)}
    if mode != "steer-ack-first":
        env["AH_TEST_WS_STEER_EOF_BEFORE_DRAIN"] = "1"
    path = "/" + mode
    with client_for(server, root, mode, env=env) as (client, sid):
        client.receive(lambda row: row.get("event") == "model.delta")
        assert client.request("steer", "steer", session=sid, command="new", text="correction",
                              expected_turn=1)["status"] == "ok"
        client.receive(lambda row: row.get("event") == "network.state" and
                       row["data"]["phase"] == "steer_accepted")
        assert not any(s["phase"] == "steer_sent" for s in states(client))
        if mode == "steer-ack-first":
            release.touch()
            client.receive(lambda row: row.get("event") == "network.state" and
                           row["data"]["phase"] == "steer_sent")
        observed.set()
        assert client.reply("run")["result"]["state"] == "completed", client.output
        trace = states(client)
        assert sum(s["phase"] == "steer_accepted" for s in trace) == 1
        assert sum(s["phase"] == "steer_sent" for s in trace) == (mode == "steer-ack-first")
        events = [row.get("event") for row in map(json.loads, client.output)]
        assert all(events.count(name) == 1 for name in
                   ("input.steered", "tool.intent", "tool.started", "tool.completed")), events
        records = server.ws[path]
        assert server.steers[path] == [0] and not server.http[path]
        assert [index for index, _ in records] == {
            "steer-ack-first": [0, 0], "steer-ack-eof": [0, 1, 1], "steer-ack-tools-eof": [0, 1],
        }[mode], records
        outputs = [item for _, body in records for item in body["input"]
                   if item.get("type") == "function_call_output"]
        assert len(outputs) == 1 and outputs[0]["call_id"] == "once"
        assert "saved evidence" in outputs[0]["output"]
        if mode == "steer-ack-first":
            assert records[1][1]["previous_response_id"] == "response-0"
        else:
            recovered = records[1][1]
            assert "previous_response_id" not in recovered
            assert sum(item.get("content") == "correction" for item in recovered["input"]) == 1
        assert events.count("model.interrupted") == (mode == "steer-ack-eof")


def parked_steer(server, root, mode):
    server.idle_probe = tuple(threading.Event() for _ in range(4))
    probe, acknowledged, running, ran = server.idle_probe
    path = "/" + mode
    with client_for(server, root, mode) as (client, sid):
        client.receive(lambda row: row.get("event") == "model.delta")
        assert client.request("steer", "steer", session=sid, command="new", text="correction",
                              expected_turn=1)["status"] == "ok"
        approval = client.receive(lambda row: row.get("event") == "approval.requested")
        assert client.reply("run")["result"]["state"] == "waiting_approval", client.output
        # The first RPC worker is finished and a new run cannot poll the provider yet.
        probe.set()
        assert acknowledged.wait(3), "The parked socket did not handle the peer control frame"
        assert client.request("allow", "decide", session=sid, operation=approval["operation"],
                              allow=True)["status"] == "ok"
        client.send(dict(id="resume", method="run", session=sid))
        client.receive(lambda row: row.get("event") == "tool.started")
        running.set()
        if mode == "steer-idle":
            assert ran.wait(2), "The in-flight process blocked the Pong"
        assert client.reply("resume")["result"]["state"] == "completed", client.output
        events = [row.get("event") for row in map(json.loads, client.output)]
        assert all(events.count(name) == 1 for name in ("tool.intent", "tool.started", "tool.completed"))
        assert ran.is_set() == (mode == "steer-idle")
        assert server.steers[path] == [0] and not server.http[path]
        assert sum(s["phase"] == "steer_waiting_tools" for s in states(client)) == 1
        first, continuation = server.ws[path]
        assert first[0] == 0
        index, body = continuation
        if mode == "steer-idle":
            assert index == 0 and body["previous_response_id"] == "response-0"
            assert [(item["type"], item["call_id"]) for item in body["input"]] == [
                ("function_call_output", "once")]
        else:
            assert index == 1 and "previous_response_id" not in body
            assert [(item["type"], item["call_id"]) for item in body["input"] if "call_id" in item] == [
                ("function_call", "once"), ("function_call_output", "once")]
            assert sum(item.get("content") == "correction" for item in body["input"]) == 1


def tui_run(server, root):
    from test_tui_ask import terminal
    from test_tui_graphics import receive
    server.tui_start = threading.Event()
    try:
        with terminal(Path(sys.argv[1]).resolve(), root, server, "--model", "fixture",
                      "--transport", "websocket") as master:
            receive(master, b"Ask ah anything")
            os.write(master, b"Run over WebSocket\r")
            receive(master, b"via WebSocket")
            server.tui_start.set()
            receive(master, b"TUI WebSocket ready.")
    finally:
        server.tui_start.set()
    assert len(server.ws["/v1/responses"]) == 1 and not server.http["/v1/responses"]


class ClosedTLSHandshake(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(2)
        record = self.rfile.read(5)
        if len(record) == 5:
            self.rfile.read(int.from_bytes(record[3:], "big"))
        self.connection.shutdown(socket.SHUT_WR)


def tls_disconnect(root):
    with socketserver.ThreadingTCPServer(("127.0.0.1", 0), ClosedTLSHandshake) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        endpoint = f"https://127.0.0.1:{server.server_address[1]}/responses"
        client = Client(root / "tls-disconnect.db", endpoint, extra=("--transport", "websocket"))
        try:
            sid = client.request("create", "create", model="fixture")["result"]["id"]
            assert client.request("submit", "submit", session=sid, command="start", text="start")["status"] == "ok"
            assert client.request("run", "run", session=sid)["status"] != "ok"
            trace = states(client)
            assert [(state["phase"], state["transport"], state["reason"]) for state in trace] == [
                ("connecting", "websocket", ""),
                ("backoff", "websocket", "transport"),
                ("connecting", "websocket", ""),
                ("fallback", "websocket", "websocket_unavailable"),
                ("connecting", "http", ""),
                ("backoff", "http", "tls_handshake_disconnected"),
                ("connecting", "http", ""),
                ("failed", "http", "tls_handshake_disconnected"),
            ], trace
        finally:
            client.close()
            server.shutdown()
            thread.join()


def main():
    env = dict(os.environ)
    env.pop("AH_ACCESS_TOKEN", None)
    probe = subprocess.run([sys.argv[1], "rpc", "--db", ":memory:", "--transport", "websocket"],
                           input="", text=True, capture_output=True, env=env, timeout=5)
    if "linked libcurl does not support WebSocket transport" in probe.stderr:
        raise SystemExit(77)
    assert probe.returncode == 0, probe.stderr
    with TemporaryDirectory() as tmp, LoopbackServer(("127.0.0.1", 0), Peer) as server:
        root = Path(tmp)
        (root / "source.txt").write_text("saved evidence")
        server.ws, server.http, server.connections = (defaultdict(list) for _ in range(3))
        server.errors = []
        server.steers, server.client_pings = defaultdict(list), defaultdict(list)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            if "--steer-faults" in sys.argv[2:]:
                for mode in ACK_FAULTS:
                    steer_completion_window(server, root, mode)
                assert not server.errors, server.errors
                return
            for mode in ("chain", "missing"):
                with client_for(server, root, mode) as (client, sid):
                    assert client.reply("run")["result"]["state"] == "completed", client.output
                    records = server.ws["/" + mode]
                    assert records[0][0] == records[1][0] == 0
                    assert "previous_response_id" not in records[0][1]
                    assert records[1][1]["previous_response_id"] == "response-0"
                    assert [item["type"] for item in records[1][1]["input"]] == ["function_call_output"]
                    assert "saved evidence" in records[1][1]["input"][0]["output"]
                    assert sum(json.loads(line).get("event") == "tool.intent" for line in client.output) == 1
                    if mode == "missing":
                        assert len(records) == 3 and records[2][0] == 1
                        rebuilt = records[2][1]
                        assert "previous_response_id" not in rebuilt
                        assert rebuilt["input"][:-2] == records[0][1]["input"]
                        assert [item["call_id"] for item in rebuilt["input"][-2:]] == ["once", "once"]
                    else:
                        assert len(records) == 2
                        assert client.request("submit2", "submit", session=sid, command="second",
                                              text="next turn")["status"] == "ok"
                        assert client.request("run2", "run", session=sid)["result"]["state"] == "completed"
                        connection, continuation = records[2]
                        assert connection == 0 and continuation["previous_response_id"] == "response-1"
                        assert continuation["input"] == [{"role": "user", "content": "next turn"}]
            for mode, reason, sent in (("upgrade", "websocket_unavailable", 0),
                                       ("upgrade-timeout", "connect_timeout", 0),
                                       ("size", "websocket_size", 0),
                                       ("early-close", "websocket_size", 1)):
                with client_for(server, root, mode) as (client, _):
                    assert client.reply("run")["result"]["state"] == "completed", client.output
                    assert len(server.http["/" + mode]) == 1 and len(server.ws["/" + mode]) == sent
                    fallback = [state for state in states(client) if state["phase"] == "fallback"]
                    assert [(s["transport"], s["reason"]) for s in fallback] == [("websocket", reason)], states(client)
                    assert states(client)[-1]["transport"] == "http"
            for mode, reason in (("unknown", "transport"), ("late-close", "transport"),
                                 ("wrong-id", "protocol"), ("permission", "permission")):
                with client_for(server, root, mode) as (client, _):
                    client.reply("run")
                    assert len(server.connections["/" + mode]) == 1 and not server.http["/" + mode]
                    assert states(client)[-1]["reason"] == reason, client.output
                    assert not any(json.loads(line).get("event") == "tool.intent" for line in client.output)
            for mode, phase in (("rate", "backoff"), ("auth", "refreshing")):
                with client_for(server, root, mode) as (client, _):
                    assert client.reply("run")["result"]["state"] == "completed", client.output
                    events = states(client)
                    assert len(server.connections["/" + mode]) == 2 and not server.http["/" + mode]
                    retry = [state for state in events if state["phase"] == phase]
                    assert len(retry) == 1 and retry[0]["retry_in_ms"] == 0, events
            for mode in ("steer", "steer-tools", "steer-tools-lost", "steer-probe-timeout",
                         "steer-continuation-lost", "steer-duplicate", "steer-rejected", "steer-disconnect"):
                with client_for(server, root, mode) as (client, sid):
                    delta = client.receive(lambda row: row.get("event") == "model.delta")
                    assert delta["data"]["text"] == "old"
                    assert client.request("steer", "steer", session=sid, command="new", text="correction",
                                          expected_turn=1)["status"] == "ok"
                    try:
                        result = client.reply("run")
                    except Empty as exc:
                        raise AssertionError((mode, states(client), server.errors)) from exc
                    failed = mode in ("steer-duplicate", "steer-continuation-lost")
                    assert result["result"]["state"] == ("failed" if failed else "completed"), client.output
                    assert server.steers["/" + mode] == [0]
                    attempts = server.ws["/" + mode]
                    trace = states(client)
                    assert sum(s["phase"] == "steer_sent" for s in trace) == 1, trace
                    tool_intents = sum(json.loads(line).get("event") == "tool.intent" for line in client.output)
                    if mode == "steer":
                        assert len(attempts) == 1 and tool_intents == 0
                        assert [(s["phase"], s["connection"]) for s in trace if s["phase"].startswith("steer_")] == [
                            ("steer_sent", 1), ("steer_accepted", 1), ("steer_continued", 1)]
                    elif mode in ("steer-tools", "steer-duplicate", "steer-continuation-lost"):
                        assert len(attempts) == 2 and attempts[1][0] == 0 and tool_intents == 1, client.output
                        next_body = attempts[1][1]
                        assert next_body["previous_response_id"] == "response-0"
                        assert [item["call_id"] for item in next_body["input"]] == ["once"]
                        assert any(s["phase"] == "steer_waiting_tools" for s in trace), trace
                        assert len(server.client_pings["/" + mode]) == 1
                        if mode == "steer-continuation-lost":
                            assert trace[-1]["reason"] == "transport" and len(server.connections["/" + mode]) == 1
                    elif mode in ("steer-tools-lost", "steer-probe-timeout"):
                        assert len(attempts) == 2 and attempts[1][0] == 1 and tool_intents == 1, client.output
                        next_body = attempts[1][1]
                        assert "previous_response_id" not in next_body
                        assert [item["call_id"] for item in next_body["input"] if "call_id" in item] == ["once", "once"]
                        assert sum(item.get("content") == "correction" for item in next_body["input"]) == 1
                        if mode == "steer-probe-timeout":
                            assert len(server.client_pings["/" + mode]) == 1
                            assert not server.http["/" + mode]
                    else:
                        assert len(attempts) == 2 and attempts[1][0] == 1 and tool_intents == 0
                        assert "previous_response_id" not in attempts[1][1]
                        assert attempts[1][1]["input"][-1]["content"] == "correction"
            for mode in IDLE_MODES:
                parked_steer(server, root, mode)
            with client_for(server, root, "custom", transport="auto") as (client, _):
                assert client.reply("run")["result"]["state"] == "completed", client.output
                assert not server.connections["/custom"] and states(client)[-1]["transport"] == "http"
            with client_for(server, root, "large-event") as (client, _):
                assert client.reply("run")["result"]["state"] == "completed", client.output
                completed = next(row for row in map(json.loads, client.output)
                                 if row.get("event") == "model.completed")
                assert completed["data"]["fixture_padding"] == "résultat" * 12000
            large_prompt = "large request " * 12000
            with client_for(server, root, "large-upload", text=large_prompt) as (client, _):
                assert client.reply("run")["result"]["state"] == "completed", client.output[-10:]
                (_, body), = server.ws["/large-upload"]
                assert body["input"][-1]["content"] == large_prompt
            if "--tui" in sys.argv[2:]:
                tui_run(server, root)
            if "--bundled-curl" in sys.argv[2:]:
                tls_disconnect(root)
            assert not server.errors, server.errors
        finally:
            server.shutdown()
            thread.join()


if __name__ == "__main__":
    main()
