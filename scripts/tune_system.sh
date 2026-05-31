#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════
#  tune_system.sh — Kernel & OS tuning for UDS benchmark
#
#  WARNING: Modifies kernel parameters. Run as root.
#  To restore: ./tune_system.sh --restore
# ════════════════════════════════════════════════════════════════
set -euo pipefail

SAVE_FILE="/tmp/uds_bench_sysctl_save.conf"
RESTORE=0

[[ "${1:-}" == "--restore" ]] && RESTORE=1

if [[ $EUID -ne 0 ]]; then
    echo "⚠  This script requires root. Run: sudo $0"
    exit 1
fi

restore() {
    if [[ -f "$SAVE_FILE" ]]; then
        echo "Restoring saved kernel parameters..."
        sysctl -p "$SAVE_FILE" 2>/dev/null || true
        rm -f "$SAVE_FILE"
        echo "✓ Restored."
    else
        echo "No saved state found at $SAVE_FILE"
    fi
}

if [[ $RESTORE -eq 1 ]]; then
    restore
    exit 0
fi

# ── Save current values ───────────────────────────────────────
echo "# UDS bench sysctl save $(date)" > "$SAVE_FILE"
for key in \
    net.core.rmem_max \
    net.core.wmem_max \
    net.core.rmem_default \
    net.core.wmem_default \
    net.unix.max_dgram_qlen \
    net.core.somaxconn \
    net.core.netdev_max_backlog \
    kernel.sched_min_granularity_ns \
    kernel.sched_wakeup_granularity_ns \
    vm.swappiness; do
    val=$(sysctl -n "$key" 2>/dev/null || echo "")
    [[ -n "$val" ]] && echo "${key} = ${val}" >> "$SAVE_FILE"
done

echo "Saved current state to $SAVE_FILE"

# ── Apply optimizations ───────────────────────────────────────
apply() {
    local key=$1 val=$2
    if sysctl -w "${key}=${val}" > /dev/null 2>&1; then
        echo "  ✓ ${key} = ${val}"
    else
        echo "  ⚠ ${key} (skipped, not available on this kernel)"
    fi
}

echo ""
echo "Applying UDS performance tuning..."
echo ""

echo "── Socket buffer sizes ──"
apply net.core.rmem_max         134217728   # 128 MB
apply net.core.wmem_max         134217728
apply net.core.rmem_default     16777216    # 16 MB
apply net.core.wmem_default     16777216

echo ""
echo "── UNIX socket limits ──"
apply net.unix.max_dgram_qlen   8192        # DGRAM queue depth
apply net.core.somaxconn        65535       # listen backlog
apply net.core.netdev_max_backlog 65536

echo ""
echo "── Scheduler (reduce latency jitter) ──"
apply kernel.sched_min_granularity_ns   500000   # 0.5ms
apply kernel.sched_wakeup_granularity_ns 1000000  # 1ms

echo ""
echo "── Memory ──"
apply vm.swappiness 1

# ── File descriptor limits ─────────────────────────────────────
echo ""
echo "── File descriptor limits ──"
ulimit -n 1048576 2>/dev/null && echo "  ✓ ulimit -n 1048576" || \
    echo "  ⚠ Could not raise ulimit (not a hard limit issue)"

# ── CPU governor ───────────────────────────────────────────────
echo ""
echo "── CPU frequency governor ──"
if command -v cpupower &>/dev/null; then
    cpupower frequency-set -g performance > /dev/null 2>&1 && \
        echo "  ✓ CPU governor set to performance" || \
        echo "  ⚠ cpupower failed (may need root or driver)"
elif ls /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor &>/dev/null 2>&1; then
    for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo performance > "$gov" 2>/dev/null || true
    done
    echo "  ✓ CPU governors set to performance (via sysfs)"
else
    echo "  ⚠ CPU governor control not available"
fi

# ── IRQ balance ───────────────────────────────────────────────
echo ""
echo "── IRQ affinity ──"
if systemctl is-active irqbalance &>/dev/null; then
    systemctl stop irqbalance 2>/dev/null && \
        echo "  ✓ irqbalance stopped (reduces interrupt jitter)" || true
else
    echo "  ✓ irqbalance already stopped"
fi

# ── Transparent hugepages ─────────────────────────────────────
THP=/sys/kernel/mm/transparent_hugepage/enabled
if [[ -f "$THP" ]]; then
    echo never > "$THP" 2>/dev/null && \
        echo "  ✓ Transparent hugepages disabled (reduces latency spikes)" || true
fi

echo ""
echo "══════════════════════════════════════════════════════════"
echo "✓ Tuning applied. Run benchmarks now."
echo "  Restore with: sudo $0 --restore"
echo ""
echo "Additional manual tips:"
echo "  • Pin server/client to separate cores:"
echo "      taskset -c 0 ./bin/uds_server -m 0 -i 2 ..."
echo "      taskset -c 1 ./bin/uds_client -m 0 -i 2 ..."
echo "  • Disable NUMA interleaving if multi-socket system"
echo "  • Use 'perf stat' or 'perf record' for profiling"
