"""Run the extracted CLI against a certificate-verified loopback WSS peer."""
from collections import defaultdict
from contextlib import contextmanager
from pathlib import Path
import ssl
import subprocess
import sys
from tempfile import TemporaryDirectory
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from test_websocket import Client, LoopbackServer, Peer, states


@contextmanager
def peer(root):
    pem, key = root / "tls.pem", root / "tls.key"
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
         "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
         "-addext", "basicConstraints=critical,CA:TRUE",
         "-keyout", str(key), "-out", str(pem)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True,
    )
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(pem, key)
    with LoopbackServer(("127.0.0.1", 0), Peer) as server:
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
        server.ws, server.http, server.connections = (defaultdict(list) for _ in range(3))
        server.client_pings, server.steers, server.errors = defaultdict(list), defaultdict(list), []
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield server, pem
        finally:
            server.shutdown()
            thread.join()


def main():
    with TemporaryDirectory(prefix="ah-bundle-wss-") as tmp:
        root = Path(tmp)
        (root / "source.txt").write_text("release WSS fixture")
        with peer(root) as (server, ca):
            client = Client(root / "session.db", f"https://127.0.0.1:{server.server_port}/chain",
                            extra=("--transport", "websocket", "--workspace", str(root), "--no-web-search"),
                            env_overrides={"SSL_CERT_FILE": str(ca), "SSL_CERT_DIR": str(root)})
            try:
                sid = client.request("create", "create", model="fixture")["result"]["id"]
                assert client.request("start", "submit", session=sid, command="smoke", text="read")["status"] == "ok"
                result = client.request("run", "run", session=sid)
                assert result["result"]["state"] == "completed", (result, states(client), server.errors)
                first, second = server.ws["/chain"]
                assert first[0] == second[0] == 0 and not server.http["/chain"] and not server.errors
                assert second[1]["previous_response_id"] == "response-0"
                assert "release WSS fixture" in second[1]["input"][0]["output"]
                assert all(item["transport"] == "websocket" and item["phase"] != "fallback"
                           for item in states(client))
                print("Verified native WSS over TLS, same-connection tool continuation, and zero HTTP fallback.")
            finally:
                client.close()


if __name__ == "__main__":
    main()
