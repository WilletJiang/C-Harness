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
        self.server.requests.append((body, self.headers["session-id"], self.headers.get("x-codex-turn-state")))
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("x-codex-turn-state", f"opaque-route-{n}")
        self.end_headers()
        if n == 0:
            self.wfile.write(b'data: {"type":"response.output_text.delta","delta":"old draft"}\n\n')
            self.wfile.flush()
            self.server.release_old.wait(8)
        if n == 1:
            self.server.replacement.set()
            self.server.allow_reply.wait(5)
        output = []
        if n < 4:
            label = "stale" if n == 0 else str(n - 1)
            output.append({"type": "function_call", "namespace": "local", "name": "ask",
                           "call_id": f"ask-{label}", "arguments": json.dumps({"question": f"Q{label}?"})})
        event = {"type": "response.completed", "response": {"status": "completed", "output": output}}
        try:
            self.wfile.write(("data: " + json.dumps(event) + "\n\n").encode())
        except (BrokenPipeError, ConnectionResetError):
            pass


def exercise(extra):
    with tempfile.TemporaryDirectory() as tmp, LoopbackServer(("127.0.0.1", 0), Service) as server:
        server.requests = []
        server.release_old, server.replacement, server.allow_reply = (threading.Event() for _ in range(3))
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        db = Path(tmp) / "session.db"
        endpoint = f"http://127.0.0.1:{server.server_port}/responses"
        client = Client(db, endpoint, extra=extra)
        try:
            sid = client.request("c", "create", model="gpt-6.2-sol", instructions="Host rules")["result"]["id"]
            assert client.request("s", "submit", session=sid, command="start", text="Design a report")["status"] == "ok"
            client.send(dict(id="r0", method="run", session=sid))
            delta = client.receive(lambda r: r.get("event") == "model.delta")
            assert delta["data"]["text"] == "old draft"
            steer = dict(session=sid, command="change-1", text="Use a compact layout")
            assert client.request("wrong-turn", "steer", expected_turn=2, **steer)["status"] == "busy"
            assert client.request("mid-run", "steer", expected_turn=1, **steer)["result"] == {"queued": True, "turn": 1}
            assert server.replacement.wait(3), "Steer waited for the old response to finish"
            interrupted = client.receive(lambda r: r.get("event") == "model.interrupted")
            assert interrupted["data"] == {"reason": "steer", "turn": 1, "attempt": 1}
            # Retry after durable delivery: this must not interrupt the replacement.
            assert client.request("retry", "steer", **steer)["result"]["turn"] == 1
            server.allow_reply.set()
            state = client.reply("r0")["result"]
            assert client.request("paused", "steer", session=sid, command="paused",
                                  text="No active run")["status"] == "busy"
            for n in range(3):
                assert state["state"] == "waiting_input" and state["pending"]["name"] == "ask"
                assert state["pending"]["arguments"]["question"] == f"Q{n}?"
                if n == 1:
                    client.close()
                    client = Client(db, endpoint, extra=extra)
                    assert client.request("i", "inspect", session=sid)["result"]["pending"] == state["pending"]
                reply = client.request(f"a{n}", "reply", session=sid, operation=state["pending"]["operation_id"],
                                       value=f"answer-{n}")
                assert reply["status"] == "ok", reply
                state = client.request(f"r{n + 1}", "run", session=sid)["result"]
            assert state["state"] == "completed"
            first = server.requests[0][0]
            assert first["input"][0]["role"] == "developer"
            assert first["input"][0]["content"].endswith("\n\nHost rules")
            assert len(server.requests) == 5
            assert [h for _, _, h in server.requests] == [None, "opaque-route-0", "opaque-route-0", None, "opaque-route-3"]
            last = []
            for n, (body, route, _) in enumerate(server.requests):
                assert route == body["prompt_cache_key"] == sid
                assert {k: v for k, v in body.items() if k != "input"} == {
                    k: v for k, v in first.items() if k != "input"}
                items = body["input"]
                assert json.dumps(items[:len(last)]) == json.dumps(last)
                suffix = items[len(last):]
                if n == 1:
                    assert suffix == [{"role": "user", "content": steer["text"]}]
                elif n >= 2:
                    assert len(suffix) == 2
                    assert suffix[0]["call_id"] == suffix[1]["call_id"] == f"ask-{n-2}"
                    assert json.loads(suffix[1]["output"])["result"] == f"answer-{n-2}"
                last = items
            assert client.request("s2", "submit", session=sid, command="turn2", text="New task")["status"] == "ok"
            assert client.request("r2-final", "run", session=sid)["result"]["state"] == "completed"
            assert server.requests[-1][2] is None, "Routing token escaped into a new turn"
        finally:
            server.release_old.set()
            server.allow_reply.set()
            client.close()
            server.shutdown()
            worker.join()


if __name__ == "__main__":
    exercise(())
    exercise(("--agents", "2"))
