#!/usr/bin/env python3
"""Convert the simulator's P6 output to PNG, using only the standard library."""
import pathlib
import struct
import sys
import zlib

def convert(source, destination):
    with open(source, "rb") as f:
        assert f.readline() == b"P6\n"
        width, height = map(int, f.readline().split())
        assert f.readline() == b"255\n"
        pixels = f.read()
    assert len(pixels) == width * height * 3
    def chunk(kind, data):
        return struct.pack("!I", len(data)) + kind + data + struct.pack("!I", zlib.crc32(kind + data))
    rows = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack("!IIBBBBB", width, height, 8, 2, 0, 0, 0))
    pathlib.Path(destination).write_bytes(png + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))

if __name__ == "__main__":
    convert(*sys.argv[1:])
