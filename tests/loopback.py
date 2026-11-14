"""Numeric loopback fixtures must not depend on the runner's reverse DNS."""
import http.server
import socketserver


class LoopbackServer(http.server.ThreadingHTTPServer):
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]
