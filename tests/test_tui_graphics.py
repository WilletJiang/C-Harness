"""Drive the real TUI through a PTY with an offline Responses fixture."""
import base64
import fcntl
import http.server
import json
import os
from pathlib import Path
import pty
import re
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


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.prompts.append(request["input"][-1]["content"])
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        answer = (
            "Formula begins.\n\n"
            r"$$\widehat{f}(\xi)=\int_{-\infty}^{\infty}f(x)e^{-2\pi ix\xi}\,\mathrm{d}x$$"
            "\n\nUnknown formula follows.\n\n"
            r"$$\notarealcommand{x}$$"
            "\n\nFormula ends."
        )
        output = [{"type": "message", "role": "assistant",
                   "content": [{"type": "output_text", "text": answer}]}]
        payload = {"type": "response.completed",
                   "response": {"status": "completed", "output": output}}
        self.wfile.write(("data: " + json.dumps(payload) + "\n\n").encode())


class StreamingProvider(Provider):
    blocks = "\n\n".join(f"**scrollback-block-{i:02}**" for i in range(17)) + "\n\n"

    def do_GET(self):
        if self.path != "/v1/models" or self.headers.get("Authorization") != "Bearer graphics-fixture":
            self.send_error(403)
            return
        catalog = {"data": [
            {"id": "fixture"},
            {"id": "picked-model", "supported_reasoning_levels": [
                {"effort": "low"}, {"effort": "high"},
            ]},
        ]}
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(catalog).encode())

    def event(self, payload):
        self.wfile.write(("data: " + json.dumps(payload) + "\n\n").encode())
        self.wfile.flush()

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append(body)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        if len(self.server.requests) == 1:
            self.event({"type": "response.output_text.delta", "delta": self.blocks})
            if not self.server.release.wait(10):
                return
            text = self.blocks + "Before the tool"
            tools = [{"type": "function_call", "call_id": "find-once", "name": "find",
                      "arguments": '{"mode":"files"}'}]
        else:
            text = "Final answer after the tool"
            tools = []
        message = {"type": "message", "role": "assistant",
                   "content": [{"type": "output_text", "text": text}]}
        self.event({"type": "response.completed",
                    "response": {"status": "completed", "output": [message, *tools]}})


def receive(fd, marker, timeout=10):
    output = b""
    end = time.monotonic() + timeout
    while marker not in output and time.monotonic() < end:
        if select.select([fd], [], [], .05)[0]:
            output += os.read(fd, 65536)
    assert marker in output, output[-1800:]
    return output


def assert_png(output):
    image = []
    chunks = re.findall(rb"\x1b_G([^\x1b]+)\x1b\\", output)
    for chunk in chunks:
        metadata, data = chunk.split(b";", 1)
        if b"a=T," not in metadata and not image:
            continue
        assert len(data) <= 4096
        if image:
            assert metadata in (b"m=0", b"m=1"), metadata
        image.append(base64.b64decode(data, validate=True))
        if b"m=0" in metadata:
            break
    png = b"".join(image)
    assert png.startswith(b"\x89PNG\r\n\x1a\n")
    width, height = struct.unpack(">II", png[16:24])
    assert width > 0 and height > 20 and png[25] == 6
    rows = (height + 19) // 20
    assert f"\033[{rows}A\r".encode() in output
    assert f"\r\033[{rows}B".encode() in output


def exercise(binary, server, mode, replies, expect_png, probe=True, tmux=False):
    with tempfile.TemporaryDirectory(prefix="ah-tui-graphics-") as directory:
        root = Path(directory)
        env = dict(os.environ, TERM="xterm-256color", AH_ACCESS_TOKEN="graphics-fixture",
                   XDG_CONFIG_HOME=str(root / "config"))
        env.pop("TMUX", None)
        if tmux:
            env["TMUX"] = "offline-fixture"
        env.pop("AH_TUI_MATH", None)
        if mode:
            env["AH_TUI_MATH"] = mode
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 27, 92, 0, 0))
        before = termios.tcgetattr(slave)
        endpoint = f"http://127.0.0.1:{server.server_port}/responses"
        process = subprocess.Popen(
            [binary, "tui", "--db", root / "test.db", "--model", "fixture",
             "--allow-http-loopback", "--endpoint", endpoint, "--no-web-search"],
            stdin=slave, stdout=slave, stderr=slave, env=env, start_new_session=True,
        )
        try:
            startup = receive(master, b"Ask ah anything")
            assert (b"a=q,t=d" in startup) == probe, startup
            if replies:
                query = re.search(rb"\x1b_Gi=(\d+),a=q,", startup)
                assert query
                os.write(master, b"\x1b_Gi=" + query.group(1) + b";OK\x1b\\")
            for reply in replies:
                os.write(master, reply)
            os.write(master, b"Check the Fourier transform\r")
            output = receive(master, b"Formula ends.")
            assert server.prompts[-1] == "Check the Fourier transform"
            assert b"\\notarealcommand{x}" in output
            if expect_png:
                assert_png(output)
                assert "∫".encode() not in output
            else:
                assert b"a=T," not in output
                assert "∫".encode() in output
            process.terminate()
            end = time.monotonic() + 8
            while process.poll() is None and time.monotonic() < end:
                if select.select([master], [], [], .05)[0]:
                    os.read(master, 65536)
            assert process.poll() == 0
            assert termios.tcgetattr(slave) == before
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)


def exercise_stream_and_model_options(binary):
    with LoopbackServer(("127.0.0.1", 0), StreamingProvider) as server, \
         tempfile.TemporaryDirectory(prefix="ah-tui-stream-") as directory:
        server.requests = []
        server.release = threading.Event()
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        root = Path(directory)
        env = dict(os.environ, TERM="xterm-256color", AH_ACCESS_TOKEN="graphics-fixture",
                   AH_TUI_MATH="text", XDG_CONFIG_HOME=str(root / "config"))
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 27, 92, 0, 0))
        endpoint = f"http://127.0.0.1:{server.server_port}/v1/responses"
        process = subprocess.Popen(
            [binary, "tui", "--db", root / "test.db", "--model", "fixture", "--workspace", root,
             "--allow-http-loopback", "--endpoint", endpoint, "--no-web-search"],
            stdin=slave, stdout=slave, stderr=slave, env=env, start_new_session=True,
        )
        try:
            receive(master, b"Ask ah anything")
            os.write(master, b"/model\r")
            receive(master, b"Enter to choose effort")
            os.write(master, b"\x1b[B\r")
            assert b"picked-model" in receive(master, b"Enter starts a new session")
            with sqlite3.connect((root / "test.db").as_uri() + "?mode=ro", uri=True) as db:
                assert db.execute("SELECT COUNT(*) FROM sessions").fetchone() == (1,)
            os.write(master, b"\x1b")
            receive(master, b"Enter to choose effort")
            os.write(master, b"\r")
            assert b"picked-model" in receive(master, b"Enter starts a new session")
            os.write(master, b"\x1b[B\x1b[B\r")
            assert b"picked-model" in receive(master, b"Ask ah anything")
            os.write(master, b"Check the workspace\r")
            early = receive(master, b"scrollback-block-16")
            assert b"scrollback-block-00" in early
            assert b"**scrollback-block-00" not in early
            assert len(server.requests) == 1
            server.release.set()
            final = receive(master, b"Final answer after the tool")
            assert b"Final answer after the tool" in final
            assert b"scrollback-block-00" not in final
            receipt = re.search(rb"find +\x1b\[0m +mode=files", final)
            assert receipt and final.index(b"Before the tool") < receipt.start()
            assert len(server.requests) == 2
            for body in server.requests:
                assert body["model"] == "picked-model"
                assert body["reasoning"] == {"effort": "high"}
            assert any(item.get("type") == "function_call_output"
                       for item in server.requests[1]["input"])
            os.write(master, b"/model\r")
            receive(master, b"Enter to choose effort")
            os.write(master, b"\r")
            receive(master, b"Applies next turn")
            os.write(master, b"\x1b[A\r")
            receive(master, b"Reasoning effort: low")
            os.write(master, b"Follow up\r")
            receive(master, b"Final answer after the tool")
            assert server.requests[2]["reasoning"] == {"effort": "low"}
            with sqlite3.connect((root / "test.db").as_uri() + "?mode=ro", uri=True) as db:
                assert db.execute("SELECT COUNT(*) FROM sessions").fetchone() == (2,)
            os.write(master, b"/sessions\r")
            listing = receive(master, b"Choose a conversation")
            if b"Last message" not in listing:
                listing += receive(master, b"Last message", 5)
            if b"Empty chat" not in listing:
                listing += receive(master, b"Empty chat", 5)
            assert b"Check the workspace" in listing and b"Empty chat" in listing
            os.write(master, b"\x1b[B\r")
            assert b"fixture" in receive(master, b"Ask ah anything")
        finally:
            server.release.set()
            process.kill()
            process.wait()
            os.close(master)
            os.close(slave)
            server.shutdown()
            thread.join()


def main():
    size = b"\x1b[6;20;10t"
    colors = (b"\x1b]10;rgb:e7e7/ebeb/f3f3\x1b", b"\\\x1b]11;rgb:1818/1a1a/1f1f\x07")
    with LoopbackServer(("127.0.0.1", 0), Provider) as server:
        server.prompts = []
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            exercise(sys.argv[1], server, None, (size, *colors), True)
            exercise(sys.argv[1], server, None, (size,), False)
            exercise(sys.argv[1], server, None, (), False)
            exercise(sys.argv[1], server, "dark", (size,), True)
            exercise(sys.argv[1], server, "text", (), False, probe=False)
            exercise(sys.argv[1], server, None, (), False, probe=False, tmux=True)
        finally:
            server.shutdown()
            thread.join()
    exercise_stream_and_model_options(sys.argv[1])


if __name__ == "__main__":
    main()
