import os
import unittest
from unittest.mock import Mock, patch

from packages.host import tui_auth
from packages.host.oauth import AuthError


class TuiAuthTests(unittest.TestCase):
    def test_login_only_when_needed_and_interactive(self):
        manager = Mock()
        manager.access_token.side_effect = [
            AuthError("login_required", "Sign in"),
            "fixture-access",
        ]
        with patch.object(tui_auth, "login") as login:
            with self.assertRaises(AuthError):
                tui_auth.obtain_token(manager, False)
            login.assert_not_called()
            manager.access_token.side_effect = [
                AuthError("login_required", "Sign in"),
                "fixture-access",
            ]
            self.assertEqual(tui_auth.obtain_token(manager, True), "fixture-access")
            login.assert_called_once()

    def test_private_pipe_is_the_only_credential_output(self):
        read_fd, write_fd = os.pipe()
        with os.fdopen(read_fd, "rb") as read, patch.object(tui_auth, "KeychainVault"), \
                patch.object(tui_auth, "SessionManager") as manager, \
                patch.object(tui_auth.sys, "argv", ["tui_auth", "reuse", str(write_fd)]):
            manager.return_value.access_token.return_value = "fixture-access"
            self.assertEqual(tui_auth.main(), 0)
            self.assertEqual(read.read(), b"ok fixture-access\n")

    def test_reactive_refresh_uses_fingerprint_and_never_opens_browser(self):
        read_fd, write_fd = os.pipe()
        fingerprint = "a" * 64
        with os.fdopen(read_fd, "rb") as read, patch.object(tui_auth, "KeychainVault"), \
                patch.object(tui_auth, "SessionManager") as manager, \
                patch.object(tui_auth, "login") as login, \
                patch.object(tui_auth.sys, "argv", ["tui_auth", "refresh", str(write_fd), fingerprint]):
            manager.return_value.access_token.return_value = "fixture-new-access"
            self.assertEqual(tui_auth.main(), 0)
            self.assertEqual(read.read(), b"ok fixture-new-access\n")
            manager.return_value.access_token.assert_called_once_with(rejected_sha256=fingerprint)
            login.assert_not_called()


if __name__ == "__main__":
    unittest.main()
