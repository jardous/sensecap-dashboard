import importlib.util
import ipaddress
import json
from pathlib import Path
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

spec = importlib.util.spec_from_file_location("bridge", Path(__file__).parents[1] / "bridge.py")
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)

class ProjectionTests(unittest.TestCase):
    def test_allowlist_no_secrets_or_invented_peers(self):
        data = {"interfaces": [{"name": "TCPInterface[server]", "status": True,
                 "rxb": 100, "txb": 200, "clients": 3, "private_key": "secret"}],
                "transport_id": "ab" * 16, "identity": "secret", "rxb": 400, "txb": 500}
        status = bridge.project(data)
        self.assertIsNone(status["public_transport_hash"])
        self.assertNotIn("secret", json.dumps(status))
        self.assertEqual(status["interfaces"][0]["interface_clients"], 3)
        self.assertIsNone(status["reticulum_peers"])
        self.assertIsNone(status["known_destinations"])
        self.assertEqual(status["rx_bytes"], 400) # no double counting interface clients
        self.assertEqual(bridge.project(data, True)["public_transport_hash"], "ab" * 16)

    def test_missing_and_invalid_values(self):
        status = bridge.project({"interfaces": [{"status": "online", "rxb": -1, "txb": float("nan")} ]})
        self.assertIsNone(status["interfaces"][0]["online"])
        self.assertIsNone(status["interfaces"][0]["rx_bytes"])
        self.assertIsNone(status["interfaces"][0]["tx_bytes"])
        json.dumps(status, allow_nan=False)
        with self.assertRaises(ValueError):
            bridge.project({"interfaces": "invalid"})
        self.assertIsNone(bridge.number(10**1000))
        self.assertIsNone(bridge.number(True))

    def test_token_permissions(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "token"
            path.write_text("x" * 48)
            path.chmod(0o600)
            self.assertEqual(bridge.read_token(path), b"x" * 48)
            path.chmod(0o644)
            with self.assertRaises(ValueError):
                bridge.read_token(path)

    def test_collector_fixed_argv_failure(self):
        collector = bridge.Collector("/nonexistent/rnstatus", "/not/a/config")
        self.assertEqual(collector.argv, ["/nonexistent/rnstatus", "--config", "/not/a/config", "-j"])
        collector.collect()
        self.assertFalse(json.loads(collector.snapshot())["available"])

class HttpTests(unittest.TestCase):
    def setUp(self):
        collector = bridge.Collector("/nonexistent", "/config")
        self.server = bridge.Server(("127.0.0.1", 0), bridge.handler_for(
            collector, b"x" * 48, [ipaddress.ip_network("127.0.0.1/32")]))
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def request(self, path, token="x" * 48, method="GET"):
        request = urllib.request.Request(self.url + path, headers={"Authorization": "Bearer " + token}, method=method)
        return urllib.request.urlopen(request, timeout=2)

    def test_authenticated_read_only_endpoint(self):
        with self.request("/v1/status") as response:
            self.assertEqual(response.status, 200)
            self.assertEqual(response.headers["Cache-Control"], "no-store")
            self.assertFalse(json.load(response)["available"])
        for path, token, method, expected in [("/v1/status", "bad", "GET", 401),
                ("/config", "x" * 48, "GET", 404), ("/v1/status", "x" * 48, "POST", 405)]:
            with self.assertRaises(urllib.error.HTTPError) as caught:
                self.request(path, token, method)
            self.assertEqual(caught.exception.code, expected)
            caught.exception.close()

    def test_client_allowlist(self):
        collector = bridge.Collector("/nonexistent", "/config")
        self.server.RequestHandlerClass = bridge.handler_for(collector, b"x" * 48, [ipaddress.ip_network("192.0.2.1/32")])
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.request("/v1/status")
        self.assertEqual(caught.exception.code, 403)
        caught.exception.close()

if __name__ == "__main__":
    unittest.main()
