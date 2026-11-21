"""Verify prefix and cache affinity through live steering, ask replies and a restart."""
import json
from http.server import BaseHTTPRequestHandler
from pathlib import Path
import tempfile
import threading

from loopback import LoopbackServer
from test_rpc import Client


class Service(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        n = len(self.server.requests)
        self.server.requests.append((body, self.headers["session-id"]))
        if n == 0:
            self.server.inflight.set()
            assert self.server.steered.wait(5)
        output = []
        if n < 3:
            output.append({"type": "function_call", "namespace": "local", "name": "ask",
                           "call_id": f"ask-{n}", "arguments": json.dumps({"question": f"Q{n}?"})})
        event = {"type": "response.completed", "response": {"status": "completed", "output": output}}
        wire = ("data: " + json.dumps(event) + "\n\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(wire)))
        self.end_headers()
        self.wfile.write(wire)


def main():
    with tempfile.TemporaryDirectory() as tmp, LoopbackServer(("127.0.0.1", 0), Service) as server:
        server.requests = []
        server.inflight, server.steered = threading.Event(), threading.Event()
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        db = Path(tmp) / "session.db"
        endpoint = f"http://127.0.0.1:{server.server_port}/responses"
        client = Client(db, endpoint)
        try:
            sid = client.request("c", "create", model="gpt-6.2-sol", instructions="Host rules")["result"]["id"]
            assert client.request("s", "submit", session=sid, command="start", text="Design a report")["status"] == "ok"
            client.send(dict(id="r0", method="run", session=sid))
            assert server.inflight.wait(5)
            steer = dict(session=sid, command="change-1", text="Use a compact layout")
            for ident in ("mid-run", "retry"):
                assert client.request(ident, "steer", **steer)["result"]["queued"]
            server.steered.set()
            state = client.reply("r0")["result"]
            assert client.request("paused", "steer", session=sid, command="paused",
                                  text="No active run")["status"] == "busy"
            for n in range(3):
                assert state["state"] == "waiting_input" and state["pending"]["name"] == "ask"
                assert state["pending"]["arguments"]["question"] == f"Q{n}?"
                if n == 1:
                    client.close()
                    client = Client(db, endpoint)
                    assert client.request("i", "inspect", session=sid)["result"]["pending"] == state["pending"]
                reply = client.request(f"a{n}", "reply", session=sid, operation=state["pending"]["operation_id"],
                                       value=f"answer-{n}")
                assert reply["status"] == "ok", reply
                state = client.request(f"r{n + 1}", "run", session=sid)["result"]
            assert state["state"] == "completed"
            first = server.requests[0][0]
            assert first["input"][0]["role"] == "developer"
            assert first["input"][0]["content"].endswith("\n\nHost rules")
            assert len(server.requests) == 4
            last = []
            for n, (body, route) in enumerate(server.requests):
                assert route == body["prompt_cache_key"] == sid
                assert {k: v for k, v in body.items() if k != "input"} == {
                    k: v for k, v in first.items() if k != "input"}
                items = body["input"]
                assert json.dumps(items[:len(last)]) == json.dumps(last)
                if n:
                    suffix = items[len(last):]
                    assert suffix[0]["call_id"] == suffix[1]["call_id"] == f"ask-{n-1}"
                    assert suffix[1]["type"] == "function_call_output"
                    assert json.loads(suffix[1]["output"])["result"] == f"answer-{n-1}"
                    assert suffix[2:] == ([{"role": "user", "content": steer["text"]}] if n == 1 else [])
                last = items
        finally:
            server.steered.set()
            client.close()
            server.shutdown()
            worker.join()


if __name__ == "__main__":
    main()
