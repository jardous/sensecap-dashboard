#!/usr/bin/env python3
"""Read-only Reticulum status bridge. Python >=3.10, standard library only.

Invokes a fixed local `rnstatus --config DIR -j` argv, never a shell. This attaches
to an existing shared RNS instance; it does not create a transport or expose RPC.
No Reticulum code is bundled. Missing capabilities remain JSON null.
"""
import argparse
import hmac
import ipaddress
import json
import logging
import math
import os
from pathlib import Path
import ssl
import stat
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

LOG = logging.getLogger("sensecap-bridge")
MAX_OUTPUT = 256 * 1024

def number(value):
    if isinstance(value, int) and not isinstance(value, bool):
        return value if 0 <= value <= 2**64 - 1 else None
    if isinstance(value, float) and math.isfinite(value) and 0 <= value <= 2**64 - 1:
        return value
    return None

def project(raw, expose_hash=False):
    """An allowlist, never a pass-through of rnstatus/RNS internals."""
    if not isinstance(raw, dict) or not isinstance(raw.get("interfaces"), list):
        raise ValueError("Unsupported rnstatus schema")
    interfaces = []
    for entry in raw["interfaces"][:32]:
        if not isinstance(entry, dict):
            continue
        name = entry.get("name")
        name = "".join(c for c in name[:96] if c.isprintable()) if isinstance(name, str) else "Unknown"
        interfaces.append({
            "name": name,
            "online": entry.get("status") if isinstance(entry.get("status"), bool) else None,
            "rx_bytes": number(entry.get("rxb")),
            "tx_bytes": number(entry.get("txb")),
            "interface_clients": number(entry.get("clients")),
            # Do not mislabel interface clients as Reticulum or LXMF peers.
        })
    public_hash = raw.get("transport_id")
    if not (expose_hash and isinstance(public_hash, str) and len(public_hash) == 32
            and all(c in "0123456789abcdefABCDEF" for c in public_hash)):
        public_hash = None
    return {
        "schema": 1,
        "available": True,
        "source": "rnstatus -j / existing shared instance",
        "collected_at_unix": int(time.time()),
        "public_transport_hash": public_hash,
        "interfaces": interfaces,
        "rx_bytes": number(raw.get("rxb")),
        "tx_bytes": number(raw.get("txb")),
        "transport_uptime_seconds": number(raw.get("transport_uptime")),
        "known_destinations": None,
        "reticulum_peers": None,
        "lxmf_propagation": None,
        "lxmf_message_queue": None,
        "last_announce_unix": None,
    }

class Collector:
    def __init__(self, executable, config, expose_hash=False):
        self.argv = [str(executable), "--config", str(config), "-j"]
        self.expose_hash = expose_hash
        self.lock = threading.Lock()
        self.payload = {"schema": 1, "available": False, "error": "Not collected yet"}
        self.stop = threading.Event()

    def collect(self):
        try:
            # File-backed bounded read prevents a hostile/corrupt stdout allocation.
            with tempfile.TemporaryFile() as output:
                result = subprocess.run(self.argv, stdin=subprocess.DEVNULL, stdout=output,
                                        stderr=subprocess.DEVNULL, timeout=4, check=False,
                                        env={"PATH": "/usr/bin:/bin", "HOME": os.environ.get("HOME", "/nonexistent")})
                if result.returncode:
                    raise ValueError("Shared instance unavailable")
                output.seek(0)
                data = output.read(MAX_OUTPUT + 1)
                if len(data) > MAX_OUTPUT:
                    raise ValueError("Status too large")
                payload = project(json.loads(data), self.expose_hash)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            LOG.warning("Reticulum status unavailable; check local rnstatus/config compatibility")
            payload = {"schema": 1, "available": False, "collected_at_unix": int(time.time()),
                       "error": "Shared instance unavailable or unsupported status"}
        with self.lock:
            self.payload = payload

    def run(self):
        while not self.stop.is_set():
            self.collect()
            self.stop.wait(5)

    def snapshot(self):
        with self.lock:
            return json.dumps(self.payload, allow_nan=False, separators=(",", ":")).encode()

class Server(HTTPServer):
    request_queue_size = 8

    def get_request(self):
        sock, address = super().get_request()
        sock.settimeout(2)
        return sock, address

def handler_for(collector, token, allowed):
    class Handler(BaseHTTPRequestHandler):
        server_version = "SenseCAPBridge/1"
        sys_version = ""
        protocol_version = "HTTP/1.0"

        def log_message(self, *_):
            pass # Authorization, paths, hashes and remote addresses never logged

        def reply(self, status, payload):
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self):
            address = ipaddress.ip_address(self.client_address[0])
            supplied = self.headers.get("Authorization", "")
            if not any(address in network for network in allowed):
                self.reply(403, b'{"error":"forbidden"}')
                return
            if len(supplied) > 512 or not hmac.compare_digest(supplied.encode(), b"Bearer " + token):
                self.reply(401, b'{"error":"unauthorized"}')
                return
            if self.path != "/v1/status":
                self.reply(404, b'{"error":"not found"}')
                return
            self.reply(200, collector.snapshot())

        def do_POST(self):
            self.reply(405, b'{"error":"read only"}')

        do_PUT = do_POST
        do_DELETE = do_POST
        do_PATCH = do_POST

    return Handler

def read_token(path):
    with open(path, "rb") as f:
        info = os.fstat(f.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077 or info.st_uid != os.getuid():
            raise ValueError("Token must be an owner-only regular file (0600 or 0400)")
        token = f.read(256).strip()
    if not 32 <= len(token) <= 128 or any(c < 33 or c > 126 for c in token):
        raise ValueError("Token must contain 32..128 printable non-space ASCII characters")
    return token

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1", help="Literal IPv4 bind address")
    parser.add_argument("--port", type=int, default=8086)
    parser.add_argument("--allow", action="append", required=True, help="Explicit authorized client IP/CIDR; repeatable")
    parser.add_argument("--token-file", type=Path, required=True)
    parser.add_argument("--rnstatus", type=Path, required=True, help="Absolute path to trusted rnstatus executable")
    parser.add_argument("--rns-config", type=Path, required=True)
    parser.add_argument("--cert", type=Path)
    parser.add_argument("--key", type=Path)
    parser.add_argument("--expose-public-transport-hash", action="store_true", help="Off by default; never exports identity files")
    args = parser.parse_args()
    address = ipaddress.IPv4Address(args.bind)
    if not args.rnstatus.is_absolute() or not os.access(args.rnstatus, os.X_OK):
        parser.error("rnstatus must be an absolute executable path")
    if not args.rns_config.is_absolute() or not args.rns_config.is_dir():
        parser.error("RNS config must be an existing absolute directory")
    if bool(args.cert) != bool(args.key) or (not address.is_loopback and not args.cert):
        parser.error("Non-loopback listening requires --cert and --key; TLS is mandatory on LAN")
    try:
        token = read_token(args.token_file)
        allowed = [ipaddress.IPv4Network(network, strict=False) for network in args.allow]
    except ValueError as error:
        parser.error(str(error))
    if any(network.prefixlen == 0 for network in allowed):
        parser.error("Unrestricted client networks are not allowed")
    collector = Collector(args.rnstatus, args.rns_config, args.expose_public_transport_hash)
    server = Server((args.bind, args.port), handler_for(collector, token, allowed))
    if args.cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.cert, args.key)
        # Handshake happens after accept with the 2s socket timeout, not in accept().
        old_get_request = server.get_request
        def tls_request():
            sock, peer = old_get_request()
            try:
                return context.wrap_socket(sock, server_side=True), peer
            except Exception:
                sock.close()
                raise
        server.get_request = tls_request
    worker = threading.Thread(target=collector.run, daemon=True)
    worker.start()
    LOG.info("Read-only bridge started; authorized clients only")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        collector.stop.set()
        server.server_close()
        worker.join(timeout=5)

if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    main()
