#!/usr/bin/env python3
"""Send an RPX through the actual WiiLoad plugin; inspect its log for launch success."""
import argparse
from pathlib import Path
import socket
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rpx", type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4299)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    size = args.rpx.stat().st_size
    if not 52 <= size <= 64 * 1024 * 1024:
        parser.error("Expected an RPX between 52 bytes and 64 MiB")
    data = args.rpx.read_bytes()
    if data[:9] != b"\x7fELF\x01\x02\x01\xca\xfe" or data[16:20] != b"\xfe\x01\x00\x14":
        parser.error("Expected a Wii U PowerPC RPX")
    # 0.4 is deliberately uncompressed. The tested plugin's compressed path
    # reuses the input temporary file as the output, then removes that path.
    header = b"HAXX" + bytes((0, 4, 0, 0)) + struct.pack(">I", len(data))
    with socket.create_connection((args.host, args.port), args.timeout) as connection:
        connection.sendall(header)
        connection.sendall(data)
    print(f"Sent {len(data)} bytes through WiiLoad. Check the target log for executable entry.")


if __name__ == "__main__":
    main()
