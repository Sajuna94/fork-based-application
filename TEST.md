## 3.1 Project Compilation
```bash
make clean && make DEBUG=1
```
```bash
ldd build/bin/server && ldd build/bin/client
```

## 3.2 Basic Client–Server Communication
```bash
./build/bin/server \
  --bind 127.0.0.1 \
  --port 9000 \
  --max-clients 64 \
  --log-level debug
```
```bash
./build/bin/client 127.0.0.1 9000 INFO
```

# 3.3 Concurrent Client Execution
```bash
./build/bin/server --port 9000 --max-clients 32 --log-level info
```
```bash
for i in $(seq 1 10); do
  ./build/bin/client 127.0.0.1 9000 HOLD 5 &
done
wait
```

# 3.4 Robustness Comparison
```bash
./build/bin/server \
  --port 9000 \
  --max-clients 1 \
  --robustness robust \
  --io-timeout 2 \
  --log-level debug
```
```bash
./build/bin/server \
  --port 9001 \
  --max-clients 1 \
  --robustness baseline \
  --io-timeout 2 \
  --log-level debug
```
```bash
python3 tests/robustness_comparison.py 9000 9001
```
# 3.5 Compile-time and Runtime Debug Control
```bash
make clean && make DEBUG=0
./build/bin/server --log-level debug
./build/bin/client --log-level debug 127.0.0.1 9000 INFO
```
```bash
make clean && make DEBUG=1
./build/bin/server
./build/bin/server --log-level debug
```

# 3.6 TCP Packet Capture and Analysis
```bash
 sudo tcpdump -i lo -nn -s0 \
    -w captures/loopback.pcap \
    'tcp port 9000'
```
```bash
./build/bin/server --log-level debug
```
```bash
./build/bin/client --log-level debug 127.0.0.1 9000 INFO
```
```bash
sudo tcpdump -nn -tttt -r captures/loopback.pcap
```