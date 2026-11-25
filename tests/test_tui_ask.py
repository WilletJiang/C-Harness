"""Exercise durable ask choices and plain replies through the real terminal."""
from contextlib import contextmanager
import fcntl
import http.server
import json
import os
from pathlib import Path
import pty
import sqlite3
import struct
import subprocess
import sys
import tempfile
import termios
import threading

from loopback import LoopbackServer
from test_tui_graphics import receive


QUESTION = "接下来你想做哪一步？"
CHOICES = ["阅读设计", "/quit", "实现示例并运行离线测试"]


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        item = request["input"][-1]
        if item.get("type") == "function_call_output":
            self.server.replies.append(json.loads(item["output"])["result"])
            output = [{"type": "message", "role": "assistant",
                       "content": [{"type": "output_text", "text": "Reply recorded."}]}]
        else:
            mode = item["content"]
            args = {"question": QUESTION}
            if mode != "plain question":
                args["options"] = CHOICES
            output = [{"type": "function_call", "name": "ask", "call_id": mode,
                       "arguments": json.dumps(args, ensure_ascii=False)}]
        event = {"type": "response.completed", "response": {"status": "completed", "output": output}}
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        self.wfile.write(("data: " + json.dumps(event, ensure_ascii=False) + "\n\n").encode())


class BackgroundProvider(Provider):
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        n = len(self.server.requests)
        self.server.requests.append(request)
        if n == 0:
            output = [{"type": "function_call", "name": "ask", "call_id": "background",
                       "arguments": json.dumps({"question": QUESTION, "blocking": False})}]
        else:
            if n == 1:
                self.server.working.set()
                self.server.release.wait(6)
            text = "Independent work ready." if n == 1 else "Background answer recorded."
            output = [{"type": "message", "role": "assistant",
                       "content": [{"type": "output_text", "text": text}]}]
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        event = {"type": "response.completed", "response": {"status": "completed", "output": output}}
        self.wfile.write(("data: " + json.dumps(event) + "\n\n").encode())


class BackoffProvider(Provider):
    def do_POST(self):
        if self.server.attempts:
            self.server.attempts += 1
            return super().do_POST()
        self.server.attempts += 1
        self.rfile.read(int(self.headers["Content-Length"]))
        self.send_response(429)
        self.send_header("Retry-After", "1")
        self.send_header("Content-Length", "0")
        self.end_headers()


@contextmanager
def terminal(binary, root, server, *args):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 78, 0, 0))
    env = dict(os.environ, TERM="xterm-256color", AH_ACCESS_TOKEN="ask-fixture",
               AH_TUI_MATH="text", XDG_CONFIG_HOME=str(root / "config"))
    endpoint = f"http://127.0.0.1:{server.server_port}/v1/responses"
    process = subprocess.Popen(
        [binary, "tui", "--db", root / "test.db", "--endpoint", endpoint,
         "--allow-http-loopback", "--no-web-search", *args],
        cwd=root, env=env, stdin=slave, stdout=slave, stderr=slave, start_new_session=True,
    )
    try:
        yield master
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
        os.close(master)
        os.close(slave)


def expect_choices(master):
    output = receive(master, b"Type a reply")
    assert b"Agent question" in output and QUESTION.encode() in output
    assert all(choice.encode() in output for choice in CHOICES), output


def network_backoff(binary):
    with LoopbackServer(("127.0.0.1", 0), BackoffProvider) as server, \
         tempfile.TemporaryDirectory(prefix="ah-tui-backoff-") as directory:
        server.attempts = 0
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            with terminal(binary, Path(directory), server, "--model", "fixture") as master:
                receive(master, b"Ask ah anything")
                os.write(master, b"recover\r")
                receive(master, b"Retrying")
                expect_choices(master)
                assert server.attempts == 2
        finally:
            server.shutdown()
            worker.join()


def background_reply(binary):
    with LoopbackServer(("127.0.0.1", 0), BackgroundProvider) as server, \
         tempfile.TemporaryDirectory(prefix="ah-tui-live-question-") as directory:
        root = Path(directory)
        server.requests = []
        server.working, server.release = threading.Event(), threading.Event()
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            with terminal(binary, root, server, "--model", "fixture") as master:
                receive(master, b"Ask ah anything")
                os.write(master, b"Start independent work\r")
                receive(master, b"Question available. Use /pending to reply.")
                assert server.working.wait(3)
                os.write(master, b"/note Include a sidebar\r")
                receive(master, b"Note queued for the next model request")
                os.write(master, b"/pending\r")
                receive(master, b"Type your answer")
                os.write(master, "保持结果简洁\r".encode())
                receive(master, b"Queue steering message while running")
                assert len(server.requests) == 2
                server.release.set()
                receive(master, b"Background answer recorded.")
            assert len(server.requests) == 3
            final_input = server.requests[-1]["input"]
            assert final_input[-2] == {"role": "user", "content": "Include a sidebar"}
            assert json.loads(final_input[-1]["content"])["answer"] == "保持结果简洁"
        finally:
            server.release.set()
            server.shutdown()
            worker.join()


def main():
    binary = Path(sys.argv[1]).resolve()
    with LoopbackServer(("127.0.0.1", 0), Provider) as server, \
         tempfile.TemporaryDirectory(prefix="ah-tui-ask-") as directory:
        root = Path(directory)
        db = (root / "test.db").as_uri() + "?mode=ro"
        server.replies = []
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with terminal(binary, root, server, "--model", "fixture") as master:
                receive(master, b"Ask ah anything")
                os.write(master, b"choose\r")
                expect_choices(master)
            with sqlite3.connect(db, uri=True) as conn:
                session, = conn.execute("SELECT id FROM sessions").fetchone()
                assert conn.execute("SELECT status FROM operations").fetchone() == ("input",)
            with terminal(binary, root, server, "--session", session) as master:
                expect_choices(master)
                os.write(master, b"\x1b[B\r")
                receive(master, b"Reply recorded.")
                receive(master, b"Ask ah anything")
                os.write(master, b"custom\r")
                expect_choices(master)
                os.write(master, b"\x1b")
                receive(master, b"waiting_input")
                os.write(master, b"/pending\r")
                expect_choices(master)
                os.write(master, b"\x1b[A\r")
                receive(master, b"waiting_input")
                os.write(master, "我想自己回答\r".encode())
                receive(master, b"Reply recorded.")
                receive(master, b"Ask ah anything")
                os.write(master, b"plain question\r")
                output = receive(master, b"waiting_input")
                assert QUESTION.encode() in output and b"Type a reply" not in output
                os.write(master, b"Free text.\r")
                receive(master, b"Reply recorded.")
                receive(master, b"Ask ah anything")
            with sqlite3.connect(db, uri=True) as conn:
                replies = [json.loads(result) for result, in conn.execute(
                    "SELECT result FROM operations WHERE status='done' ORDER BY seq")]
                assert replies == [CHOICES[1], "我想自己回答", "Free text."], replies
                assert conn.execute("SELECT turn FROM sessions").fetchall() == [(3,)]
            assert server.replies == replies
        finally:
            server.shutdown()
            thread.join()


if __name__ == "__main__":
    main()
    background_reply(Path(sys.argv[1]).resolve())
    network_backoff(Path(sys.argv[1]).resolve())
