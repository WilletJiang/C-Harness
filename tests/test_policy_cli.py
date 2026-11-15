"""Exercise policy configuration and ownership over offline CLI/RPC metadata."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def verify_help(binary, environment):
    def help_text(*arguments):
        result = subprocess.run([binary, *arguments], text=True, capture_output=True,
                                env=environment, timeout=10)
        assert result.returncode == 0 and not result.stderr, result
        return result.stdout

    normal = help_text("--help")
    assert normal == help_text() == help_text("help")
    complete = help_text("--help-all")
    normal_flags = set(re.findall(r"--[a-z][a-z-]*", normal))
    complete_flags = set(re.findall(r"--[a-z][a-z-]*", complete))
    everyday = {"--model", "--workspace", "--context-bytes", "--parallel-tools",
                "--auto-context", "--agents", "--skills", "--mcp", "--plugins"}
    advanced = {"--context-trigger", "--context-target", "--context-reserve",
                "--keep-exchanges", "--compact-tokens", "--model-tool-bytes",
                "--agent-limit", "--agent-depth", "--extension-context-bytes"}
    assert everyday <= normal_flags and everyday <= complete_flags
    assert not advanced & normal_flags and advanced <= complete_flags
    assert normal_flags - {"--help-all"} <= complete_flags
    assert "default off" in normal and "--help-all" in normal
    assert "thresholds follow the context budget" in normal


def main():
    binary = sys.argv[1]
    environment = dict(os.environ)
    environment.pop("AH_ACCESS_TOKEN", None)
    verify_help(binary, environment)
    with tempfile.TemporaryDirectory(prefix="ah-policy-cli-") as directory:
        workspace = Path(directory)
        (workspace / "a").mkdir()
        (workspace / "b").mkdir()
        database = workspace / "history.db"

        def command(name, *arguments, success=True):
            result = subprocess.run(
                [binary, name, "--db", str(database), *arguments],
                text=True, capture_output=True, env=environment, timeout=10)
            assert (result.returncode == 0) == success, result.stderr
            return json.loads(result.stdout) if success else result

        source = command("new", "--model", "offline")["id"]
        other = command("new", "--model", "offline")["id"]
        assert not command("context", "--session", source)["enabled"]
        assert not command("context", "--session", source,
                           "--context-bytes", "8192")["enabled"]
        for budget in (4096, 8192, 1048576, 67108864):
            derived = command("context", "--session", source, "--auto-context",
                              "--context-bytes", str(budget))
            assert derived["enabled"]
            assert derived["trigger_bytes"] == budget - budget // 4, derived
            assert derived["target_bytes"] == budget // 2, derived
            assert derived["reserve_bytes"] == min(budget // 8, 65536), derived
        configured = command(
            "context", "--session", source, "--auto-context", "--context-bytes", "8192",
            "--context-trigger", "6144", "--context-target", "4096", "--context-reserve", "1024",
            "--keep-exchanges", "2", "--compact-tokens", "5000")
        assert configured["enabled"] and configured["trigger_bytes"] == 6144, configured
        assert configured["target_bytes"] == 4096 and configured["reserve_bytes"] == 1024
        for flags in [
            ["--context-bytes", "4095"], ["--context-bytes", "67108865"],
            ["--context-target", "-1"], ["--context-trigger", "x"],
            ["--keep-exchanges", "65"], ["--compact-tokens", "0"],
            ["--context-reserve"], ["--coordinate-writes"],
        ]:
            assert command("context", "--session", source, *flags, success=False).returncode == 2
        command("context", "--session", source, "--context-target", "5000",
                "--context-trigger", "4000", success=False)
        command("context", "--session", source, "--context-bytes", "8192",
                "--context-trigger", "7000", "--context-reserve", "2048", success=False)
        assert command("context", "--session", source,
                       "--context-target", "4096")["enabled"]
        assert not command("context", "--session", source)["enabled"]
        # All legacy numeric flags remain available, including serial/uncapped sentinels.
        for parallel in ("0", "1", "16"):
            command("new", "--model", "offline", "--parallel-tools", parallel,
                    "--model-tool-bytes", "0", "--agents", "1", "--agent-limit", "1024",
                    "--agent-depth", "32", "--keep-turns", "10000")
        numeric_options = ("--parallel-tools", "--model-tool-bytes", "--agents",
                           "--agent-limit", "--agent-depth", "--keep-turns",
                           "--context-bytes", "--context-target", "--extension-context-bytes")
        for option in numeric_options:
            for invalid in ("", "-1", "+1024", " 1024", "1024x", str(2**128)):
                result = command("new", "--model", "offline", option, invalid, success=False)
                assert result.returncode == 2 and f"Invalid {option}" in result.stderr, result
            assert command("new", "--model", "offline", option, success=False).returncode == 2
        for option, invalid in (("--parallel-tools", "17"), ("--model-tool-bytes", "1023"),
                                ("--agents", "0"), ("--agents", "33"),
                                ("--agent-limit", "1025"), ("--agent-depth", "33"),
                                ("--keep-turns", "10001")):
            assert command("new", "--model", "offline", option, invalid,
                           success=False).returncode == 2
        state = command("workspace", "--coordinate-writes", "--workspace", str(workspace))
        assert state["enabled"] and state["owners"] == [], state
        assert command("workspace")["owners"] == []
        typed = '[{"type":"input_text","text":"typed startup"}]'
        assert command("new", "--model", "offline", "--content", typed,
                       "--command-id", "startup")["state"] == "ready"
        assert command("new", "--model", "offline", "--content", typed,
                       success=False).returncode == 2
        assert command("run", "--session", source, "--content", typed, "--prompt", "text",
                       "--command-id", "ambiguous", success=False).returncode == 2
        command("forget", "--session", source, "--prompt", "surprise", "--command-id", "x",
                success=False)
        assert command("inspect", "--session", source)["state"] == "idle"
        frames = [
            {"id": 1, "method": "workspace"},
            {"id": 2, "method": "workspace_assign", "session": source, "paths": ["a"]},
            {"id": 3, "method": "workspace_assign", "session": other, "paths": ["a/file"]},
            {"id": 4, "method": "workspace_assign", "session": other, "paths": ["b"]},
            {"id": 5, "method": "workspace_transfer", "from": source, "to": other, "path": "a"},
            {"id": 6, "method": "workspace_assign", "session": source, "paths": [], "broad": True},
            {"id": 7, "method": "workspace_assign", "session": source, "paths": ["a", 1]},
            {"id": 8, "method": "workspace_assign", "session": source, "paths": [], "broad": "true"},
            {"id": 9, "method": "workspace_assign", "session": source},
            {"id": 10, "method": "context", "session": source},
            {"id": 11, "method": "workspace_assign", "session": other, "paths": []},
            {"id": 12, "method": "workspace_assign", "session": source, "paths": [], "broad": True},
            {"id": 13, "method": "workspace_assign", "session": source, "paths": []},
            {"id": 14, "method": "workspace"},
        ]
        result = subprocess.run(
            [binary, "rpc", "--db", str(database), "--workspace", str(workspace),
             "--coordinate-writes", "--auto-context", "--context-bytes", "8192"],
            text=True, capture_output=True, env=environment, timeout=10,
            input="".join(json.dumps(frame) + "\n" for frame in frames))
        assert result.returncode == 0, result.stderr
        replies = {item["id"]: item for line in result.stdout.splitlines()
                   if "id" in (item := json.loads(line)) and "status" in item}
        expected = {1: "ok", 2: "ok", 3: "busy", 4: "ok", 5: "auth", 6: "busy",
                    7: "invalid", 8: "invalid", 9: "invalid", 10: "ok", 11: "ok",
                    12: "ok", 13: "ok", 14: "ok"}
        for identifier, status in expected.items():
            assert replies[identifier]["status"] == status, replies[identifier]
        assert replies[1]["result"]["owners"] == []
        assert replies[10]["result"]["enabled"]
        assert replies[12]["result"]["owners"] == [
            {"session_id": source, "broad": True, "paths": []}]
        assert replies[14]["result"]["owners"] == []
    print("policy CLI/RPC tests passed")


if __name__ == "__main__":
    main()
