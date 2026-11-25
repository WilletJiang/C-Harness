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
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode == "refresh" and len(sys.argv) == 4:
        fingerprint = sys.argv[3]
        if len(fingerprint) != 64 or any(c not in "0123456789abcdef" for c in fingerprint):
            return 2
    elif mode in {"login", "reuse"} and len(sys.argv) == 3:
        fingerprint = None
    else:
        return 2
    fd = int(sys.argv[2])
    if fd < 3 or not stat.S_ISFIFO(os.fstat(fd).st_mode):
        return 2
    os.set_inheritable(fd, False)
    with os.fdopen(fd, "wb") as channel:
        try:
            manager = SessionManager(KeychainVault(), DEFAULT_PROFILE)
            token = (manager.access_token(rejected_sha256=fingerprint) if fingerprint
                     else obtain_token(manager, mode == "login")).encode("ascii")
            if not token or len(token) > 16384 or any(b < 33 or b > 126 for b in token):
                raise AuthError("protocol", "Invalid access token")
        except (AuthError, VaultError, UnicodeError) as error:
            code = getattr(error, "code", "keychain")
            if code in UNUSABLE:
                code = "login_required"
            elif getattr(error, "retryable", False) and code != "refresh_deferred":
                code = "network"
            allowed = {"login_required", "access_denied", "timeout", "network", "keychain",
                       "refresh_deferred"}
            channel.write(("error " + (code if code in allowed else "protocol") + "\n").encode())
            return 1
        channel.write(b"ok " + token + b"\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
