#!/usr/bin/env python3
"""Run the same malformed/abnormal inputs against baseline and robust servers."""
import socket
import struct
import sys
import time
from typing import Optional

HOST = "127.0.0.1"
ROBUST_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19092
BASELINE_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 19093
IO_TIMEOUT = 2


def recv_exact(sock: socket.socket, count: int) -> Optional[bytes]:
    data = bytearray()
    while len(data) < count:
        try:
            part = sock.recv(count - len(data))
        except (TimeoutError, socket.timeout, OSError):
            return None
        if not part:
            return None
        data.extend(part)
    return bytes(data)


def read_response(sock: socket.socket) -> Optional[bytes]:
    header = recv_exact(sock, 4)
    if header is None:
        return None
    length = struct.unpack("!I", header)[0]
    return recv_exact(sock, length)


def fragmented_request(port: int) -> bool:
    payload = b"INFO"
    frame = struct.pack("!I", len(payload)) + payload
    try:
        with socket.create_connection((HOST, port), timeout=3) as sock:
            sock.settimeout(2)
            for byte in frame:
                sock.sendall(bytes([byte]))
                time.sleep(0.08)
            response = read_response(sock)
            return response is not None and response.startswith(b"hostname:")
    except OSError:
        return False


def zero_length_frame(port: int) -> bool:
    try:
        with socket.create_connection((HOST, port), timeout=3) as sock:
            sock.settimeout(2)
            sock.sendall(struct.pack("!I", 0))
            response = read_response(sock)
            return response is not None and b"unknown command" in response
    except OSError:
        return False


def idle_client(port: int) -> bool:
    """True means a new client can be served after one client idles too long."""
    try:
        with socket.create_connection((HOST, port), timeout=3):
            # Keep the first connection open without sending a frame. With a
            # one-worker limit, it occupies the only slot in baseline mode.
            time.sleep(IO_TIMEOUT + 1.0)
            try:
                with socket.create_connection((HOST, port), timeout=3) as next_client:
                    next_client.settimeout(2)
                    next_client.sendall(struct.pack("!I", 4) + b"INFO")
                    response = read_response(next_client)
                    return response is not None and response.startswith(b"hostname:")
            except OSError:
                return False
    except OSError:
        return False


def main() -> int:
    cases = [
        (
            "A. TCP fragmented header/payload",
            fragmented_request(BASELINE_PORT),
            fragmented_request(ROBUST_PORT),
            False,
            True,
            "request fails: baseline closes after a short header read",
            "INFO response returned: robust recv_all reassembles the stream",
        ),
        (
            "B. Zero-length frame (invalid by protocol)",
            zero_length_frame(BASELINE_PORT),
            zero_length_frame(ROBUST_PORT),
            True,
            False,
            "malformed frame is processed: baseline replies UNKNOWN COMMAND",
            "malformed frame is rejected: robust closes the connection",
        ),
        (
            f"C. Idle client ({IO_TIMEOUT + 1}s > configured {IO_TIMEOUT}s timeout; max_clients=1)",
            idle_client(BASELINE_PORT),
            idle_client(ROBUST_PORT),
            False,
            True,
            "next client is rejected BUSY: idle baseline worker occupies the only slot",
            "next client receives INFO: robust timeout frees the worker slot",
        ),
    ]

    failed = False
    for name, baseline, robust, expected_baseline, expected_robust, baseline_text, robust_text in cases:
        baseline_ok = baseline == expected_baseline
        robust_ok = robust == expected_robust
        print(f"\n{name}")
        print(f"  baseline mode: {baseline_text} -> {'PASS' if baseline_ok else 'FAIL'}")
        print(f"  robust mode:   {robust_text} -> {'PASS' if robust_ok else 'FAIL'}")
        failed = failed or not (baseline_ok and robust_ok)

    if failed:
        print("\nFAIL: comparison did not match the expected contrast.")
        return 1
    print("\nPASS: all three baseline-vs-robust comparisons matched their expected behavior.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
