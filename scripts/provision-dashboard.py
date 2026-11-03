#!/usr/bin/env python3
"""Load only the SenseCAP sniffer link over its CH340 USB console.

Token comes from a private file, never argv/output. The firmware keeps existing
Wi-Fi/host/radio settings and acknowledges commit before restarting D1S.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import time


def link_request(token, host="lora-sniffer.local", port=8080):
    if not isinstance(token, str) or not 32 <= len(token) <= 95 or any(not 33 <= ord(c) <= 126 for c in token):
        raise ValueError("Token must contain 32..95 visible ASCII characters")
    if not isinstance(host, str) or not 1 <= len(host) <= 95 or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-" for c in host):
        raise ValueError("Host must be a hostname or IPv4 address without a URL prefix/path")
    if type(port) is not int or not 1 <= port <= 65535:
        raise ValueError("Port must be 1..65535")
    data = {"schema": 1, "type": "provision_link", "host": host,
            "port": port, "token": token, "enabled": True}
    wire = json.dumps(data, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    if len(wire) > 511:
        raise ValueError("Encoded setup request exceeds 511 bytes")
    return wire + b"\n"


def send_request(port, wire, timeout=20):
    # Fixed read-only handshake: never send the secret to a device which hasn't
    # identified itself as this dashboard USB setup endpoint.
    probe = b'{"schema":1,"type":"sniffer_setup_probe"}\n'
    port.reset_input_buffer()
    deadline = time.monotonic() + 8
    next_probe = 0
    seen = b""
    while time.monotonic() < deadline:
        if time.monotonic() >= next_probe:
            port.write(probe)
            port.flush()
            next_probe = time.monotonic() + 1
        seen = (seen + port.read(1024))[-4096:]
        if b"DASH_USB_LINK_READY" in seen:
            break
    else:
        raise TimeoutError("Dashboard USB setup handshake missing; no token was sent")
    port.reset_input_buffer()
    port.write(wire)
    port.flush()
    seen = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        seen = (seen + port.read(1024))[-4096:]
        if b"DASH_USB_LINK_SAVED" in seen:
            return
        for marker in (b"DASH_USB_LINK_INVALID", b"DASH_USB_LINK_BUSY", b"DASH_USB_LINK_FAILED"):
            if marker in seen:
                raise ValueError("Dashboard rejected the request, is busy, or could not commit settings")
    raise TimeoutError("No commit acknowledgement; inspect dashboard privately before retrying")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="SenseCAP CH340 USB UART, never RP2040/XIAO")
    parser.add_argument("--token-file", required=True, type=Path)
    parser.add_argument("--host", default="lora-sniffer.local")
    parser.add_argument("--endpoint-port", type=int, default=8080)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("private_provision", Path(__file__).with_name("provision-sniffer.py"))
    private = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(private)
    token = private.read_private(args.token_file).decode("ascii").removesuffix("\n")
    wire = link_request(token, args.host, args.endpoint_port)
    import serial
    port = serial.Serial()
    port.port, port.baudrate, port.timeout, port.write_timeout = args.port, 115200, .2, 2
    port.dtr = port.rts = False
    port.open()
    try:
        send_request(port, wire)
        print("SenseCAP acknowledged link settings committed; restarting. Token was not displayed.")
    finally:
        port.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, TimeoutError) as exc:
        raise SystemExit(str(exc)) from None
