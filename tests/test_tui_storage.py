"""Exercise two real terminal windows using a single workspace."""
import contextlib
import fcntl
import hashlib
import http.server
import json
import os
from pathlib import Path
import pty
import select
import sqlite3
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

from loopback import LoopbackServer
from test_tui_graphics import receive


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        text = request["input"][-1]["content"]
        if text in ("First window", "Second window"):
            self.server.concurrent.wait()
        message = {"type": "message", "role": "assistant",
                   "content": [{"type": "output_text", "text": f"Finished {text}"}]}
        payload = {"type": "response.completed", "response": {"status": "completed", "output": [message]}}
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        self.wfile.write(("data: " + json.dumps(payload) + "\n\n").encode())


@contextlib.contextmanager
def terminal(binary, workspace, endpoint, env, *args):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 110, 0, 0))
    process = subprocess.Popen(
        [binary, "tui", "--endpoint", endpoint, "--allow-http-loopback", "--no-web-search", *args],
        cwd=workspace, env=env, stdin=slave, stdout=slave, stderr=slave, start_new_session=True,
    )
    try:
        yield master, process
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
        os.close(master)
        os.close(slave)


def send(master, text):
    os.write(master, text.encode() + b"\r")


def wait_ready(master, reply):
    if b"Ask ah anything" not in receive(master, reply):
        receive(master, b"Ask ah anything")


def sessions(path):
    with contextlib.closing(sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)) as conn:
        return conn.execute("SELECT id,turn FROM sessions").fetchall()

def wait_exit(master, process):
    deadline = time.monotonic() + 5
    while process.poll() is None and time.monotonic() < deadline:
        if select.select([master], [], [], .05)[0]:
            os.read(master, 65536)
    assert process.poll() == 0


def exercise(binary, root, endpoint):
    workspace = root / "workspace"
    workspace.mkdir()
    alias = root / "alias"
    alias.symlink_to(workspace, target_is_directory=True)
    xdg = root / "state"
    env = dict(os.environ, TERM="xterm-256color", AH_TUI_MATH="text",
               AH_ACCESS_TOKEN="storage-fixture", XDG_STATE_HOME=str(xdg),
               XDG_CONFIG_HOME=str(root / "config"), CODEX_HOME=str(root / "codex"))
    digest = hashlib.sha256(os.fsencode(workspace.resolve())).hexdigest()
    directory = xdg / "ah" / "tui" / digest
    first_db, second_db = directory / "window-1.db", directory / "window-2.db"
    with terminal(binary, workspace, endpoint, env, "--model", "fixture") as (first, first_process):
        receive(first, b"Ask ah anything")
        with terminal(binary, workspace, endpoint, env, "--model", "fixture",
                      "--workspace", alias) as (second, _):
            receive(second, b"Ask ah anything")
            assert sorted(directory.glob("*.db")) == [first_db, second_db]
            assert directory.stat().st_mode & 0o077 == 0
            assert not list(workspace.iterdir())
            assert sessions(first_db) == sessions(second_db) == []
            send(first, "First window")
            send(second, "Second window")
            wait_ready(first, b"Finished First window")
            wait_ready(second, b"Finished Second window")
            [(first_id, turn)] = sessions(first_db)
            [(second_id, second_turn)] = sessions(second_db)
            assert turn == second_turn == 1 and first_id != second_id
            send(second, "/sessions")
            listing = receive(second, b"Choose a conversation")
            for title in (b"First window", b"Second window"):
                if title not in listing:
                    listing += receive(second, title)
            assert b"First window" in listing and b"Second window" in listing
            os.write(second, b"\x1b[B\r")
            # The picker remains open after an unsuccessful handoff.
            os.write(second, b"q")
            receive(second, b"Another window is using this conversation's database")
            send(first, "/quit")
            wait_exit(first, first_process)

            send(second, "/sessions")
            receive(second, b"First window")
            os.write(second, b"\x1b[B\r")
            wait_ready(second, b"Finished First window")
            send(second, "Follow up")
            wait_ready(second, b"Finished Follow up")
            assert sessions(first_db) == [(first_id, 2)]
            assert sessions(second_db) == [(second_id, 1)]

            with terminal(binary, workspace, endpoint, env, "--session", second_id) as (reopened, _):
                wait_ready(reopened, b"Finished Second window")
            assert sorted(directory.glob("*.db")) == [first_db, second_db]
            assert not list(workspace.iterdir())


def main():
    binary = Path(sys.argv[1]).resolve()
    with LoopbackServer(("127.0.0.1", 0), Provider) as server, \
         tempfile.TemporaryDirectory(prefix="ah-tui-storage-") as directory:
        server.concurrent = threading.Barrier(2, timeout=6)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            endpoint = f"http://127.0.0.1:{server.server_port}/v1/responses"
            exercise(binary, Path(directory), endpoint)
        finally:
            server.shutdown()
            worker.join()


if __name__ == "__main__":
    main()
