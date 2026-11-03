#!/usr/bin/env python3
"""Bounded read-only local WebSocket observer. No RF transmit or config commands.

Requires Python>=3.11 and websockets>=15. Raw payload output is opt-in; packet contents can be private.
"""
import argparse
import asyncio
import importlib.util
import json
from pathlib import Path
from urllib.parse import urlsplit


async def observe(args, token):
    from websockets.asyncio.client import connect
    async with connect(args.url, additional_headers={"Authorization": "Bearer " + token},
                       max_size=2048, max_queue=4, open_timeout=8, close_timeout=2, proxy=None) as ws:
        try:
            async with asyncio.timeout(args.seconds):
                await receive(ws, args)
        except TimeoutError:
            pass  # normal observation-window expiry, not a connection timeout


async def receive(ws, args):
    async for raw in ws:
        if not isinstance(raw, str):
            raise ValueError("Unexpected binary telemetry")
        event = json.loads(raw)
        if not isinstance(event, dict) or event.get("schema") != 1:
            raise ValueError("Unsupported telemetry schema")
        if event.get("type") == "error":
            raise ValueError("Receiver returned an error; check token/client limit")
        if args.raw:
            print(raw, flush=True)
        elif event.get("type") == "packet":
            print(f"RX #{event['sequence']} {event['length']} bytes / RSSI {event['rssi_dbm']} dBm / SNR {event['snr_db']} dB", flush=True)
        else:
            print(f"{event.get('type')}: ready={event.get('radio_ready', 'n/a')} RX={event.get('rx_packets', 'n/a')}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url", help="ws://lora-sniffer.local:8080/lora or manual private IP")
    parser.add_argument("--token-file", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--raw", action="store_true", help="Print complete JSON including raw payload HEX")
    args = parser.parse_args()
    url = urlsplit(args.url)
    if url.scheme != "ws" or not url.hostname or url.path != "/lora" or url.username or url.password or url.query or url.fragment:
        raise ValueError("Use ws://<trusted-LAN-host>:8080/lora without userinfo/query/fragment")
    if not 1 <= args.seconds <= 3600:
        raise ValueError("Seconds must be 1..3600")
    spec = importlib.util.spec_from_file_location("provision", Path(__file__).with_name("provision-sniffer.py"))
    provision = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(provision)
    token = provision.read_private(args.token_file).decode("ascii").removesuffix("\n")
    if not 32 <= len(token) <= 95 or any(not 33 <= ord(c) <= 126 for c in token):
        raise ValueError("Invalid token file")
    asyncio.run(observe(args, token))


if __name__ == "__main__":
    main()
