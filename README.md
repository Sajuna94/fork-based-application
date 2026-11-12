# fork-info-server：Linux C / fork() Client–Server 專案

本專案使用 IPv4 TCP。Server 每接受一個連線就 `fork()` 一個 child process 處理，不使用 threads。Client 可要求 Server 回傳主機系統資訊，也提供 ECHO 與 HOLD 測試命令。

## 1. 專案架構

```text
fork-info-server/
├── Makefile
├── README.md
├── include/
│   └── common.h             # 共用 API、封包上限、log macro
├── src/
│   ├── common.c             # 編譯成 libcommon.so
│   ├── server.c             # socket / poll / fork / SIGCHLD reap
│   └── client.c             # Client CLI
├── tests/
│   ├── concurrency_test.sh  # 10 個 client 同時保持連線，驗證 child 數
│   ├── fragmented_frame.py  # 將一個 frame 切成多次 send
│   ├── oversized_frame.py   # 傳送超出長度上限的 frame
│   ├── partial_frame.py     # 傳送半截 frame 後斷線，再驗證 Server
│   ├── run_robustness_tests.sh
│   ├── robustness_comparison.py  # baseline / robust 成對測試
│   └── run_robustness_comparison.sh
└── scripts/
    └── capture_loopback.sh  # tcpdump loopback 擷取
```

編譯後：

```text
build/
├── bin/server
├── bin/client
└── lib/libcommon.so
```

`server` 與 `client` 都透過 `-lcommon` 動態連結 `libcommon.so`；執行檔使用 `$ORIGIN/../lib` 作為 RPATH，因此一般不需要另外設定 `LD_LIBRARY_PATH`。

## 2. 通訊協定

每個 TCP application message 使用相同 framing：

```text
+----------------------------+--------------------------+
| 4-byte unsigned length     | payload                  |
| network byte order         | UTF-8 / ASCII command    |
+----------------------------+--------------------------+
```

目前 payload 上限為 4096 bytes。TCP 是 byte stream，不能假設一次 `recv()` 就能取得完整 header 或 payload；`libcommon.so` 的 `app_recv_frame()` 會持續讀取直到指定長度收完，並處理 EINTR、EOF、半截封包及長度檢查。

Server 預設使用 `--robustness robust`。另提供 `--robustness baseline` 作為課堂對照實驗：刻意以單次 `recv()` 讀取 header/payload、接受零長度 frame，且不設定 socket I/O timeout。此模式仍保留緩衝區容量檢查，避免為了展示差異而真的執行記憶體越界；它只用於重現本專案選定的三種缺陷，**不可用於正式服務**。

支援命令：

| 命令 | 功能 |
|---|---|
| `INFO` | 回傳 hostname、OS/kernel、CPU 數量、記憶體資訊 |
| `ECHO <text>` | 回傳文字，方便驗證 request/response |
| `HOLD <1-10>` | Server child 暫停指定秒數再回覆，專門用於併發測試 |

每個連線目前處理一個 request/response 後關閉。要擴展成 persistent connection，可在 child 的 `handle_client()` 中將收發包流程改成迴圈。

## 3. 編譯環境與建置

需求：Linux、GCC、GNU Make、Python 3（只用於測試腳本）；封包擷取另需 tcpdump。

```bash
cd fork-info-server
make
```

預設 `DEBUG=1`，編譯時保留 Debug log。若要在編譯期關閉 Debug log：

```bash
make clean
make DEBUG=0
```

查看動態連結：

```bash
ldd build/bin/server | grep libcommon
ldd build/bin/client | grep libcommon
```

預期會指向專案的 `build/lib/libcommon.so`。

## 4. 啟動與基本執行

### Terminal 1：啟動 Server

```bash
./build/bin/server --bind 127.0.0.1 --port 9000 --max-clients 64 --log-level debug
```

參數：

- `--bind 127.0.0.1`：只接受本機連線。跨電腦測試時改成 `0.0.0.0` 或 Server 的網卡 IP。
- `--port 9000`：監聽 TCP 9000。
- `--max-clients 64`：最多同時保留 64 個 child worker；範圍 1–1024。
- `--log-level debug`：執行期 log 門檻。
- `--robustness robust|baseline`：選擇正常防禦模式或教學用缺陷對照模式，預設 `robust`。
- `--io-timeout 15`：robust 模式下的 socket 收發 timeout 秒數，預設 15 秒；baseline 模式刻意不套用。

看到 `server ready` 即代表啟動成功。按 `Ctrl+C` 可停止 Server。

### Terminal 2：Client 要求系統資訊

```bash
./build/bin/client 127.0.0.1 9000 INFO
```

其他命令：

```bash
./build/bin/client 127.0.0.1 9000 ECHO hello
./build/bin/client 127.0.0.1 9000 HOLD 5
```

Client 和 Server 都支援 `--log-level error|warn|info|debug`，例如：

```bash
./build/bin/client --log-level debug 127.0.0.1 9000 INFO
```

## 5. 兩層 Debug log 控制

兩層控制是互相疊加的：

1. **Compile-time**：`make DEBUG=1` 才會編入 `LOGD()`；`make DEBUG=0` 時，`LOGD()` 在編譯期被移除。
2. **Runtime**：`--log-level error|warn|info|debug` 控制已編入的 log 類別。

因此，`make DEBUG=0` 即使執行時指定 `--log-level debug`，仍不會出現 `LOGD()` 訊息。`ERROR/WARN/INFO` 不受 compile-time Debug 開關影響。

驗證方式：

```bash
make clean && make DEBUG=1
./build/bin/server --port 9000 --log-level debug
# 另一個 terminal 執行 client；可看到 DEBUG 訊息

# 停止 Server 後：
make clean && make DEBUG=0
./build/bin/server --port 9000 --log-level debug
# 再執行 client；不會出現 DEBUG macro 訊息
```

## 6. 10 個以上 Client 同時連線：Runtime proof

在專案根目錄執行：

```bash
make test
```

測試腳本會自行啟動測試 Server，並同時啟動 10 個 `HOLD 4` Client。每個 child 都會維持約 4 秒，所以 parent 可在同一時間持有至少 10 個 worker。Server log 會記錄 `active_children` 和 `peak_children`。

預期結果形式：

```text
Observed peak concurrent child processes: 10
PASS: runtime proof confirms at least 10 clients were served concurrently.
```

實際數字依執行時環境而定；若顯示 10 或更大，即是本次執行的併發證明。這不是只依賴 listen backlog：每個 HOLD request 都由獨立 child process 執行。

也可手動觀察：

```bash
# Terminal 1：Server
./build/bin/server --port 9000 --max-clients 32 --log-level info

# Terminal 2：同時啟動 10 個 client
for i in $(seq 1 10); do
  ./build/bin/client 127.0.0.1 9000 HOLD 5 &
done
wait
```

## 7. Robustness 測試：用 baseline / robust 成對證明差異

只有執行安全版並看到 `PASS`，只能證明防禦有效；不能直接證明「沒有防禦時」會出現什麼差異。因此本專案額外提供 `baseline` 對照模式，將相同測試輸入分別送給兩個 Server，並比較實際結果。

### 一次執行三組成對實驗

```bash
make compare-robustness
```

腳本會在兩個不同 Port 啟動 Server：

- Robust Server：`--robustness robust --io-timeout 2`
- Baseline Server：`--robustness baseline --io-timeout 2`

兩者接收相同格式的異常請求。預期輸出如下（實際 log 文字可能略有不同）：

```text
A. TCP fragmented header/payload
  baseline mode: request fails: baseline closes after a short header read -> PASS
  robust mode:   INFO response returned: robust recv_all reassembles the stream -> PASS

B. Zero-length frame (invalid by protocol)
  baseline mode: malformed frame is processed: baseline replies UNKNOWN COMMAND -> PASS
  robust mode:   malformed frame is rejected: robust closes the connection -> PASS

C. Idle client (3s > configured 2s timeout; max_clients=1)
  baseline mode: next client is rejected BUSY: idle baseline worker occupies the only slot -> PASS
  robust mode:   next client receives INFO: robust timeout frees the worker slot -> PASS

PASS: all three baseline-vs-robust comparisons matched their expected behavior.
```

### 三組實驗的行為差異

| 異常情境 | Baseline（未啟用該項防禦） | Robust（啟用防禦） | 證明的機制 |
|---|---|---|---|
| A. TCP header/payload 分段抵達 | 單次 `recv()` 只讀到部分 header，連線被錯誤拒絕；合法的 INFO 請求失敗 | `recv_all()` 循環讀取，最終成功回傳 INFO | TCP Stream 完整讀取 |
| B. 長度為 0 的非法 frame | 將空字串當成請求，回覆 unknown command；錯誤封包被當成可處理請求 | 在讀取 payload 前以 `EMSGSIZE` 拒絕並關閉該連線 | Frame 長度／格式驗證 |
| C. Client 連線後閒置超過 2 秒 | 測試 Server 的 `max_clients=1`；閒置 Client 長時間占用唯一 Child，第二個正常 Client 收到 BUSY | 2 秒 socket timeout 到期，Child 被回收，第二個正常 Client 成功取得 INFO | Socket I/O timeout、資源回收 |

第三項測試刻意把 `max_clients` 設為 1，讓差異可以直接觀察：baseline 的閒置連線吃掉唯一 Worker slot，下一個正常使用者因此被拒絕；robust 模式的 timeout 讓 Child 結束並釋放 slot，下一個正常使用者可以成功取得 INFO。

### 單獨執行安全版的額外測試

下列測試保留原本三項健壯性測試，可用來檢查 robust 模式的更多細節：

```bash
make test-robustness
```

其中包含：

- `fragmented_frame.py`：逐 byte 傳送合法 INFO frame，確認伺服器可重組。
- `oversized_frame.py`：傳送 4097 bytes 的長度欄位，確認超過 4096 bytes 的 frame 被拒絕。
- `partial_frame.py`：傳送部分 payload 後關閉連線，再確認新的 Client 仍可取得 INFO。

### 手動啟動兩種模式

```bash
# Terminal 1：安全版，2 秒 timeout 方便觀察
./build/bin/server --port 9000 --robustness robust --io-timeout 2 --log-level debug

# Terminal 2：教學用 baseline，請使用另一個 port
./build/bin/server --port 9001 --robustness baseline --io-timeout 2 --log-level debug
```

`baseline` 是刻意保留的缺陷示範，並非建議的生產設定。它只關閉本節比較的三項保護，並沒有移除所有安全檢查；例如仍保留目的緩衝區的容量檢查，避免測試本身造成記憶體破壞。

## 8. tcpdump 封包擷取與分析

### Loopback（同一台 Linux）

需要先安裝 tcpdump（Debian / Ubuntu）：

```bash
sudo apt update
sudo apt install tcpdump
```

Terminal 1 擷取：

```bash
mkdir -p captures
sudo tcpdump -i lo -nn -s0 -w captures/loopback.pcap 'tcp port 9000'
```

Terminal 2 產生流量：

```bash
./build/bin/client 127.0.0.1 9000 INFO
./build/bin/client 127.0.0.1 9000 ECHO packet-test
```

回到 Terminal 1 按 `Ctrl+C` 結束擷取。分析封包：

```bash
sudo tcpdump -nn -tttt -r captures/loopback.pcap
sudo tcpdump -nn -X -r captures/loopback.pcap
```

`-nn` 不解析主機名稱與服務名稱，`-tttt` 顯示時間，`-X` 同時顯示 hex 與 ASCII。可以觀察 TCP 三向交握、Client 到 Server 的 request，以及 Server 回傳的 response。Application payload 前 4 bytes 是 network byte order 長度，後面才是命令或系統資訊。

也可使用 Wireshark 開啟 `captures/loopback.pcap`，選取封包後使用 **Follow → TCP Stream**，較容易檢視重組後的 application stream。

### 不同電腦間

Server：

```bash
./build/bin/server --bind 0.0.0.0 --port 9000 --log-level info
```

在 Server 所在電腦擷取實際網卡（例如 `eth0`，請以 `ip -br link` 查詢）：

```bash
sudo tcpdump -i eth0 -nn -s0 -w server-traffic.pcap 'tcp port 9000'
```

Client：

```bash
./build/bin/client <SERVER_IP> 9000 INFO
```

需確認防火牆允許 TCP 9000。不要在不可信任的公網直接暴露這個未加入認證與加密的示範服務。

## 9. Robustness 設計摘要

| 異常情況 | 處理方式 |
|---|---|
| `recv()` / `send()` 被訊號中斷 | 對 `EINTR` 重試 |
| TCP 資料分段抵達 | `recv_all()` 依長度讀滿 |
| 長度欄位為 0 或超過上限 | 在讀 payload 前拒絕 |
| Client 提前斷線 | 偵測 EOF / `ECONNRESET`，只結束該 child |
| Client 長時間不傳資料 | `SO_RCVTIMEO` / `SO_SNDTIMEO` |
| 對端關閉造成寫入錯誤 | `MSG_NOSIGNAL`，另忽略 `SIGPIPE` |
| Child 結束成 zombie | parent 收到 `SIGCHLD` 後 `waitpid(WNOHANG)` |
| Child 數量超過設定上限 | 回覆 BUSY 並關閉新連線 |
| `fork()` 失敗 | 回覆錯誤並關閉連線，不影響 parent |
| Server 收到 SIGINT / SIGTERM | 停止 accept，關閉 listener，等待 worker 結束 |

## 10. 注意事項與可延伸方向

- 這是教學與作業示範，未加入 TLS、使用者認證或 request rate limit；僅建議在 localhost 或可信任的區域網路測試。
- `INFO` 回傳的是 Server 主機資訊，不是 Client 本機資訊。
- `HOLD` 是併發驗證用命令，僅允許 1–10 秒，避免任意長時間佔用 worker。
- 系統資訊透過 `uname()`、`sysconf()`、`/proc/meminfo` 取得，適用於 Linux。
