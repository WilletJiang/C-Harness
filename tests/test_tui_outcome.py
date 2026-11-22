"""Reconcile a real interrupted tool run from the TUI after reopening it."""
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


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if any(item.get("type") == "function_call_output" for item in request["input"]):
            output = [{"type": "message", "role": "assistant",
                       "content": [{"type": "output_text", "text": "Recovered turn."}]}]
        else:
            output = [{"type": "function_call", "name": "test",
                       "call_id": "test-once", "arguments": "{}"}]
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        event = {"type": "response.completed", "response": {"status": "completed", "output": output}}
        self.wfile.write(("data: " + json.dumps(event) + "\n\n").encode())


def main():
    with LoopbackServer(("127.0.0.1", 0), Provider) as server, \
         tempfile.TemporaryDirectory(prefix="ah-tui-outcome-") as directory:
        root = Path(directory)
        test = root / "test"
        test.write_text("#!/bin/sh\nexec sleep 10\n")
        test.chmod(0o700)
        db = root / "test.db"
        env = dict(os.environ, TERM="xterm-256color", AH_ACCESS_TOKEN="outcome-fixture",
                   AH_TUI_MATH="text", XDG_CONFIG_HOME=str(root / "config"))
        opts = ["--db", db, "--workspace", root, "--no-web-search", "--allow-http-loopback",
                "--tool-timeout-ms", "1000",
                "--endpoint", f"http://127.0.0.1:{server.server_port}/v1/responses"]
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            exercise(Path(sys.argv[1]).resolve(), root, db, env, opts)
        finally:
            server.shutdown()
            thread.join()


def exercise(binary, root, db, env, opts):
    def cli(command, *args, check=True):
        return subprocess.run([binary, command, *opts, *args], cwd=root, env=env, capture_output=True,
                              text=True, check=check, timeout=10)

    session = json.loads(cli("new", "--model", "fixture").stdout)["id"]
    scope = ["--session", session]
    run = cli("run", *scope, "--command-id", "request", "--prompt", "Run the tests")
    operation = json.loads(run.stdout)["pending"]["operation_id"]
    cli("approve", *scope, "--operation", operation)
    stopped = cli("run", *scope, check=False)
    assert stopped.returncode == 1, (stopped.stdout, stopped.stderr)
    with sqlite3.connect(db.as_uri() + "?mode=ro", uri=True) as conn:
        raw, = conn.execute("SELECT json FROM events WHERE type='tool.outcome_unknown'").fetchone()
        evidence = json.loads(raw)
        assert (evidence["signal"], evidence["exit_code"]) == (15, None), evidence

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 110, 0, 0))
    process = subprocess.Popen([binary, "tui", *opts, *scope], cwd=root, env=env, stdin=slave,
                               stdout=slave, stderr=slave, start_new_session=True)
    try:
        receive(master, b"outcome_unknown")
        os.write(master, b"/pending\r")
        receive(master, b"Review saved evidence first")
        os.write(master, b"\r")
        receive(master, b'"signal": 15')
        os.write(master, b"/pending\r")
        receive(master, b"Review saved evidence first")
        os.write(master, b"\x1b[B\r")
        receive(master, b"Describe the failure or interruption")
        with sqlite3.connect(db.as_uri() + "?mode=ro", uri=True) as conn:
            assert conn.execute("SELECT status FROM operations").fetchone() == ("unknown",)
        os.write(master, b"Interrupted before the suite finished.\r")
        receive(master, b"Outcome recorded. Use /resume to continue.")
        with sqlite3.connect(db.as_uri() + "?mode=ro", uri=True) as conn:
            status, error, result = conn.execute("SELECT status,is_error,result FROM operations").fetchone()
            assert (status, error) == ("resolved", 1)
            assert json.loads(result) == {"ok": False, "summary": "Interrupted before the suite finished."}
        os.write(master, b"/resume\r")
        receive(master, b"Recovered turn.")
        with sqlite3.connect(db.as_uri() + "?mode=ro", uri=True) as conn:
            assert conn.execute("SELECT COUNT(*) FROM events WHERE type='tool.started'").fetchone() == (1,)
    finally:
        process.kill()
        process.wait()
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
