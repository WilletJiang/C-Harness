"""Exercise workspace auto-approval through the real terminal frontend."""
from contextlib import contextmanager
import fcntl
import hashlib
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


def call(name, ident, arguments):
    return {"type": "function_call", "call_id": ident, "name": name,
            "arguments": json.dumps(arguments)}


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        item = request["input"][-1]
        if item.get("type") == "function_call_output":
            output = [{"type": "message", "role": "assistant",
                       "content": [{"type": "output_text", "text": "Turn finished."}]}]
        elif item["content"] == "Guarded edits":
            output = [
                call("write_file", "new-note", {"path": "note.txt", "content": "new", "expect_absent": True}),
                call("apply_changes", "patch-note", {"files": [
                    {"path": "existing.txt", "base_revision": hashlib.sha256(b"before\n").hexdigest(),
                     "edits": [{"old_text": "before", "new_text": "after"}]},
                ]}),
                call("write_file", "protected", {"path": ".env", "content": "protected", "expect_absent": True}),
            ]
        elif item["content"] == "Process check":
            output = [call("test", "process", {})]
        elif item["content"] == "List files":
            output = [call("exec", "list-files", {"argv": ["/bin/ls", "-a"]})]
        else:
            prompt = item["content"]
            output = [call("write_file", prompt, {"path": prompt + ".txt", "content": "pending",
                                                  "expect_absent": True})]
        event = {"type": "response.completed", "response": {"status": "completed", "output": output}}
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        self.wfile.write(("data: " + json.dumps(event) + "\n\n").encode())


@contextmanager
def terminal(binary, root, server, *args):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 110, 0, 0))
    env = dict(os.environ, TERM="xterm-256color", AH_ACCESS_TOKEN="approval-fixture",
               AH_TUI_MATH="text", XDG_CONFIG_HOME=str(root / "config"))
    endpoint = f"http://127.0.0.1:{server.server_port}/v1/responses"
    process = subprocess.Popen(
        [binary, "tui", "--db", root / "test.db", "--workspace", root, "--endpoint", endpoint,
         "--allow-http-loopback", "--no-web-search", *args],
        cwd=root, env=env, stdin=slave, stdout=slave, stderr=slave, start_new_session=True,
    )
    try:
        receive(master, b"Ask ah anything")
        yield master
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
        os.close(master)
        os.close(slave)


def submit(master, text, expected):
    os.write(master, text.encode() + b"\r")
    out = receive(master, expected)
    if expected == b"Turn finished.":
        out += receive(master, b"Ask ah anything")
    return out


def main():
    binary = Path(sys.argv[1]).resolve()
    with LoopbackServer(("127.0.0.1", 0), Provider) as server, \
         tempfile.TemporaryDirectory(prefix="ah-tui-approval-") as directory:
        root = Path(directory)
        db = root / "test.db"
        (root / "existing.txt").write_text("before\n")
        prefs = root / "config" / "ah" / "tui-preferences.json"
        prefs.parent.mkdir(parents=True, mode=0o700)
        prefs.write_text(json.dumps({"workspace": str(root.resolve()), "yolo": True, "exec": True,
                                     "endpoint": f"http://127.0.0.1:{server.server_port}/v1/responses"}))
        prefs.chmod(0o600)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with terminal(binary, root, server, "--model", "fixture") as master:
                submit(master, "List files", b"waiting_approval")
                submit(master, "/approve", b"Turn finished.")
                submit(master, "default", b"waiting_approval")
                submit(master, "/deny", b"Turn finished.")
                submit(master, "/auto-approve", b"Auto-approve on")
                submit(master, "Guarded edits", b"waiting_approval")
                assert (root / "note.txt").read_text() == "new"
                assert (root / "existing.txt").read_text() == "after\n"
                assert not (root / ".env").exists()
                submit(master, "/deny", b"Turn finished.")
                submit(master, "Process check", b"waiting_approval")
                submit(master, "/deny", b"Turn finished.")
                submit(master, "/auto-approve", b"Auto-approve off")
                submit(master, "disabled", b"waiting_approval")
                submit(master, "/deny", b"Turn finished.")
                submit(master, "/auto-approve", b"Auto-approve on")
                submit(master, "/quit", b"\x1b[?2004l")
            with sqlite3.connect(db) as conn:
                session, = conn.execute("SELECT id FROM sessions").fetchone()
                approvals = conn.execute(
                    "SELECT o.call_id,e.type FROM events e JOIN operations o ON e.operation_id=o.id "
                    "WHERE e.type IN ('approval.auto','approval.requested') ORDER BY e.seq"
                ).fetchall()
                assert approvals == [("list-files", "approval.requested"),
                                     ("default", "approval.requested"),
                                     ("new-note", "approval.auto"), ("patch-note", "approval.auto"),
                                     ("protected", "approval.requested"), ("process", "approval.requested"),
                                     ("disabled", "approval.requested")], approvals
                assert conn.execute("SELECT COUNT(*) FROM events WHERE type='input.submitted'").fetchone() == (5,)
                result, = conn.execute("SELECT result FROM operations WHERE call_id='list-files'").fetchone()
                assert "existing.txt" in json.loads(result)["stdout"]
            with terminal(binary, root, server, "--session", session) as master:
                submit(master, "reopened", b"waiting_approval")
                assert not (root / "reopened.txt").exists()
                submit(master, "/pending", b"Pending operation")
                os.write(master, b"A")
                receive(master, b"Turn finished.")
                receive(master, b"Ask ah anything")
                assert (root / "reopened.txt").read_text() == "pending"
                submit(master, "immediate", b"Turn finished.")
                assert (root / "immediate.txt").read_text() == "pending"
                submit(master, "/quit", b"\x1b[?2004l")
            with terminal(binary, root, server, "--session", session) as master:
                submit(master, "persists", b"Turn finished.")
                assert (root / "persists.txt").read_text() == "pending"
                submit(master, "/yolo off", b"YOLO mode off")
                submit(master, "opted-out", b"waiting_approval")
                assert not (root / "opted-out.txt").exists()
        finally:
            server.shutdown()
            thread.join()


if __name__ == "__main__":
    main()
