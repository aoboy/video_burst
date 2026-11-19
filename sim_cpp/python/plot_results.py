#!/usr/bin/env python3
"""plot_results.py -- render plots from the C++ simulator's CSV artefacts.

Reads <outdir>/timeseries.csv and <outdir>/stream_samples.csv (written by the
bsim binary) and produces a figure mirroring the plots of cam_stream_sim.py.

Usage:
    python3 python/plot_results.py [--outdir output] [--out output/results.png]
"""

import argparse
import csv
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser(description="Plot C++ simulation results")
    ap.add_argument("--outdir", default=str(ROOT / "output"),
                    help="directory containing timeseries.csv")
    ap.add_argument("--out", default=None, help="output PNG path")
    args = ap.parse_args()

    csv_path = Path(args.outdir) / "timeseries.csv"
    samples_path = Path(args.outdir) / "stream_samples.csv"
    if not csv_path.exists():
        sys.exit(f"no timeseries.csv in {args.outdir}; run the simulator first")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("matplotlib is required for plotting")

    with open(csv_path) as f:
        rows = list(csv.DictReader(f))
    t = [float(r["t"]) for r in rows]

    fig, axes = plt.subplots(3, 2, figsize=(16, 13))
    fig.suptitle("Camera Stream Burst Simulation (C++)", fontsize=14,
                 fontweight="bold")

    # 1/2. Aggregate bandwidth
    ax = axes[0, 0]
    ax.plot(t, [float(r["ingress_gbps"]) for r in rows], label="Ingress",
            color="red", alpha=0.8, linewidth=1)
    ax.plot(t, [float(r["egress_gbps"]) for r in rows], label="Egress",
            color="green", alpha=0.8, linewidth=1)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Bandwidth (Gbps)")
    ax.set_title("Aggregate Bandwidth")
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(True, alpha=0.3)

    # 2. Buffer occupancy
    ax = axes[0, 1]
    ax.plot(t, [float(r["buffer_gb"]) for r in rows], color="purple", linewidth=1)
    ax.plot(t, [float(r["emergency_gb"]) for r in rows], color="orange",
            label="Emergency pool", linewidth=1)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Buffer (GB)")
    ax.set_title("Buffer Occupancy")
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(True, alpha=0.3)

    # 3. Burst count
    ax = axes[1, 0]
    ax.fill_between(t, 0, [int(r["bursts"]) for r in rows], alpha=0.5, color="red")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Concurrent Bursts")
    ax.set_title("Active Burst Streams")
    ax.grid(True, alpha=0.3)

    # 4. Continuity
    ax = axes[1, 1]
    ax.plot(t, [float(r["continuity"]) * 100 for r in rows], color="green",
            linewidth=1.5)
    ax.axhline(100, color="black", linestyle="--", alpha=0.5)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Continuity (%)")
    ax.set_title("Client Feed Continuity Ratio")
    ax.set_ylim([0, 105])
    ax.grid(True, alpha=0.3)

    # 5/6. Per-stream samples
    streams = {}
    if samples_path.exists():
        with open(samples_path) as f:
            for r in csv.DictReader(f):
                streams.setdefault(int(r["stream_id"]), []).append(r)
    ax = axes[2, 0]
    for sid, rows_s in streams.items():
        ts = [float(r["t"]) for r in rows_s]
        ax.plot(ts, [float(r["bitrate_mbps"]) for r in rows_s],
                label=f"Stream {sid}", alpha=0.8, linewidth=1)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Bitrate (Mbps)")
    ax.set_title("Sample Stream Bitrates")
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(True, alpha=0.3)

    ax = axes[2, 1]
    for sid, rows_s in streams.items():
        ts = [float(r["t"]) for r in rows_s]
        ax.plot(ts, [float(r["occupancy_pct"]) for r in rows_s],
                label=f"Stream {sid}", alpha=0.8, linewidth=1)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Occupancy (%)")
    ax.set_title("Sample Stream Buffer Occupancy")
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(True, alpha=0.3)

    plt.tight_layout(rect=[0, 0.03, 1, 0.95])
    out = args.out or str(Path(args.outdir) / "results.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    plt.savefig(out, dpi=150, bbox_inches="tight")
    print(f"Plot saved to: {out}")


if __name__ == "__main__":
    main()