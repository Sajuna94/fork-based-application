#!/usr/bin/env python3
"""Disconnect mid-frame, then verify the server still serves a fresh client."""
import socket
import struct
import sys

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 9000

with socket.create_connection((host, port), timeout=5) as sock:
    sock.sendall(struct.pack("!I", 100) + b"INFO")
    sock.shutdown(socket.SHUT_WR)

with socket.create_connection((host, port), timeout=5) as sock:
    sock.settimeout(5)
    payload = b"INFO"
    sock.sendall(struct.pack("!I", len(payload)) + payload)
    header = sock.recv(4)
    if len(header) != 4:
        raise RuntimeError("server did not answer the follow-up INFO request")
    length = struct.unpack("!I", header)[0]
    response = bytearray()
    while len(response) < length:
        part = sock.recv(length - len(response))
        if not part:
            raise RuntimeError("truncated follow-up response")
        response.extend(part)
    print("PASS: incomplete frame was isolated to its child; server accepted the next client")
    print(response.decode(errors="replace"), end="")
