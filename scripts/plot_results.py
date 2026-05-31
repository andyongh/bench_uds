#!/usr/bin/env python3
"""
plot_results.py — Generate performance charts from UDS bench CSV

Usage:
    python3 scripts/plot_results.py results/uds_bench_*.csv
    python3 scripts/plot_results.py results/out.csv --output results/charts/
"""
import sys
import os
import argparse
import csv
from pathlib import Path
from collections import defaultdict

# Try to import matplotlib; give clear error if missing
try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import matplotlib.ticker as mticker
    import numpy as np
    HAS_MPL = True
except ImportError:
    HAS_MPL = False
    print("WARNING: matplotlib not found. Install with: pip3 install matplotlib numpy")
    print("Falling back to text summary only.")

# ─── Color palette ──────────────────────────────────────────────
COLORS = {
    'BLOCK':    '#2196F3',
    'NONBLOCK': '#FF9800',
    'EPOLL':    '#4CAF50',
    'STREAM':   '#9C27B0',
    'DGRAM':    '#F44336',
}
MARKERS = {'BLOCK': 'o', 'NONBLOCK': 's', 'EPOLL': '^'}

def parse_csv(path):
    rows = []
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                rows.append({
                    'label':     row['label'],
                    'sock_mode': row['sock_mode'],
                    'io_mode':   row['io_mode'],
                    'sockets':   int(row['sockets']),
                    'threads':   int(row['threads']),
                    'msg_size':  int(row['msg_size']),
                    'qps':       float(row['qps']),
                    'tput_mb':   float(row['tput_mb']),
                    'avg_us':    float(row['avg_us']),
                    'p50_us':    float(row['p50_us']),
                    'p99_us':    float(row['p99_us']),
                    'p999_us':   float(row['p999_us']),
                    'max_us':    float(row['max_us']),
                })
            except (KeyError, ValueError):
                pass
    return rows

def text_summary(rows):
    """Print a formatted text table."""
    print("\n" + "═"*90)
    print(f"{'Label':<38} {'QPS':>10} {'Tput MB/s':>10} {'avg µs':>8} {'p99 µs':>8} {'p99.9 µs':>9}")
    print("─"*90)
    for r in sorted(rows, key=lambda x: -x['qps']):
        print(f"{r['label']:<38} {r['qps']:>10,.0f} {r['tput_mb']:>10.2f} "
              f"{r['avg_us']:>8.2f} {r['p99_us']:>8.2f} {r['p999_us']:>9.2f}")
    print("═"*90)

    # Best per category
    print("\n── Best QPS by IO mode ──")
    best = defaultdict(lambda: {'qps': 0})
    for r in rows:
        key = r['io_mode']
        if r['qps'] > best[key]['qps']:
            best[key] = r
    for k, v in sorted(best.items()):
        print(f"KV{k}:{v}")
        print(f"  {k:12s}: {v['qps']:12,.0f} msg/s  @ msg={v['msg_size']} "
              f"S={v['sockets']} — {v['label']}")

    print("\n── Best Throughput ──")
    top = sorted(rows, key=lambda x: -x['tput_mb'])[:3]
    for i, r in enumerate(top):
        print(f"  #{i+1}: {r['tput_mb']:.2f} MB/s  — {r['label']}")

    print("\n── Lowest Latency (p99) ──")
    lat = [r for r in rows if r['p99_us'] > 0]
    lat.sort(key=lambda x: x['p99_us'])
    for r in lat[:3]:
        print(f"  p99={r['p99_us']:.2f} µs  avg={r['avg_us']:.2f} µs  — {r['label']}")

if not HAS_MPL:
    def make_charts(rows, outdir):
        text_summary(rows)
        return

def make_charts(rows, outdir):
    os.makedirs(outdir, exist_ok=True)
    text_summary(rows)

    # ── fig 1: QPS vs Message Size (per IO mode, STREAM) ──────
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))
    fig.suptitle('UDS Benchmark — QPS vs Message Size', fontsize=14, fontweight='bold')

    for ax, sock in zip(axes, ['STREAM', 'DGRAM']):
        data = defaultdict(lambda: defaultdict(list))
        for r in rows:
            if r['sock_mode'] == sock:
                data[r['io_mode']][r['msg_size']].append(r['qps'])

        for io_mode, szdict in sorted(data.items()):
            sizes = sorted(szdict.keys())
            qps   = [max(szdict[s]) for s in sizes]
            ax.plot(sizes, qps,
                    color=COLORS.get(io_mode, '#888'),
                    marker=MARKERS.get(io_mode, 'o'),
                    label=io_mode, linewidth=2, markersize=7)

        ax.set_xscale('log', base=2)
        ax.set_yscale('log')
        ax.set_xlabel('Message Size (bytes)')
        ax.set_ylabel('QPS (msg/s)')
        ax.set_title(f'{sock}')
        ax.legend()
        ax.grid(True, alpha=0.3)
        ax.xaxis.set_major_formatter(mticker.FuncFormatter(
            lambda x, _: f'{int(x)}B' if x < 1024 else f'{int(x)//1024}K'))

    plt.tight_layout()
    p = os.path.join(outdir, 'qps_vs_msgsize.png')
    plt.savefig(p, dpi=150)
    plt.close()
    print(f"  Saved: {p}")

    # ── fig 2: Throughput vs Message Size ─────────────────────
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))
    fig.suptitle('UDS Benchmark — Throughput vs Message Size', fontsize=14, fontweight='bold')

    for ax, sock in zip(axes, ['STREAM', 'DGRAM']):
        data = defaultdict(lambda: defaultdict(list))
        for r in rows:
            if r['sock_mode'] == sock:
                data[r['io_mode']][r['msg_size']].append(r['tput_mb'])

        for io_mode, szdict in sorted(data.items()):
            sizes = sorted(szdict.keys())
            tput  = [max(szdict[s]) for s in sizes]
            ax.plot(sizes, tput,
                    color=COLORS.get(io_mode, '#888'),
                    marker=MARKERS.get(io_mode, 'o'),
                    label=io_mode, linewidth=2, markersize=7)

        ax.set_xscale('log', base=2)
        ax.set_xlabel('Message Size (bytes)')
        ax.set_ylabel('Throughput (MB/s)')
        ax.set_title(f'{sock}')
        ax.legend()
        ax.grid(True, alpha=0.3)
        ax.xaxis.set_major_formatter(mticker.FuncFormatter(
            lambda x, _: f'{int(x)}B' if x < 1024 else f'{int(x)//1024}K'))

    plt.tight_layout()
    p = os.path.join(outdir, 'tput_vs_msgsize.png')
    plt.savefig(p, dpi=150)
    plt.close()
    print(f"  Saved: {p}")

    # ── fig 3: Latency percentile bar chart ───────────────────
    # Group by io_mode, pick msg_size=1024 STREAM
    target_size = 1024
    lat_rows = [r for r in rows if r['sock_mode'] == 'STREAM'
                and r['msg_size'] == target_size and r['sockets'] == 1]

    if lat_rows:
        io_modes = sorted(set(r['io_mode'] for r in lat_rows))
        x = np.arange(len(io_modes))
        width = 0.2

        fig, ax = plt.subplots(figsize=(10, 6))
        pcts = [('avg', 'avg_us'), ('p50', 'p50_us'), ('p99', 'p99_us'), ('p99.9', 'p999_us')]
        colors = ['#2196F3', '#4CAF50', '#FF9800', '#F44336']

        for i, (name, col) in enumerate(pcts):
            vals = []
            for io in io_modes:
                candidates = [r[col] for r in lat_rows if r['io_mode'] == io]
                vals.append(min(candidates) if candidates else 0)
            bars = ax.bar(x + i*width, vals, width, label=name, color=colors[i])
            ax.bar_label(bars, fmt='%.1f', padding=2, fontsize=8)

        ax.set_xlabel('IO Mode')
        ax.set_ylabel('Latency (µs)')
        ax.set_title(f'RTT Latency Percentiles — STREAM, msg={target_size}B, S=1')
        ax.set_xticks(x + width * 1.5)
        ax.set_xticklabels(io_modes)
        ax.legend()
        ax.grid(True, axis='y', alpha=0.3)

        plt.tight_layout()
        p = os.path.join(outdir, 'latency_percentiles.png')
        plt.savefig(p, dpi=150)
        plt.close()
        print(f"  Saved: {p}")

    # ── fig 4: Socket count scalability ───────────────────────
    scale_data = defaultdict(lambda: defaultdict(list))
    for r in rows:
        if r['sock_mode'] == 'STREAM' and r['msg_size'] == 1024:
            scale_data[r['io_mode']][r['sockets']].append(r['qps'])

    if scale_data:
        fig, ax = plt.subplots(figsize=(9, 6))
        for io_mode, sdict in sorted(scale_data.items()):
            xs = sorted(sdict.keys())
            ys = [max(sdict[s]) for s in xs]
            ax.plot(xs, ys,
                    color=COLORS.get(io_mode, '#888'),
                    marker=MARKERS.get(io_mode, 'o'),
                    label=io_mode, linewidth=2, markersize=8)

        ax.set_xlabel('Number of Sockets')
        ax.set_ylabel('QPS (msg/s)')
        ax.set_title('QPS Scalability vs Socket Count — STREAM msg=1024B')
        ax.legend()
        ax.grid(True, alpha=0.3)
        plt.tight_layout()
        p = os.path.join(outdir, 'scalability_sockets.png')
        plt.savefig(p, dpi=150)
        plt.close()
        print(f"  Saved: {p}")

    print(f"\n✓ All charts saved to: {outdir}")

def main():
    parser = argparse.ArgumentParser(description='Plot UDS benchmark results')
    parser.add_argument('csv_files', nargs='+', help='CSV result file(s)')
    parser.add_argument('--output', '-o', default='results/charts',
                        help='Output directory for charts (default: results/charts)')
    args = parser.parse_args()

    all_rows = []
    for path in args.csv_files:
        rows = parse_csv(path)
        print(f"Loaded {len(rows)} rows from {path}")
        all_rows.extend(rows)

    if not all_rows:
        print("No data found in CSV files.")
        return 1

    make_charts(all_rows, args.output)
    return 0

if __name__ == '__main__':
    sys.exit(main())
