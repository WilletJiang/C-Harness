"""Offline CLI/RPC content and deletion boundaries; never calls a provider."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile


def main():
    binary = sys.argv[1]
    environment = dict(os.environ)
    environment.pop("AH_ACCESS_TOKEN", None)
    with tempfile.TemporaryDirectory(prefix="ah-content-cli-") as directory:
        root = Path(directory)
        database = root / "history.db"
        document = root / "notes.txt"
        document.write_text("immutable local notes", encoding="utf-8")

        def command(name, *arguments, success=True):
            result = subprocess.run(
                [binary, name, "--db", str(database), *arguments],
                text=True, capture_output=True, env=environment, timeout=10)
            assert (result.returncode == 0) == success, result.stderr
            return json.loads(result.stdout) if success else result

        source = command("new", "--model", "offline")["id"]
        artifact = command("artifact-import", "--session", source,
                           "--artifact-path", str(document), "--mime", "text/plain")["artifact_id"]
        assert command("artifacts", "--session", source)[0]["artifact_id"] == artifact
        fork = command("fork", "--session", source)["id"]
        content = [{"type": "input_file", "artifact_id": artifact, "filename": "notes.txt"}]
        fields = ["--session", fork, "--command-id", "typed", "--content", json.dumps(content)]
        assert command("submit-content", *fields)["state"] == "ready"
        assert command("submit-content", *fields)["state"] == "ready"
        command("submit-content", "--session", fork, "--command-id", "typed",
                "--content", '[{"type":"input_text","text":"different"}]', success=False)
        command("forget", "--session", fork, success=False)
        assert command("forget", "--session", source) == {"forgotten": True}
        with sqlite3.connect(database) as connection:
            assert connection.execute("SELECT count(*) FROM artifacts").fetchone()[0] == 1
            assert connection.execute("SELECT count(*) FROM commands").fetchone()[0] == 1

        unrelated = command("new", "--model", "offline")["id"]
        frames = [
            {"id": 1, "method": "submit_content", "session": unrelated,
             "command": "private", "content": content},
            {"id": 2, "method": "artifact_import", "session": unrelated,
             "path": str(document), "mime": "text/plain"},
            {"id": 3, "method": "artifacts", "session": unrelated},
            {"id": 4, "method": "forget", "session": unrelated, "tree": "yes"},
            {"id": 5, "method": "forget", "session": unrelated},
            {"id": 6, "method": "storage_vacuum"},
        ]
        result = subprocess.run(
            [binary, "rpc", "--db", str(database)], text=True, capture_output=True,
            input="".join(json.dumps(frame) + "\n" for frame in frames),
            env=environment, timeout=10)
        assert result.returncode == 0, result.stderr
        replies = {item["id"]: item for line in result.stdout.splitlines()
                   if "id" in (item := json.loads(line)) and "status" in item}
        assert replies[1]["status"] == "not_found", replies
        assert replies[2]["result"]["artifact_id"] == artifact, replies
        assert replies[3]["result"][0]["artifact_id"] == artifact, replies
        assert replies[4]["status"] == "invalid", replies
        assert replies[5]["result"] == {"forgotten": True}, replies
        assert replies[6]["result"] == {"vacuumed": True}, replies
        assert command("vacuum") == {"vacuumed": True}
        with sqlite3.connect(database) as connection:
            assert connection.execute("SELECT count(*) FROM artifacts").fetchone()[0] == 1
            assert not connection.execute("PRAGMA foreign_key_check").fetchall()
    print("content CLI/RPC tests passed")


if __name__ == "__main__":
    main()
