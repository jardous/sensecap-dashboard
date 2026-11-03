#!/usr/bin/env python3
"""Capture a bounded ESP32 boot log using pyserial from the IDF environment.

--reset toggles only ESP32 reset (RTS), never BOOT/DTR or flash writes.
Keep captures private if using this tool with another firmware that logs secrets.
"""
import argparse
import sys
import time
import serial

parser = argparse.ArgumentParser()
parser.add_argument("port")
parser.add_argument("--seconds", type=int, default=45)
parser.add_argument("--reset", action="store_true")
args = parser.parse_args()
port = serial.Serial()
port.port = args.port
port.baudrate = 115200
port.timeout = 0.25
port.dtr = False
port.rts = False
port.open()
if args.reset:
    port.rts = True
    time.sleep(0.1)
    port.rts = False
deadline = time.monotonic() + args.seconds
try:
    while time.monotonic() < deadline:
        data = port.read(1024)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
finally:
    port.close()
