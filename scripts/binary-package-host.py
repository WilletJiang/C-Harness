"""Check that a packaged TUI locates its adjacent host adapter and Python on PATH."""
import fcntl
import json
import os
from pathlib import Path
import pty
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
import termios

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from test_tui_graphics import receive


def main():
    binary = Path(sys.argv[1]).absolute()
    with TemporaryDirectory(prefix="ah-bundle-host-") as tmp:
        root = Path(tmp)
        config = root / "config" / "ah"
        config.mkdir(parents=True, mode=0o700)
        profile = config / "tui.json"
        profile.write_text(json.dumps({"endpoint": "https://api.openai.com/v1/responses",
                                       "provider": "chatgpt", "model": "fixture"}))
        profile.chmod(0o600)
        mock = root / "python3"
        mock.write_text("""#!/bin/sh
set -eu
test "$6" = reuse
test -f "$5/packages/host/tui_auth.py"
printf '%s\\n' "$5" > "$AH_TEST_HOST_MARKER"
printf 'ok package-fixture-token\\n' >&3
""")
        mock.chmod(0o700)
        marker = root / "host"
        env = dict(os.environ, TERM="xterm-256color", AH_TUI_MATH="text",
                   XDG_CONFIG_HOME=str(config.parent), AH_TEST_HOST_MARKER=str(marker),
                   PATH=f"{root}{os.pathsep}{os.environ['PATH']}")
        for key in ("AH_ACCESS_TOKEN", "OPENAI_API_KEY"):
            env.pop(key, None)
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 78, 0, 0))
        proc = subprocess.Popen(
            [binary, "tui", "--db", root / "session.db", "--no-web-search", "--workspace", root],
            cwd=root, env=env, stdin=slave, stdout=slave, stderr=slave, start_new_session=True,
        )
        try:
            receive(master, b"Ask ah anything")
            expect = binary.resolve().parents[1] / "share" / "ah" / "host"
            assert Path(marker.read_text().strip()).resolve() == expect
            print("Packaged TUI found its adjacent host adapter using Python on PATH.")
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            os.close(master)
            os.close(slave)


if __name__ == "__main__":
    main()
