# UDS Performance Benchmark Suite

A comprehensive Unix Domain Socket (UDS) benchmark suite measuring **throughput**, **QPS**, and **latency** across all meaningful socket/IO mode combinations.

## Test Matrix

| Dimension      | Values                                      |
|----------------|---------------------------------------------|
| Socket type    | `STREAM`, `DGRAM`                           |
| IO mode        | `BLOCKING`, `NONBLOCKING`, `EPOLL`          |
| Socket count   | `1`, `4`, `8` (configurable)                |
| Thread count   | `1..N` per socket                           |
| Message sizes  | `64B` → `64KB`                              |
| Metrics        | QPS, Throughput (MB/s), RTT Latency (min/avg/p50/p90/p99/p99.9/max) |

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    run_bench.sh                             │
│   (orchestrates server + client, all combinations)          │
└────────────┬───────────────────────┬────────────────────────┘
             │                       │
    ┌────────▼────────┐    ┌─────────▼───────────┐
    │   uds_server    │    │     uds_client       │
    │                 │    │                      │
    │  • STREAM/DGRAM │◄──►│  • Sends msgs        │
    │  • BLOCK        │    │  • Timestamps RTT    │
    │  • NONBLOCK     │    │  • HDR histogram     │
    │  • EPOLL        │    │  • CSV export        │
    └─────────────────┘    └──────────────────────┘
             │
    ┌────────▼────────┐
    │ uds_throughput  │
    │                 │
    │  Multi-sender   │
    │  flood mode     │
    │  (max bandwidth)│
    └─────────────────┘
```

### Latency Measurement

Every message carries a `msg_header_t` with a `CLOCK_MONOTONIC` nanosecond timestamp. The client records the full round-trip time (RTT) and feeds it into an **HDR-inspired histogram** with 1 µs bucket resolution, enabling accurate p99.9 measurements without memory overhead.

```
 Client sends ──[ts_send_ns]──► Server echoes ──► Client records RTT
```

## Build

```bash
# Prerequisites: gcc, make, pthreads (standard on all Linux distros)
make

# Optional: python3 + matplotlib for charts
pip3 install matplotlib numpy
```

## Quick Start

```bash
# 1. Tune system (optional but recommended, needs root)
sudo ./scripts/tune_system.sh

# 2. Run full benchmark suite (~30 min)
./scripts/run_bench.sh

# 3. Quick run (~5 min)
./scripts/run_bench.sh --quick

# 4. Generate charts
python3 scripts/plot_results.py results/uds_bench_*.csv
```

## Manual Usage

### Server
```
bin/uds_server [options]

  -m <0|1>    Socket mode: 0=STREAM (default), 1=DGRAM
  -i <0|1|2>  IO mode: 0=BLOCKING (default), 1=NONBLOCKING, 2=EPOLL
  -n <N>      Number of sockets (default: 1)
  -s <bytes>  Message size (default: 1024)
  -d <sec>    Duration (default: 10)
  -v          Verbose
```

### Client
```
bin/uds_client [options]

  -m <0|1>    Socket mode
  -i <0|1|2>  IO mode
  -n <N>      Number of sockets
  -t <N>      Threads per socket
  -s <bytes>  Message size
  -d <sec>    Duration
  -p <N>      Pipeline depth (EPOLL mode, default: 4)
  -w <sec>    Warmup seconds (default: 1)
  -o <file>   CSV output file
  -v          Verbose
```

### Example: STREAM + EPOLL, 4 sockets, 4KB messages
```bash
# Terminal 1 — server
bin/uds_server -m 0 -i 2 -n 4 -s 4096 -d 30

# Terminal 2 — client
bin/uds_client -m 0 -i 2 -n 4 -s 4096 -d 10 -o results/test.csv
```

### Example: DGRAM + BLOCKING, single socket, 512B messages
```bash
bin/uds_server -m 1 -i 0 -n 1 -s 512 -d 30 &
bin/uds_client -m 1 -i 0 -n 1 -s 512 -d 10 -o results/dgram.csv
```

### Example: Max throughput flood
```bash
# Terminal 1 — server (flag -S = server mode)
bin/uds_throughput -S -s 65536 -n 8 -d 20

# Terminal 2 — client (8 senders)
bin/uds_throughput -s 65536 -n 8 -d 15
```

## Output Format

### Console
```
╔══════════════════════════════════════════════════════╗
║  UDS STREAM/EPOLL S4 T1 MSG4096                     ║
╠══════════════════════════════════════════════════════╣
║  Mode : STREAM    IO: EPOLL      Sockets: 4          ║
║  Threads: 1    MsgSize: 4096   Duration: 10s         ║
╠══════════════════════════════════════════════════════╣
║  QPS         :          847,293 msg/s                ║
║  Throughput  :         3,302.71 MB/s                 ║
║  Total msgs  :        8,472,930                      ║
╠══════════════════════════════════════════════════════╣
║  Latency (RTT)                                       ║
║    min   :       1.23 µs                             ║
║    avg   :       4.72 µs                             ║
║    p50   :       4.10 µs                             ║
║    p90   :       7.30 µs                             ║
║    p99   :      18.50 µs                             ║
║    p99.9 :      45.80 µs                             ║
║    max   :     312.00 µs                             ║
╚══════════════════════════════════════════════════════╝
```

### CSV columns
```
label, sock_mode, io_mode, sockets, threads, msg_size,
qps, tput_mb, avg_us, p50_us, p90_us, p99_us, p999_us, max_us
```

## Benchmark Tips

### CPU Pinning (reduces jitter significantly)
```bash
# Pin server to core 0, client to core 1
taskset -c 0 bin/uds_server -m 0 -i 2 -n 1 -s 1024 -d 30 &
taskset -c 1 bin/uds_client -m 0 -i 2 -n 1 -s 1024 -d 15
```

### Profiling
```bash
# CPU profile (server)
perf record -g -p $(pgrep uds_server) -- sleep 10
perf report

# Syscall trace
strace -c -p $(pgrep uds_server)

# Flamegraph
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

### Interpretation Guide

| Metric      | What it reveals                                          |
|-------------|----------------------------------------------------------|
| **QPS**     | Small message rate; CPU-bound bottleneck visibility      |
| **Tput**    | Large message efficiency; buffer/copy costs              |
| **avg RTT** | Typical latency under load                               |
| **p99 RTT** | Tail latency; scheduler jitter, buffer bloat             |
| **p99.9**   | Outlier latency; GC pauses, interrupt storms             |

### Expected Rankings (typical Linux)

**QPS (small messages):**  
`EPOLL ≈ BLOCK > NONBLOCK (spin wastes CPU)`

**Throughput (large messages):**  
`STREAM >> DGRAM (DGRAM limited by max_dgram_qlen)`

**Latency (p99):**  
`BLOCK < EPOLL < NONBLOCK` (NONBLOCK spins, causing jitter)

**Scalability:**  
`EPOLL scales best with socket count (single thread handles N connections)`

## File Structure

```
uds-bench/
├── include/
│   └── uds_bench.h         # Shared types, HDR histogram, utilities
├── src/
│   ├── uds_server.c        # Echo server (STREAM/DGRAM × BLOCK/NB/EPOLL)
│   ├── uds_client.c        # Latency + QPS client
│   └── uds_throughput.c    # Pure throughput flood test
├── scripts/
│   ├── run_bench.sh         # Master test runner
│   ├── plot_results.py      # Chart generator
│   └── tune_system.sh       # Kernel parameter tuning
├── results/                 # CSV results and charts (generated)
├── bin/                     # Compiled binaries (generated)
└── Makefile
```
