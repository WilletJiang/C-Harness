"""Exercise discovery and retry feedback as the model actually sees them."""

import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler
from pathlib import Path

from loopback import LoopbackServer


class Service(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            outputs = {
                v["call_id"]: json.loads(v["output"])
                for v in request["input"]
                if v.get("type") == "function_call_output"
            }
            calls = self.server.case.send(
                [outputs[key] for key in self.server.pending] if self.server.pending else None
            )
        except StopIteration:
            calls = []
        except (AssertionError, KeyError, TypeError, ValueError, OSError) as error:
            self.server.failure = error
            calls = []
        self.server.pending = []
        items = []
        for name, arguments in calls:
            self.server.index += 1
            key = f"call-{self.server.index}"
            self.server.pending.append(key)
            items.append(
                {
                    "type": "function_call",
                    "call_id": key,
                    "namespace": "local",
                    "name": name,
                    "arguments": json.dumps(arguments),
                }
            )
        response = {"status": "completed", "output": items}
        data = (
            "data: " + json.dumps({"type": "response.completed", "response": response}) + "\n\n"
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def find(**args):
    return [("find", args)]


def repeated_pages(ws):
    args = dict(mode="files", path="build", glob="*.o", include_ignored=True, max_results=24)
    (first,) = yield find(**args)
    page = first["result"]
    assert len(page["results"]) == 24 and page["truncated"]
    (second,) = yield find(**args)
    ref = second["result"]
    assert ref["reused"] and "results" not in ref and ref["retrieval_tool"] == "read_result"
    for field in ("snapshot", "next_offset", "total_files", "scan_truncated", "truncated"):
        assert ref[field] == page[field]
    (saved,) = yield [("read_result", {"operation_id": ref["operation_id"]})]
    assert json.loads(saved["result"]["content"]) == page
    (forced,) = yield find(**args, force=True)
    assert forced == first
    (ws / "build" / "000-new.o").touch()
    (changed,) = yield find(**args)
    assert changed["result"]["snapshot"] != page["snapshot"]
    assert {"path": "build/000-new.o"} in changed["result"]["results"]


def discovery(ws):
    args = {"mode": "files", "glob": "*.c", "max_results": 1}
    (first,) = yield find(**args)
    first = first["result"]
    assert first["total_files"] == 3 and first["results"] == [{"path": "a.c"}], first
    assert first["next_offset"] == 1 and not first["scan_truncated"]
    (second,) = yield find(**args, snapshot=first["snapshot"], offset=1)
    assert second["result"]["results"] == [{"path": "src/keep.c"}]
    (ws / "z.c").write_text("new")
    (stale,) = yield find(**args, snapshot=first["snapshot"], offset=2)
    assert stale["is_error"] and stale["result"]["error"] == "stale_snapshot"
    assert stale["result"]["next_offset"] is None
    (searched,) = yield find(path=".", query="needle", glob="*.c")
    assert not searched["result"]["scan_truncated"]
    assert [r["path"] for r in searched["result"]["results"]] == ["src/keep.c"]
    (scoped,) = yield find(mode="files", path="src", glob="*.c")
    assert [r["path"] for r in scoped["result"]["results"]] == ["src/keep.c", "src/sub/file.c"]
    (all_paths,) = yield find(mode="files", path="src", include_ignored=True)
    paths = [r["path"] for r in all_paths["result"]["results"]]
    assert "src/sub/gen/no.c" in paths and "src/binary" in paths
    assert "src/link.c" not in paths and "src/pipe" not in paths
    (override,) = yield find(mode="files", path="build", glob="*.c", include_ignored=True)
    assert override["result"]["results"] == [{"path": "build/ignored.c"}]
    (ignored,) = yield find(mode="files", path="build")
    assert ignored["result"]["results"] == []
    (objects,) = yield find(mode="files", path="build", glob="*.o", include_ignored=True, max_results=1)
    listing = objects["result"]
    assert listing["total_files"] == 2112 and not listing["scan_truncated"]
    (last,) = yield find(
        mode="files",
        path="build",
        glob="*.o",
        include_ignored=True,
        offset=2111,
        snapshot=listing["snapshot"],
    )
    assert len(last["result"]["results"]) == 1 and last["result"]["next_offset"] is None
    # Drift in large ignored/nonmatching populations cannot use up the content file cap.
    (match,) = yield find(path=".", query="needle", glob="*.c", include_ignored=True)
    assert not match["result"]["scan_truncated"] and match["result"]["total_excerpts"] == 2


def edit_conflicts(ws):
    edit = {"path": "a.c", "base_revision": "0" * 64, "edits": [{"old_text": "x", "new_text": "y"}]}
    for expected in (None, 2, None):
        (row,) = yield [("apply_changes", {"files": [edit]})]
        assert row["is_error"] and note(row) == expected, row
        if expected == 2:
            (ws / "a.c").write_text("externally changed")
    for _ in range(2):
        (denied,) = yield [("test", {})]
        assert denied["is_error"] and note(denied) is None


def note(row):
    return row["result"].get("retry_feedback", {}).get("attempts")


def attempts(ws):
    missing = {"path": "missing", "max_lines": 1}
    (first,) = yield [("read_file", missing)]
    assert first["is_error"] and note(first) is None, first
    for count in [2, 3]:
        (repeated,) = yield [("read_file", {"max_lines": 1, "path": "missing"})]
        assert repeated["is_error"] and note(repeated) == count, repeated
        assert "find" in repeated["result"]["retry_feedback"]["next_action"]
    (ws / "missing").write_text("appeared")
    (success,) = yield [("read_file", missing)]
    assert not success["is_error"] and success["result"]["content"] == "appeared"
    (ws / "missing").unlink()
    (again,) = yield [("read_file", missing)]
    assert again["is_error"] and note(again) is None
    # A different deterministic failure has its own advice.
    for count in (None, 2):
        (bad,) = yield [("read_file", {"path": 123})]
        assert bad["is_error"] and note(bad) == count, bad
    for _ in range(2):
        (transient,) = yield [("read_file", {"path": "src/link.c"})]
        assert transient["is_error"] and note(transient) is None
    siblings = yield [("read_file", missing), ("read_file", missing)]
    assert all(r["is_error"] and note(r) is None for r in siblings)
    # End this command on a known failure. A new submission must reset the streak.
    yield [("read_file", missing)]


def new_turn():
    (first,) = yield [("read_file", {"max_lines": 1, "path": "missing"})]
    assert first["is_error"] and note(first) is None
    script = """
      let failures=[];
      for (let i=0;i<3;i++) {
        try { await tools.read_file({path:"other"}); }
        catch(e) { failures.push(JSON.parse(e.message).retry_feedback?.attempts || 1); }
      }
      return failures;
    """
    (flow,) = yield [("flow", {"script": script})]
    assert not flow["is_error"] and flow["result"] == [1, 2, 3], flow


def main(cli):
    with tempfile.TemporaryDirectory() as tmp, LoopbackServer(("127.0.0.1", 0), Service) as server:
        ws = Path(tmp) / "repo"
        for d in ["build", "src/sub/gen", ".git"]:
            (ws / d).mkdir(parents=True)
        (ws / ".gitignore").write_text("/build/\n*.c\n!a.c\n!z.c\n!src/**/*.c\n")
        (ws / "src/.gitignore").write_text("**/gen/\nignored.c\n")
        for name in [
            "a.c",
            "src/keep.c",
            "src/sub/file.c",
            "src/ignored.c",
            "src/sub/gen/no.c",
            "build/ignored.c",
        ]:
            (ws / name).write_text("needle" if name in ("src/keep.c", "build/ignored.c") else "ok")
        (ws / "src/binary").write_bytes(b"\x00\xff")
        (ws / "src/link.c").symlink_to("keep.c")
        os.mkfifo(ws / "src/pipe")
        for i in range(2112):
            (ws / "build" / f"{i}.o").touch()
        server.index = 0
        server.failure = None
        threading.Thread(target=server.serve_forever, daemon=True).start()

        def run(cmd, *opts):
            p = subprocess.run(
                [
                    cli,
                    cmd,
                    "--db",
                    str(Path(tmp) / "s.db"),
                    "--workspace",
                    str(ws),
                    "--allow-http-loopback",
                    "--endpoint",
                    f"http://127.0.0.1:{server.server_port}/r",
                    *opts,
                ],
                env=dict(os.environ, AH_ACCESS_TOKEN="offline"),
                text=True,
                capture_output=True,
                timeout=20,
                check=False,
            )
            assert p.returncode == 0, (p.stderr, p.stdout)
            return json.loads(p.stdout)

        try:
            sid = run("new", "--model", "fixture")["id"]
            cases = (discovery(ws), attempts(ws), new_turn(), edit_conflicts(ws), repeated_pages(ws))
            for i, case in enumerate(cases):
                server.pending = []
                server.case = case
                state = run("run", "--session", sid, "--command-id", str(i), "--prompt", "check")
                while state["state"] == "waiting_approval":
                    pending = state["pending"]
                    run(
                        "deny" if pending["name"] == "test" else "approve",
                        "--session",
                        sid,
                        "--operation",
                        pending["operation_id"],
                    )
                    state = run("run", "--session", sid)
                if server.failure:
                    raise server.failure
                assert state["state"] == "completed"
        finally:
            server.shutdown()


if __name__ == "__main__":
    main(sys.argv[1])
