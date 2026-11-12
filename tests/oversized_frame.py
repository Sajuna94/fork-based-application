#!/usr/bin/env python3
"""Send a frame length above the protocol limit; the server should close it."""
import socket
import struct
import sys

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 9000

with socket.create_connection((host, port), timeout=5) as sock:
    sock.settimeout(5)
    sock.sendall(struct.pack("!I", 4097))
    data = sock.recv(1)
    if data:
        raise RuntimeError(f"unexpected response to oversized frame: {data!r}")
print("PASS: oversized frame was rejected and the connection was closed")
