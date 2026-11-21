"""Native and flow entry points against an instrumented Keenable REST fixture."""

import json
import os
import signal
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
from collections import Counter
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler
from pathlib import Path

from loopback import LoopbackServer


class Fixture(LoopbackServer):
    request_queue_size = 256
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.lock = threading.Lock()
        self.reset()

    def reset(self, *, latency=0.045):
        self.latency = latency
        self.starts, self.calls, self.models = [], Counter(), []
        self.active = self.peak = 0
        self.action = None
        self.result = None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def send(self, code, data, headers=(), mime="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        for k, v in headers:
            self.send_header(k, str(v))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == "/v1/search/public":
            self.search(req)
            return
        assert self.path == "/model"
        f = self.server
        f.models.append(req)
        outputs = [
            i["output"] for i in req["input"] if i.get("type") == "function_call_output"
        ]
        items = []
        if outputs:
            f.result = json.loads(outputs[-1])
        elif f.action:
            name, args = f.action
            items = [
                {
                    "type": "function_call",
                    "name": name,
                    "call_id": "q",
                    "arguments": json.dumps(args),
                }
            ]
        response = {"id": "r", "status": "completed", "output": items}
        payload = (
            "data: "
            + json.dumps({"type": "response.completed", "response": response})
            + "\n\n"
        )
        self.send(200, payload.encode(), mime="text/event-stream")

    def search(self, args):
        assert self.headers.get_content_type() == "application/json"
        assert self.headers["X-Keenable-Title"] == "C Harness"
        assert not any(
            h in self.headers for h in ("Authorization", "X-API-Key", "Cookie")
        )
        assert set(args) == {"query", "max_results", "snippet_max_length"}
        assert args["snippet_max_length"] == 512
        q = args["query"]
        f = self.server
        now = time.monotonic()
        with f.lock:
            f.starts.append((q, now))
            f.calls[q] += 1
            attempt = f.calls[q]
            f.active += 1
            f.peak = max(f.peak, f.active)
        time.sleep(0.6 if q == "stall" else f.latency * (1.4 if q == "slow" else 1))
        headers = []
        code = 200
        rows = [
            {
                "title": q,
                "url": f"https://example.org/{i}",
                "snippet": f"hit {i}",
                "published_at": "2026-01-01T00:00:00Z",
            }
            for i in range(args["max_results"])
        ]
        data = json.dumps({"query": q, "results": rows}).encode()
        if q in {"retry", "exhaust", "cooldown"} and (attempt == 1 or q == "exhaust"):
            code = 429
            headers.append(("Retry-After", "60" if q == "cooldown" else "1"))
        elif q == "last_credit":
            until = datetime.now(timezone.utc) + timedelta(hours=1)
            headers += [
                ("X-RateLimit-Remaining", "0"),
                ("X-RateLimit-Reset", until.strftime("%Y-%m-%dT%H:%M:%S.123Z")),
            ]
        elif q == "overload" and attempt == 1:
            code = 529
            data = b'{"retry_after_seconds":1}'
        elif q == "bad":
            data = b"{"
        elif q == "missing":
            code = 404
        elif q == "credential":
            code = 401
        elif q == "redirect":
            code = 302
            headers.append(("Location", "/must-not-follow"))
        elif q == "empty":
            data = b'{"results":[]}'
        elif q == "fallback":
            data = b'{"results":[{"title":"short","url":"https://example.org","snippet":"","description":"from description","published_at":null}]}'
        elif q == "oversized":
            data = b"x" * (512 * 1024 + 1)
        with f.lock:
            f.active -= 1
        self.send(code, data, headers)


@contextmanager
def upstream():
    with Fixture() as f:
        thread = threading.Thread(target=f.serve_forever, daemon=True)
        thread.start()
        try:
            yield f
        finally:
            f.shutdown()
            thread.join()


def run(
    cli, f, action=None, *, disabled=False, timeout=30000, live=False, interrupt=False
):
    f.action = action
    port = f.server_port
    env = dict(os.environ, AH_ACCESS_TOKEN="fixture-model-token")
    # Neither search credentials nor model credentials are sent to Keenable.
    env.pop("KEENABLE_API_KEY", None)
    with tempfile.TemporaryDirectory() as tmp:
        db = str(Path(tmp) / "db")
        common = [
            "--db",
            db,
            "--allow-http-loopback",
            "--endpoint",
            f"http://127.0.0.1:{port}/model",
            "--model-tool-bytes",
            "524288",
            "--tool-timeout-ms",
            str(timeout),
        ]
        if not live:
            common += ["--search-endpoint", f"http://127.0.0.1:{port}/v1/search/public"]
        if disabled:
            common.append("--no-web-search")

        def invoke(*args):
            p = subprocess.run(
                [cli, *args, *common],
                check=False,
                capture_output=True,
                text=True,
                env=env,
                timeout=timeout / 1000 + 15,
            )
            assert p.returncode == 0, (p.stdout[-500:], p.stderr)
            return json.loads(p.stdout)

        sid = invoke("new", "--model", "search-test")["id"]
        start = time.monotonic()
        cmd = ["run", "--session", sid, "--prompt", "research", "--command-id", "1"]
        if interrupt:
            with subprocess.Popen(
                [cli, *cmd, *common],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=env,
            ) as proc:
                end = time.monotonic() + 4
                while not f.calls["stall"] and time.monotonic() < end:
                    time.sleep(0.005)
                assert f.calls["stall"] and proc.poll() is None
                proc.send_signal(signal.SIGINT)
                _, stderr = proc.communicate(timeout=3)
                assert proc.returncode == 130, stderr
            with sqlite3.connect(db) as conn:
                value = conn.execute(
                    "select result from operations where name='web_run'"
                ).fetchone()
            result = json.loads(value[0])["results"]
            assert result[0]["hits"] and [v["error"] for v in result[1:]] == [
                "cancelled",
                "cancelled",
            ], result
            assert f.calls["unsent"] == 0
            return
        invoke(*cmd)
        elapsed = time.monotonic() - start
        assert len(f.models) == (2 if action else 1)
        with sqlite3.connect(db) as conn:
            ops = conn.execute(
                "SELECT name,status FROM operations ORDER BY seq"
            ).fetchall()
            assert all(status == "done" for _, status in ops), ops
        return elapsed, ops


def flow(queries, count=1):
    args = json.dumps({"queries": queries, "count": count}, ensure_ascii=False)
    return "flow", {"script": f"return await tools.web_run({args})"}


def rows(f):
    assert not f.result["is_error"], f.result
    return f.result["result"]["results"]


def validate(cli, f):
    run(cli, f, live=True)
    assert "web_run" in json.dumps(f.models[0]["tools"])
    f.reset()
    run(cli, f, disabled=True)
    assert "web_run" not in json.dumps(f.models[0]["tools"])
    f.reset()
    f.latency = 0.23
    terms = [
        "slow",
        "雪 & ? +",
        "empty",
        "bad",
        "missing",
        "slow",
        "redirect",
        "oversized",
    ]
    run(cli, f, ("web_run", {"queries": terms, "count": 2}))
    result = rows(f)
    assert [r["query"] for r in result] == terms
    assert (
        result[0] == result[5] and f.calls["slow"] == 1 and 1 < f.peak <= len(f.calls)
    )
    assert result[1]["hits"][1] == {
        "title": terms[1],
        "url": "https://example.org/1",
        "snippet": "hit 1",
        "published_at": "2026-01-01T00:00:00Z",
    }
    assert result[2]["hits"] == [] and result[3]["error"] == "invalid_response"
    assert [result[i]["http_status"] for i in (4, 6)] == [404, 302]
    assert result[7]["error"] == "response_too_large"
    f.reset()
    queries = [f"topic {i}" for i in range(100)]
    f.latency = 0.35
    _, ops = run(cli, f, flow(queries))
    assert ops == [("flow", "done"), ("web_run", "done")]
    assert [r["hits"][0]["title"] for r in rows(f)] == queries and len(f.starts) == 100
    assert 1 < f.peak <= 10
    assert f.result["result"]["stats"]["attempts"] == 100
    # Arrival jitter on a loaded runner may move individual arrivals by a few ms.
    assert f.starts[-1][1] - f.starts[0][1] >= 9.7
    for terms in ([], queries + ["too many"]):
        f.reset()
        run(cli, f, ("web_run", {"queries": terms}))
        assert f.result["is_error"] and not f.starts
    f.reset()
    run(cli, f, flow(["retry", "success"]))
    assert all("hits" in r for r in rows(f)) and f.calls == {"retry": 2, "success": 1}
    assert f.starts[1][1] - f.starts[0][1] >= 1
    f.reset()
    run(cli, f, flow(["exhaust", "success"]))
    assert (
        rows(f)[0]["http_status"] == 429
        and rows(f)[1]["hits"]
        and f.calls["exhaust"] == 3
    )
    f.reset()
    run(cli, f, flow(["cooldown", "b", "c", "d"]), timeout=2000)
    assert len(f.starts) == 1 and all(
        r["error"] == "rate_limited" and r["retry_after_ms"] > 1000 for r in rows(f)
    )
    f.reset()
    run(cli, f, flow(["last_credit", "not_sent"]), timeout=2000)
    assert len(f.starts) == 1 and rows(f)[0]["hits"] and rows(f)[1]["attempts"] == 0
    f.reset()
    run(cli, f, flow(["overload"]))
    assert (
        f.calls["overload"] == 2
        and f.starts[1][1] - f.starts[0][1] >= 1
        and rows(f)[0]["hits"]
    )
    f.reset()
    run(cli, f, flow(["fallback"]))
    assert rows(f)[0]["hits"][0]["snippet"] == "from description"
    f.reset(latency=0.25)
    script = "return (await Promise.all([tools.web_run({queries:['a','b','c']}),tools.web_run({queries:['d','e','f']})])).flatMap(x=>x.results)"
    run(cli, f, ("flow", {"script": script}))
    assert len(f.starts) == 6 and f.starts[-1][1] - f.starts[0][1] >= 0.48
    assert all("hits" in r for r in f.result["result"])
    f.reset()
    run(cli, f, flow(["credential", "never"]))
    assert f.calls == {"credential": 1} and all(
        r["error"] == "access_denied" for r in rows(f)
    )
    f.reset()
    run(
        cli,
        f,
        ("web_run", {"queries": ["first", "stall", "unsent"]}),
        interrupt=True,
    )


if __name__ == "__main__":
    with upstream() as fixture:
        validate(sys.argv[1], fixture)
