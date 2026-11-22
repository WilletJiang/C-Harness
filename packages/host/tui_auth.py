"""TUI credential handoff over an inherited private pipe."""

import os
import stat
import sys

from .login import login
from .oauth import AuthError
from .session import DEFAULT_PROFILE, SessionManager, UNUSABLE
from .vault import KeychainVault, VaultError


def obtain_token(manager, interactive):
    try:
        return manager.access_token()
    except AuthError as error:
        if not interactive or (error.code != "login_required" and error.code not in UNUSABLE):
            raise
    login(manager, lambda _url: None)
    return manager.access_token()


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in {"login", "reuse"}:
        return 2
    fd = int(sys.argv[2])
    if fd < 3 or not stat.S_ISFIFO(os.fstat(fd).st_mode):
        return 2
    os.set_inheritable(fd, False)
    with os.fdopen(fd, "wb") as channel:
        try:
            manager = SessionManager(KeychainVault(), DEFAULT_PROFILE)
            token = obtain_token(manager, sys.argv[1] == "login").encode("ascii")
            if not token or len(token) > 16384 or any(b < 33 or b > 126 for b in token):
                raise AuthError("protocol", "Invalid access token")
        except (AuthError, VaultError, UnicodeError) as error:
            code = getattr(error, "code", "keychain")
            if code in UNUSABLE:
                code = "login_required"
            allowed = {"login_required", "access_denied", "timeout", "network", "keychain"}
            channel.write(("error " + (code if code in allowed else "protocol") + "\n").encode())
            return 1
        channel.write(b"ok " + token + b"\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
