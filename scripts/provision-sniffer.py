#!/usr/bin/env python3
"""Create private provisioning files, or explicitly send one over XIAO USB.

Never prints credentials; requires pyserial only for the send subcommand.
"""
import argparse
import getpass
import json
import os
from pathlib import Path
import secrets
import stat
import time


def validate(data):
    if not isinstance(data, dict) or set(data) != {"type", "ssid", "password", "token"} or data["type"] != "provision":
        raise ValueError("Expected exactly type=provision, ssid, password, token")
    if not all(isinstance(data[k], str) for k in data):
        raise ValueError("All fields must be strings")
    if not 1 <= len(data["ssid"].encode()) <= 32 or len(data["password"].encode()) > 64:
        raise ValueError("SSID/password exceed firmware byte limits")
    if not 32 <= len(data["token"]) <= 95 or any(not 33 <= ord(c) <= 126 for c in data["token"]):
        raise ValueError("Token must contain 32..95 visible ASCII characters")
    if any("\0" in value for value in data.values()):
        raise ValueError("NUL is not allowed")
    wire = json.dumps(data, separators=(",", ":"), ensure_ascii=True).encode()
    if len(wire) > 511:
        raise ValueError("Encoded provisioning request exceeds 511 bytes")
    return wire


def write_private(path, content):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as out:
        out.write(content)


def read_private(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077 or info.st_uid != os.getuid():
            raise ValueError("Use a regular, user-owned mode-0600 secret file")
        data = source.read(513)
        if len(data) > 512:
            raise ValueError("Provision file too large")
        return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create", help="Prompt privately; creates JSON and adjacent .token file without overwriting")
    create.add_argument("file", type=Path)
    send = sub.add_parser("send", help="WRITE sniffer Wi-Fi/token NVS; reboot after successful commit")
    send.add_argument("file", type=Path)
    send.add_argument("--port", required=True)
    args = parser.parse_args()
    if args.command == "create":
        token_path = args.file.with_suffix(".token")
        if token_path == args.file or token_path.exists() or args.file.exists():
            raise ValueError("Choose new JSON and .token paths; files are never overwritten")
        data = {"type": "provision", "ssid": input("Local Wi-Fi SSID: "),
                "password": getpass.getpass("Wi-Fi password: "), "token": secrets.token_hex(32)}
        wire = validate(data)
        args.file.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        write_private(args.file, wire + b"\n")
        write_private(token_path, data["token"].encode() + b"\n")
        print("Private JSON and token files created. Enter the same token in D1S LoRa settings.")
        return
    wire = validate(json.loads(read_private(args.file))) + b"\n"
    import serial
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, 115200, 0.25
    port.dtr = port.rts = False
    port.open()
    try:
        port.reset_input_buffer()
        port.write(wire)
        port.flush()
        deadline = time.monotonic() + 20
        seen = b""
        while time.monotonic() < deadline:
            seen = (seen + port.read(1024))[-4096:]
            if b"Saved; rebooting" in seen:
                print("Sniffer acknowledged NVS save and reboot.")
                return
            if b"Invalid provision request" in seen or b"Save failed" in seen:
                raise ValueError("Sniffer rejected provisioning or could not save")
        raise TimeoutError("No save acknowledgement; inspect USB/firmware privately before retrying")
    finally:
        port.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, TimeoutError) as exc:
        raise SystemExit(str(exc)) from None
