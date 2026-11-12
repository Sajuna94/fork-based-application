# Validation record

Validated in a Linux x86_64 environment with GCC 14.2.0 and GNU Make 4.4.1.

## Build

```text
make clean && make DEBUG=1
```

Result: shared library, server and client built successfully without compiler warnings.

## Runtime concurrency proof

```text
make test
Observed peak concurrent child processes: 10
PASS: runtime proof confirms at least 10 clients were served concurrently.
```

The test starts ten clients at the same time. Each client issues `HOLD 4`, so each worker remains alive while the parent accepts and forks the other clients.

## Abnormal input tests

```text
python3 tests/fragmented_frame.py 127.0.0.1 19091
PASS: fragmented request was reassembled by the server

python3 tests/oversized_frame.py 127.0.0.1 19091
PASS: oversized frame was rejected and the connection was closed

python3 tests/partial_frame.py 127.0.0.1 19091
PASS: incomplete frame was isolated to its child; server accepted the next client
```

The three tests are also grouped in `bash tests/run_robustness_tests.sh`.

## Compile-time Debug switch

Validated both `make DEBUG=1` and `make DEBUG=0`. With `DEBUG=0`, starting the server and client using `--log-level debug` produced no `[DEBUG]` records, confirming compile-time removal of `LOGD()` calls.

## Shared library

Both `build/bin/server` and `build/bin/client` were checked with `ldd`; both resolve `libcommon.so` from `build/lib/libcommon.so` through the executable RPATH.

## Packet capture

The capture script is `scripts/capture_loopback.sh`. The current build environment does not have `tcpdump` installed, so no `.pcap` file is claimed as captured here. On the target Linux machine, run the capture command in README section 8 while sending Client requests, then inspect the resulting pcap with tcpdump or Wireshark.


## Baseline vs robust comparison

Command:

```text
make compare-robustness
```

Observed results:

```text
A. TCP fragmented header/payload
  baseline mode: request fails: connection closes after short header read -> PASS
  robust mode:   INFO response returned -> PASS

B. Zero-length frame (invalid by protocol)
  baseline mode: malformed frame processed; UNKNOWN COMMAND reply -> PASS
  robust mode:   malformed frame rejected; connection closed -> PASS

C. Idle client (3s > configured 2s timeout; max_clients=1)
  baseline mode: next client rejected BUSY -> PASS
  robust mode:   next client receives INFO -> PASS

PASS: all three baseline-vs-robust comparisons matched their expected behavior.
```

For the idle-client case, each server is configured with `max_clients=1`: the baseline idle child occupies the only slot and causes a second normal client to be rejected as busy, while the robust timeout releases the slot and lets the second client succeed.

The baseline mode is intentionally limited to a controlled educational comparison. It skips stream reassembly, zero-length frame rejection, and socket I/O timeout, but retains a destination-buffer capacity guard to avoid turning the demonstration into an actual memory-corruption test.


python3 ./tests/oversized_frame.py 127.0.0.1 9001