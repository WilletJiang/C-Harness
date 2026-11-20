"""One model-issued flow across real CLI restarts, per-child approval and file faults."""

import json
import os
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler
from pathlib import Path

from loopback import LoopbackServer

CLI = sys.argv[1]
requests = []
script = """
const found = await tools.find({path: ".", glob: "*.txt", query: "before"});
await Promise.all(found.results.map(row => tools.apply_changes({files: [{
  path: row.path, base_revision: row.revision,
  edits: [{old_text: "before", new_text: "after"}]
}]})));
const result = await tools.test({});
return {edited: found.results.length, passed: result.ok};
"""


class Service(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        requests.append(body)
        receipts = [v["output"] for v in body["input"] if v.get("type") == "function_call_output"]
        name, args = None, {}
        if body["model"] == "direct":
            if not receipts:
                name, args = "find", {"path": ".", "glob": "*.txt", "query": "before"}
            elif len(receipts) == 1:
                result = json.loads(receipts[-1])
                rows = result["result"]["results"]
                name, args = (
                    "apply_changes",
                    {
                        "files": [
                            {
                                "path": r["path"],
                                "base_revision": r["revision"],
                                "edits": [{"old_text": "before", "new_text": "after"}],
                            }
                            for r in rows
                        ]
                    },
                )
            elif len(receipts) == 2:
                name = "test"
        elif not receipts:
            source = script
            if body["model"] == "collision":
                source = source.replace("found.results.map", "[found.results[0], found.results[0]].map")
            name, args = "flow", {"script": source}
        output = (
            [
                {
                    "type": "function_call",
                    "name": name,
                    "namespace": "local",
                    "call_id": f"call{len(receipts)}",
                    "arguments": json.dumps(args),
                }
            ]
            if name
            else []
        )
        response = {"id": "r", "status": "completed", "output": output}
        data = (
            "data: " + json.dumps({"type": "response.completed", "response": response}) + "\n\n"
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def exercise(endpoint, scenario):
    direct = scenario == "direct"
    conflict = scenario in {"stale", "collision", "denied"}
    with tempfile.TemporaryDirectory() as tmp:
        db = Path(tmp) / "state.db"
        ws = Path(tmp) / "project"
        ws.mkdir()
        (ws / "a.txt").write_text("before\n")
        if scenario == "alias":
            os.link(ws / "a.txt", ws / "b.txt")
        else:
            (ws / "b.txt").write_text("before\n")
        (ws / "test").write_text(
            "#!/bin/sh\nset -eu\ngrep -q after a.txt\ngrep -q after b.txt\necho pass >> runs\n"
        )
        (ws / "test").chmod(0o700)
        metrics = {}

        def invoke(command, *args):
            result = subprocess.run(
                [
                    CLI,
                    command,
                    "--db",
                    str(db),
                    "--workspace",
                    str(ws),
                    "--allow-http-loopback",
                    "--endpoint",
                    endpoint,
                    "--events",
                    *args,
                ],
                env=dict(os.environ, AH_ACCESS_TOKEN="loopback"),
                text=True,
                check=False,
                capture_output=True,
                timeout=15,
            )
            assert result.returncode == 0, (command, result.stderr, result.stdout)
            lines = [json.loads(line) for line in result.stdout.splitlines()]
            for item in lines:
                data = item.get("data", {})
                if item.get("event") == "runtime.metrics" and data["phase"] == "tool":
                    metrics[item["operation"]] = (
                        data["started_us"],
                        data["started_us"] + data["duration_us"],
                    )
            return lines[-1]

        def operations():
            with sqlite3.connect(db) as conn:
                return conn.execute(
                    "SELECT name,status,is_error,result FROM operations ORDER BY seq"
                ).fetchall()

        start = len(requests)
        started = time.monotonic()
        sid = invoke("new", "--model", scenario)["id"]
        state = invoke("run", "--session", sid, "--prompt", "update", "--command-id", "u")
        paths = (
            ["a.txt"]
            if direct
            else ["a.txt", "a.txt"]
            if scenario == "collision"
            else ["a.txt", "b.txt"]
        )
        for index, path in enumerate(paths):
            assert state["state"] == "waiting_approval", (state, operations())
            pending = state["pending"]
            assert (
                pending["name"] == "apply_changes" and pending["arguments"]["files"][0]["path"] == path
            )
            assert (ws / "a.txt").read_text() == (
                "after\n" if scenario in {"collision", "alias"} and index else "before\n"
            )
            if scenario == "stale" and path == "b.txt":
                (ws / path).write_text("changed outside the harness\n")
            decision = "deny" if scenario == "denied" and index else "approve"
            invoke(decision, "--session", sid, "--operation", pending["operation_id"])
            state = invoke("run", "--session", sid)
        if not conflict:
            assert state["pending"]["name"] == "test", (state, operations())
            assert not (ws / "runs").exists()
            invoke("approve", "--session", sid, "--operation", state["pending"]["operation_id"])
            state = invoke("run", "--session", sid)
        assert state["state"] == "completed", (state, operations())
        assert len(requests) - start == (4 if direct else 2)
        if direct:
            assert (ws / "runs").read_text() == "pass\n"
            return time.monotonic() - started
        outputs = [v for v in requests[-1]["input"] if v.get("type") == "function_call_output"]
        assert len(outputs) == 1 and outputs[0]["call_id"] == "call0"
        rows = operations()
        assert all(row[1] == "done" for row in rows)
        edits = [row for row in rows if row[0] == "apply_changes"]
        assert len(edits) == 2 and sum(row[2] for row in edits) == int(conflict), rows
        assert (ws / "a.txt").read_text() == "after\n"
        with sqlite3.connect(db) as conn:
            events = conn.execute(
                "SELECT type,operation_id FROM events WHERE operation_id IN "
                "(SELECT id FROM operations WHERE name='apply_changes') ORDER BY seq"
            ).fetchall()
        starts = [i for i, (t, _) in enumerate(events) if t == "tool.started"]
        if scenario in {"normal", "stale"}:
            assert len(starts) == 2 and max(starts) < next(
                i for i, (t, _) in enumerate(events) if t == "tool.completed"
            )
        if scenario == "normal":
            spans = [metrics[events[i][1]] for i in starts]
            assert max(s[0] for s in spans) < min(s[1] for s in spans), spans
        if scenario in {"collision", "alias"}:
            assert (
                len(starts) == 2
                and min(i for i, (t, _) in enumerate(events) if t == "tool.completed") < starts[1]
            )
        if conflict:
            assert rows[0][2] and all(row[0] != "test" for row in rows)
            assert (ws / "b.txt").read_text() == (
                "changed outside the harness\n" if scenario == "stale" else "before\n"
            )
            assert not (ws / "runs").exists()
        else:
            assert json.loads(rows[0][3]) == {"edited": 2, "passed": True}, rows
            assert (ws / "runs").read_text() == "pass\n"
        return time.monotonic() - started


with LoopbackServer(("127.0.0.1", 0), Service) as server:
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for scenario in ("normal", "stale", "collision", "alias", "denied", "direct"):
            count = len(requests)
            elapsed = exercise(f"http://127.0.0.1:{server.server_port}/responses", scenario)
            print(f"{scenario}: {len(requests) - count} scripted model requests, {elapsed:.3f}s")

    finally:
        server.shutdown()
