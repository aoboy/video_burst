#!/usr/bin/env python3
"""compare_with_python.py -- Cross-validate the C++ simulator against the
python reference implementation (cam_stream_sim.py).

Both implement the same burst-absorption + adaptation algorithm, but they use
different RNGs (numpy.RandomState vs std::mt19937_64), so the comparison is on
aggregate statistics with tolerances rather than bit-for-bit equality.

Usage:
    python3 python/compare_with_python.py
    python3 python/compare_with_python.py --cameras 2000 --duration 120 --plot
"""

import argparse
import os
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # sim_cpp/
REPO = ROOT.parent                                      # kimi/ (holds cam_stream_sim.py)
PYTHON_SIM = REPO / "cam_stream_sim.py"
SIM_BIN = ROOT / "build" / "bsim"
METRICS_FILE = REPO / "output" / "simulation_metrics.txt"

# metric key -> (kind, tolerance) ; kind: "rel" (relative) or "abs" (absolute)
# Tuned from runs over several seeds (loose enough to absorb RNG differences,
# tight enough to catch algorithm drift).
DEFAULT_TOLERANCES = {
    "mean_ingress_gbps": ("rel", 0.05),
    "max_ingress_gbps": ("rel", 0.20),
    "mean_egress_gbps": ("rel", 0.05),
    "mean_concurrent_bursts": ("rel", 0.40),
    "max_total_buffer_gb": ("rel", 0.10),
    "mean_total_buffer_gb": ("rel", 0.20),
    "max_emergency_pool_gb": ("rel", 0.60),
    "mean_emergency_pool_gb": ("rel", 0.60),
    "mean_continuity_ratio": ("abs", 0.01),
    "min_continuity_ratio": ("abs", 0.01),
    "drop_ratio": ("abs", 0.01),
}


def build_if_needed() -> bool:
    if SIM_BIN.exists():
        return True
    r = subprocess.run(["cmake", "-S", str(ROOT), "-B", str(ROOT / "build"),
                        "-DCMAKE_BUILD_TYPE=Release"])
    if r.returncode != 0:
        return False
    return subprocess.run(["cmake", "--build", str(ROOT / "build"), "--parallel"]).returncode == 0


def run_python_sim(cfg_kwargs: dict) -> dict:
    sys.path.insert(0, str(REPO))
    import cam_stream_sim
    cfg = cam_stream_sim.SimulationConfig(**cfg_kwargs)
    sim = cam_stream_sim.CameraStreamSimulator(cfg)
    metrics = sim.run()
    return metrics


def run_cpp_sim(cfg_kwargs: dict) -> dict:
    with tempfile.TemporaryDirectory() as td:
        args = [str(SIM_BIN), "--outdir", td,
                "--cameras", str(cfg_kwargs["n_cameras"]),
                "--duration", str(cfg_kwargs["duration_seconds"]),
                "--nic", str(cfg_kwargs["nic_bandwidth_gbps"]),
                "--seed", str(cfg_kwargs["seed"])]
        r = subprocess.run(args, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"C++ simulator failed: {r.stderr}")
        return parse_metrics(Path(td) / "simulation_metrics.txt")


def parse_metrics(path: Path) -> dict:
    out = {}
    for line in path.read_text().splitlines():
        if ":" in line:
            key, _, val = line.partition(":")
            out[key.strip()] = float(val.strip())
    return out


def rel_diff(a: float, b: float) -> float:
    denom = max(abs(a), abs(b), 1e-12)
    return abs(a - b) / denom


def check(metric, pm, cm):
    kind, tol = DEFAULT_TOLERANCES[metric]
    p, c = pm[metric], cm[metric]
    if kind == "rel":
        diff = rel_diff(p, c)
        ok = diff <= tol
    else:
        diff = abs(p - c)
        ok = diff <= tol
    return ok, diff, tol, kind


def main():
    ap = argparse.ArgumentParser(description="Cross-validate C++ vs python sim")
    ap.add_argument("--cameras", "-n", type=int, default=1000)
    ap.add_argument("--duration", "-d", type=float, default=120.0)
    ap.add_argument("--nic", type=float, default=25.0)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--plot", action="store_true",
                    help="render a comparison figure (requires matplotlib)")
    ap.add_argument("--out", type=str, default=None,
                    help="output path for the optional comparison plot")
    args = ap.parse_args()

    if not build_if_needed():
        sys.exit("failed to build the C++ simulator")

    cfg_kwargs = dict(n_cameras=args.cameras, duration_seconds=args.duration,
                      nic_bandwidth_gbps=args.nic, seed=args.seed)

    # Preserve the python reference's metrics file (the reference rewrites it
    # in place); restore afterwards.
    backup = METRICS_FILE.read_bytes() if METRICS_FILE.exists() else None

    try:
        print(f"Running python reference (N={args.cameras}, T={args.duration}s)...")
        pm = run_python_sim(cfg_kwargs)
        print("Running C++ simulator with identical parameters...")
        cm = run_cpp_sim(cfg_kwargs)
    finally:
        if backup is not None:
            METRICS_FILE.write_bytes(backup)

    print("\n" + "=" * 64)
    print(f"Cross-validation  (seed={args.seed}, N={args.cameras}, "
          f"T={args.duration}s)")
    print("=" * 64)
    print(f"{'metric':<28}{'python':>12}{'cpp':>12}{'diff':>10}  verdict")
    print("-" * 64)

    failures = 0
    for metric, (k, t) in DEFAULT_TOLERANCES.items():
        ok, diff, tol, kind = check(metric, pm, cm)
        failures += 0 if ok else 1
        verdict = "ok" if ok else "MISMATCH"
        print(f"{metric:<28}{pm[metric]:>12.4f}{cm[metric]:>12.4f}"
              f"{diff:>10.4f}  {verdict}  (tol {kind} {tol})")

    print("-" * 64)
    p_cont, c_cont = pm["mean_continuity_ratio"], cm["mean_continuity_ratio"]
    p_drop, c_drop = pm["drop_ratio"], cm["drop_ratio"]
    print(f"python mean continuity {p_cont:.4f} | drop {p_drop:.6f}")
    print(f"cpp   mean continuity {c_cont:.4f} | drop {c_drop:.6f}")

    if failures:
        print(f"\n{failures} metric(s) exceeded tolerance")
        return 1

    print("\nAll compared metrics within tolerance.")
    if args.plot:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
        except ImportError:
            print("matplotlib not installed; skipping plot")
            return 0
        fig, axes = plt.subplots(2, 1, figsize=(10, 7))
        ax = axes[0]
        # Re-run python to capture histories for plotting.
        sys.path.insert(0, str(REPO))
        import cam_stream_sim
        sim = cam_stream_sim.CameraStreamSimulator(
            cam_stream_sim.SimulationConfig(**cfg_kwargs))
        sim.run()
        t = sim.time_history
        ax.plot(t, [v / sim.cfg.dt / 1e9 for v in sim.total_ingress_history],
                color="red", label="python ingress", lw=1)
        ax.plot(t, [v / sim.cfg.dt / 1e9 for v in sim.total_egress_history],
                color="green", label="python egress", lw=1)
        # C++ timeseries
        import csv
        cpp_rows = None
        with tempfile.TemporaryDirectory() as td:
            subprocess.run([str(SIM_BIN), "--outdir", td,
                            "--cameras", str(args.cameras),
                            "--duration", str(args.duration),
                            "--nic", str(args.nic), "--seed", str(args.seed)],
                           capture_output=True)
            with open(Path(td) / "timeseries.csv") as f:
                cpp_rows = list(csv.DictReader(f))
            ct = [float(x["t"]) for x in cpp_rows]
            ax.plot(ct, [float(x["ingress_gbps"]) for x in cpp_rows],
                    color="red", ls="--", label="cpp ingress", lw=1)
            ax.plot(ct, [float(x["egress_gbps"]) for x in cpp_rows],
                    color="green", ls="--", label="cpp egress", lw=1)
        ax.axhline(args.nic, color="blue", ls="--", lw=1,
                   label=f"NIC ({args.nic} Gbps)")
        ax.set_ylabel("Gbps")
        ax.set_title("Aggregate bandwidth (python vs cpp)")
        ax.grid(True, alpha=0.3)
        ax.legend(loc="upper right", fontsize=8)

        ax2 = axes[1]
        tt = [float(x["t"]) for x in cpp_rows]
        ax2.plot(tt, [float(x["emergency_gb"]) for x in cpp_rows], color="orange",
                 label="cpp emergency pool", lw=1)
        ax2.plot(t, [v / 8e9 for v in sim.emergency_pool_history], color="purple",
                 label="python emergency pool", lw=1)
        ax2.set_ylabel("GB")
        ax2.set_xlabel("Time (s)")
        ax2.set_title("Emergency pool usage (python vs cpp)")
        ax2.grid(True, alpha=0.3)
        ax2.legend(loc="upper right", fontsize=8)

        plt.tight_layout()
        out = args.out or str(ROOT / "output" / "compare_cpp_python.png")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        plt.savefig(out, dpi=130)
        print(f"Comparison plot saved to: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())