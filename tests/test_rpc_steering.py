"""Verify prefix and cache affinity through live steering, ask replies and a restart."""
import json
import sqlite3
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


class BackgroundQuestionService(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        n = len(self.server.requests)
        self.server.requests.append(body)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        if n == 0:
            output = [
                {"type": "function_call", "namespace": "local", "name": "ask", "call_id": "question",
                 "arguments": json.dumps({"question": "What should the report emphasize?", "blocking": False})},
                {"type": "function_call", "namespace": "local", "name": "read_file", "call_id": "independent",
                 "arguments": json.dumps({"path": "source.txt"})},
            ]
        else:
            if n == 1:
                self.server.independent.set()
                self.server.release.wait(6)
            text = "Independent work is ready." if n == 1 else "The answer was applied."
            output = [{"type": "message", "role": "assistant",
                       "content": [{"type": "output_text", "text": text}]}]
        event = {"type": "response.completed", "response": {"status": "completed", "output": output}}
        self.wfile.write(("data: " + json.dumps(event) + "\n\n").encode())


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


def exercise_background_question(reply_while_running, extra=()):
    with tempfile.TemporaryDirectory() as tmp, LoopbackServer(("127.0.0.1", 0), BackgroundQuestionService) as server:
        root = Path(tmp)
        (root / "source.txt").write_text("Independent material.\n")
        server.requests = []
        server.independent, server.release = threading.Event(), threading.Event()
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        db = root / "session.db"
        endpoint = f"http://127.0.0.1:{server.server_port}/responses"
        options = ("--workspace", str(root), *extra)
        client = Client(db, endpoint, extra=options)
        try:
            sid = client.request("create", "create", model="gpt-6.2-sol")["result"]["id"]
            assert client.request("submit", "submit", session=sid, command="start", text="Draft a report")["status"] == "ok"
            client.send(dict(id="run", method="run", session=sid))
            question = client.receive(lambda r: r.get("event") == "input.question_opened")
            operation = question["operation"]
            assert question["data"]["arguments"]["blocking"] is False
            assert server.independent.wait(4), "Independent read was blocked by the question"
            receipts = {item["call_id"]: json.loads(item["output"])["result"]
                        for item in server.requests[1]["input"] if item.get("type") == "function_call_output"}
            assert receipts["question"] == {"pending_question": operation}
            assert "Independent material." in receipts["independent"]["content"]
            answer = dict(session=sid, operation=operation, value="Emphasize verbatim 用户选择")
            if reply_while_running:
                rejected = client.request("wrong-turn", "reply", expected_turn=2, **answer)
                assert rejected["status"] == "busy", rejected
                assert client.request("reply", "reply", expected_turn=1, **answer)["result"] == {"queued": True}
                assert client.request("duplicate", "reply", **answer)["status"] == "ok"
                assert client.request("note", "steer", session=sid, expected_turn=1, command="note",
                                      text="Also include a table.", interrupt=False)["status"] == "ok"
                assert len(server.requests) == 2
                server.release.set()
                state = client.reply("run")["result"]
            else:
                server.release.set()
                state = client.reply("run")["result"]
                assert state["state"] == "waiting_input" and state["pending"]["background"]
                assert state["pending"]["operation_id"] == operation
                client.close()
                client = Client(db, endpoint, extra=options)
                assert client.request("inspect", "inspect", session=sid)["result"]["pending"] == state["pending"]
                assert client.request("reply", "reply", **answer)["status"] == "ok"
                assert client.request("duplicate", "reply", **answer)["status"] == "ok"
                state = client.request("continue", "run", session=sid)["result"]
            assert state["state"] == "completed" and state["turn"] == 1, state
            assert len(server.requests) == 3
            items = server.requests[-1]["input"]
            final_message, answer_item = (items[-3], items[-2]) if reply_while_running else items[-2:]
            assert final_message["content"][0]["text"] == "Independent work is ready."
            if reply_while_running:
                assert items[-1] == {"role": "user", "content": "Also include a table."}
            assert answer_item["role"] == "user"
            response = json.loads(answer_item["content"])
            assert response["reply_to_question"] == operation and response["answer"] == answer["value"]
            assert sum(item.get("type") == "function_call_output" and item["call_id"] == "question"
                       for item in items) == 1
            with sqlite3.connect(db.as_uri() + "?mode=ro", uri=True) as conn:
                events = [kind for kind, in conn.execute("SELECT type FROM events WHERE session_id=?", (sid,))]
            assert events.count("input.question_answered") == 1, events
            assert "model.interrupted" not in events
        finally:
            server.release.set()
            client.close()
            server.shutdown()
            worker.join()


if __name__ == "__main__":
    exercise(())
    exercise(("--agents", "2"))
    exercise_background_question(True)
    exercise_background_question(False)
    exercise_background_question(True, ("--agents", "2"))
