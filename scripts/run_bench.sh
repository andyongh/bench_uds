#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
#  run_bench.sh — UDS Performance Test Suite
#
#  Runs all combinations:
#    Socket type : STREAM, DGRAM
#    IO mode     : BLOCKING, NONBLOCKING, EPOLL
#    Sockets     : 1, 4, 8
#    Threads     : 1 (per socket), 4 total
#    Msg sizes   : 64, 512, 1024, 4096, 16384, 65536
#
#  Usage:
#    ./scripts/run_bench.sh [--quick] [--csv results/out.csv] [--duration 10]
# ═══════════════════════════════════════════════════════════════
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${SCRIPT_DIR}/.."
BIN="${ROOT}/bin"
RESULTS="${ROOT}/results"
mkdir -p "$RESULTS"

# ── Defaults ──────────────────────────────────────────────────
DURATION=10
WARMUP=1
CSV="${RESULTS}/uds_bench_$(date +%Y%m%d_%H%M%S).csv"
QUICK=0
LOG="${RESULTS}/bench.log"

# ── Parse args ─────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick)    QUICK=1; shift ;;
        --csv)      CSV="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--quick] [--csv FILE] [--duration SEC]"
            exit 0 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

if [[ $QUICK -eq 1 ]]; then
    MSG_SIZES=(512 4096)
    SOCKET_COUNTS=(1 4)
    DURATION=5
else
    MSG_SIZES=(64 512 1024 4096 16384 65536)
    SOCKET_COUNTS=(1 4 8)
fi

# ── Helpers ────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'
YELLOW='\033[1;33m'; BOLD='\033[1m'; RESET='\033[0m'

log()  { echo -e "${CYAN}[$(date +%H:%M:%S)]${RESET} $*" | tee -a "$LOG"; }
ok()   { echo -e "${GREEN}✓${RESET} $*"; }
warn() { echo -e "${YELLOW}⚠${RESET} $*"; }
err()  { echo -e "${RED}✗${RESET} $*"; }

# sock_mode: 0=STREAM 1=DGRAM
# io_mode:   0=BLOCK  1=NONBLOCK  2=EPOLL
# declare -A IO_NAMES=([0]="BLOCK" [1]="NONBLOCK" [2]="EPOLL")
# declare -A SOCK_NAMES=([0]="STREAM" [1]="DGRAM")
declare IO_NAMES=([0]="BLOCK" [1]="NONBLOCK" [2]="EPOLL")
declare SOCK_NAMES=([0]="STREAM" [1]="DGRAM")

cleanup() {
    pkill -f uds_server 2>/dev/null || true
    rm -f /tmp/uds_bench* 2>/dev/null || true
}
trap cleanup EXIT INT TERM

check_binaries() {
    local missing=0
    for bin in uds_server uds_client uds_throughput; do
        if [[ ! -x "${BIN}/${bin}" ]]; then
            err "Missing binary: ${BIN}/${bin}  (run 'make' first)"
            missing=1
        fi
    done
    [[ $missing -eq 0 ]]
}

sysinfo() {
    echo ""
    echo -e "${BOLD}═══ System Information ═══════════════════════════════════${RESET}"
    echo "  Host    : $(hostname)"
    echo "  Kernel  : $(uname -r)"
    echo "  CPU     : $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs || echo 'N/A')"
    echo "  Cores   : $(nproc)"
    echo "  RAM     : $(free -h | awk '/^Mem:/{print $2}')"
    echo "  Date    : $(date)"
    echo ""
}

# ── Run one test case ──────────────────────────────────────────
run_one() {
    local sm=$1    # 0=STREAM, 1=DGRAM
    local im=$2    # 0=BLOCK, 1=NONBLOCK, 2=EPOLL
    local ns=$3    # num sockets
    local ms=$4    # msg size

    local label="${SOCK_NAMES[$sm]}/${IO_NAMES[$im]} S=${ns} MSG=${ms}"
    log "Testing: $label"

    cleanup

    # Start server (all sockets)
    "${BIN}/uds_server" \
        -m "$sm" -i "$im" -n "$ns" \
        -s "$ms" -d "$((DURATION + WARMUP + 2))" \
        >> "$LOG" 2>&1 &
    local server_pid=$!

    # Wait for sockets to appear
    local waited=0
    for (( i=0; i<ns; i++ )); do
        while [[ ! -S "/tmp/uds_bench.${i}" ]]; do
            sleep 0.1
            waited=$((waited + 1))
            if [[ $waited -gt 50 ]]; then
                err "Server socket /tmp/uds_bench.${i} not ready"
                kill "$server_pid" 2>/dev/null || true
                return 1
            fi
        done
    done
    sleep 0.2  # extra settle time

    # Run client
    "${BIN}/uds_client" \
        -m "$sm" -i "$im" -n "$ns" \
        -s "$ms" -d "$DURATION" \
        -o "$CSV" \
        2>&1 | tee -a "$LOG"

    # Cleanup server
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    cleanup

    sleep 0.5
    ok "Done: $label"
}

# ── Run throughput flood test ──────────────────────────────────
run_throughput() {
    local ms=$1
    local ns=$2
    local type_flag=""
    local type_name="STREAM"

    log "Throughput flood: type=${type_name} senders=${ns} msg=${ms}"
    cleanup

    "${BIN}/uds_throughput" -S -s "$ms" -d "$DURATION" -n "$ns" \
        >> "$LOG" 2>&1 &
    local server_pid=$!

    sleep 0.3
    "${BIN}/uds_throughput" -s "$ms" -d "$DURATION" -n "$ns" \
        $type_flag 2>&1 | tee -a "$LOG"

    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    cleanup
}

# ══════════════════════════════════════════════════════════════
#  MAIN
# ══════════════════════════════════════════════════════════════
echo -e "${BOLD}╔══════════════════════════════════════════════════════════╗${RESET}"
echo -e "${BOLD}║         UDS Performance Benchmark Suite                  ║${RESET}"
echo -e "${BOLD}╚══════════════════════════════════════════════════════════╝${RESET}"

sysinfo

if ! check_binaries; then
    echo ""
    echo "  Build with:  cd ${ROOT} && make"
    exit 1
fi

> "$LOG"
log "Results CSV: $CSV"
log "Log file:    $LOG"
log "Duration:    ${DURATION}s per test"
echo ""

TOTAL_TESTS=$(( ${#MSG_SIZES[@]} * ${#SOCKET_COUNTS[@]} * 4 ))
DONE=0

# ── Phase 1: STREAM tests ─────────────────────────────────────
echo -e "\n${BOLD}━━━ Phase 1: STREAM (all IO modes) ━━━━━━━━━━━━━━━━━━━━━━${RESET}\n"

for ms in "${MSG_SIZES[@]}"; do
    for ns in "${SOCKET_COUNTS[@]}"; do
        # BLOCK
        run_one 0 0 "$ns" "$ms" || warn "Test failed (skipping)"
        DONE=$((DONE+1))
        echo "Progress: ${DONE}/${TOTAL_TESTS}"

        # NONBLOCK
        run_one 0 1 "$ns" "$ms" || warn "Test failed (skipping)"
        DONE=$((DONE+1))

        # EPOLL (STREAM only — most practical)
        run_one 0 2 "$ns" "$ms" || warn "Test failed (skipping)"
        DONE=$((DONE+1))
    done
done

# ── Phase 2: DGRAM tests ──────────────────────────────────────
echo -e "\n${BOLD}━━━ Phase 2: DGRAM (blocking) ━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}\n"

for ms in "${MSG_SIZES[@]}"; do
    # DGRAM max size is limited (typically 128KB UDS)
    if [[ $ms -gt 32768 ]]; then continue; fi
    for ns in "${SOCKET_COUNTS[@]}"; do
        run_one 1 0 "$ns" "$ms" || warn "Test failed (skipping)"
        DONE=$((DONE+1))
    done
done

# ── Phase 3: Max throughput flood ─────────────────────────────
echo -e "\n${BOLD}━━━ Phase 3: Max Throughput Flood (multi-sender) ━━━━━━━━${RESET}\n"
for ms in 4096 65536; do
    for ns in 1 4 8; do
        run_throughput "$ms" "$ns" || warn "Throughput test failed"
    done
done

# ── Summary ───────────────────────────────────────────────────
echo ""
echo -e "${BOLD}╔══════════════════════════════════════════════════════════╗${RESET}"
echo -e "${BOLD}║  Benchmark Complete!                                     ║${RESET}"
echo -e "${BOLD}╠══════════════════════════════════════════════════════════╣${RESET}"
echo -e "║  Results CSV : ${CSV}"
echo -e "║  Log file    : ${LOG}"
echo -e "${BOLD}╚══════════════════════════════════════════════════════════╝${RESET}"

# Quick summary from CSV
if [[ -f "$CSV" ]]; then
    echo ""
    echo -e "${BOLD}Top 5 Highest QPS Results:${RESET}"
    (head -1 "$CSV"; tail -n +2 "$CSV" | sort -t, -k7 -rn | head -5) \
        | column -t -s,
fi

echo ""
echo "Run './scripts/plot_results.py ${CSV}' to generate charts."
