#!/usr/bin/env python3
"""Send a valid INFO frame in tiny pieces to test TCP stream reassembly."""
import socket
import struct
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 9000
payload = b"INFO"
frame = struct.pack("!I", len(payload)) + payload

with socket.create_connection((host, port), timeout=5) as sock:
    sock.settimeout(5)
    for byte in frame:
        sock.sendall(bytes([byte]))
        time.sleep(0.03)
    header = sock.recv(4)
    if len(header) != 4:
        raise RuntimeError(f"short response header: {header!r}")
    length = struct.unpack("!I", header)[0]
    chunks = bytearray()
    while len(chunks) < length:
        part = sock.recv(length - len(chunks))
        if not part:
            raise RuntimeError("server closed before full response")
        chunks.extend(part)
    print("PASS: fragmented request was reassembled by the server")
    print(chunks.decode(errors="replace"), end="")
