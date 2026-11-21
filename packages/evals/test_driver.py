"""Root and child request decoration against a numeric loopback fixture."""

import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import http.server
from tests.loopback import LoopbackServer

received = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        received.append((body, self.headers.get("session-id")))
        child = any(item.get("role") == "user" and "CHILD_MARKER" in str(item.get("content"))
                    for item in body["input"])
        continued = any(item.get("type") == "function_call_output" for item in body["input"])
        if not continued:
            name = "read_file" if child else "spawn"
            arguments = {"path": "file.txt"} if child else {
                "name": "reader", "message": "CHILD_MARKER: Read file.txt then finish."}
            output = [{"type": "function_call", "id": "item-" + name,
                       "name": name, "call_id": "call-" + name, "arguments": json.dumps(arguments)}]
        else:
            output = [{"type": "message", "id": "final-" + str(len(received)), "role": "assistant",
                       "content": [{"type": "output_text", "text": "done"}]}]
        response = {"id": "fixture-" + str(len(received)), "status": "completed",
                    "model": body["model"], "reasoning": body["reasoning"], "output": [],
                    "usage": {"input_tokens": 10, "output_tokens": 2}}
        rows = [{"type": "response.output_item.done", "output_index": i, "item": item}
                for i, item in enumerate(output)]
        rows.append({"type": "response.completed", "response": response})
        wire = "".join("data: " + json.dumps(row) + "\n\n" for row in rows).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(wire)))
        self.end_headers()
        self.wfile.write(wire)


def main(driver):
    server = LoopbackServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with tempfile.TemporaryDirectory(prefix="ah-paired-driver-") as temporary:
            root = Path(temporary)
            workspace = root / "workspace"
            workspace.mkdir()
            (workspace / "file.txt").write_text("fixture")
            image = workspace / "fixture.png"
            image.write_bytes(base64.b64decode(
                "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/"
                "x8AAwMCAO+aXioAAAAASUVORK5CYII="))
            env = {k: v for k, v in os.environ.items() if not k.startswith("AH_EVAL_")}
            env.update(AH_ACCESS_TOKEN="loopback-fixture",
                       AH_EVAL_ENDPOINT=f"http://127.0.0.1:{server.server_port}/responses",
                       AH_EVAL_ALLOW_LOOPBACK="1", AH_EVAL_AGENTS="2", AH_EVAL_AUTO_CONTEXT="1",
                       AH_EVAL_PARALLEL_TOOLS="3", AH_EVAL_CACHE_KEY="omit", AH_EVAL_TRACE_REQUESTS="1",
                       AH_EVAL_IMAGES=json.dumps([str(image)]))
            result = subprocess.run([driver, str(workspace), str(root / "state.db"),
                                     "gpt-6.2-sol", "unused", "xhigh", "10"],
                                    input="ROOT_MARKER: Delegate the read to a child and finish.",
                                    env=env, capture_output=True, text=True, timeout=15)
            assert result.returncode == 0, result.stderr
            assert 4 <= len(received) <= 5, len(received)  # A child completion may wake the parent.
            prefix = received[0][0]["input"][0]
            assert prefix["role"] == "developer"
            part, = prefix["content"]
            assert part["type"] == "input_text" and part["text"]
            assert part["prompt_cache_breakpoint"] == {"mode": "explicit"}
            histories = {}
            for body, route in received:
                assert body["model"] == "gpt-6.2-sol"
                assert "instructions" not in body and body["input"][0] == prefix
                child = any(item.get("role") == "user" and "CHILD_MARKER" in str(item.get("content"))
                            for item in body["input"])
                earlier = histories.get(child, [])
                assert body["input"][:len(earlier)] == earlier
                histories[child] = body["input"]
                images = [part for item in body["input"] for part in item.get("content", [])
                          if isinstance(part, dict) and part.get("type") == "input_image"]
                assert len(images) == (0 if child else 1)
                if images:
                    assert images[0]["image_url"].startswith("data:image/png;base64,")
                assert body["reasoning"] == {"effort": "xhigh"}
                assert "prompt_cache_key" not in body and route is None
                assert "recall" in json.dumps(body["tools"])
                assert "spawn" in json.dumps(body["tools"])
            events = [json.loads(line) for line in result.stdout.splitlines()]
            configured = next(row["data"] for row in events if row.get("event") == "evaluation.config")
            assert configured["AH_EVAL_AGENTS"] == "2"
            assert configured["AH_EVAL_AUTO_CONTEXT"] == "1"
            assert configured["AH_EVAL_PARALLEL_TOOLS"] == "3"
            assert sum(row.get("event") == "provider.request" for row in events) == len(received)
            assert sum(row.get("event") == "provider.response" for row in events) == len(received)
            assert len({row["session_id"] for row in events if row.get("event") == "model.started"}) == 2
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


if __name__ == "__main__":
    main(sys.argv[1])
