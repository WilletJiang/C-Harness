"""HTTPS connection setup for OAuth calls carrying single-use credentials."""

import http.client
import ssl
import time
import urllib.request


class OAuthConnection(http.client.HTTPSConnection):
    def connect(self):
        # Retry during connection setup, before http.client sends any OAuth body.
        for attempt in range(4):
            try:
                return super().connect()
            except (ssl.SSLEOFError, ConnectionResetError, TimeoutError):
                if self.sock is not None:
                    self.sock.close()
                    self.sock = None
                if attempt == 3:
                    raise
                time.sleep(0.25 * (2**attempt))


class OAuthHTTPSHandler(urllib.request.HTTPSHandler):
    def https_open(self, request):
        return self.do_open(OAuthConnection, request, context=self._context)
