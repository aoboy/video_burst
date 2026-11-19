#!/usr/bin/env python3
"""test_cpp_sim.py -- Python test suite for the C++ simulator binary.

These tests build the simulator with CMake (if necessary), execute it with a
number of configurations, and validate the emitted artifacts (metrics files,
time-series CSV) as well as CLI behavior.

Run:
    python3 python/test_cpp_sim.py            # or `make -f makefile.sim pytests`
"""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build"
SIM_BIN = BUILD_DIR / "bsim"
UNIT_BIN = BUILD_DIR / "bsim_tests"

# Keys that must be present in simulation_metrics.txt (python-parity contract).
REQUIRED_KEYS = [
    "duration_seconds", "n_cameras", "nic_bandwidth_gbps",
    "mean_ingress_gbps", "max_ingress_gbps", "mean_egress_gbps",
    "max_concurrent_bursts", "mean_concurrent_bursts",
    "max_total_buffer_gb", "mean_total_buffer_gb",
    "max_emergency_pool_gb", "mean_emergency_pool_gb",
    "mean_continuity_ratio", "min_continuity_ratio",
    "mean_adaptation_level", "max_adaptation_level",
    "total_dropped_bits", "total_dropped_gb", "drop_ratio",
    "streams_with_drops", "max_frames_dropped_per_stream",
    "mean_frames_dropped_per_stream",
]

INT_KEYS = ["n_cameras", "streams_with_drops", "max_frames_dropped_per_stream"]
FLOAT_KEYS = [k for k in REQUIRED_KEYS if k not in INT_KEYS and k != "max_concurrent_bursts"]

METRICS_HEADER = "Camera Stream Burst Simulation - Metrics"
CSV_HEADER = ("t,ingress_gbps,egress_gbps,buffer_gb,emergency_gb,"
              "bursts,adaptations,dropped_frames,continuity")


def ensure_built() -> bool:
    """Configure and build with CMake if the binary is missing."""
    if SIM_BIN.exists():
        return True
    print(f"[test_cpp_sim] building simulator ({SIM_BIN}) ...")
    r = subprocess.run(["cmake", "-S", str(ROOT), "-B", str(BUILD_DIR),
                        "-DCMAKE_BUILD_TYPE=Release"], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        return False
    r = subprocess.run(["cmake", "--build", str(BUILD_DIR), "--parallel"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        return False
    return SIM_BIN.exists() and UNIT_BIN.exists()


def run_sim(args, outdir):
    cmd = [str(SIM_BIN), *args, "--outdir", str(outdir)]
    return subprocess.run(cmd, capture_output=True, text=True)


def parse_metrics(path: Path) -> dict:
    metrics = {}
    with open(path) as f:
        lines = [ln.strip() for ln in f if ":" in ln]
    for ln in lines:
        key, _, value = ln.partition(":")
        metrics[key.strip()] = value.strip()
    return metrics


@unittest.skipUnless(ensure_built(), "simulator did not build")
class TestCoreLibrary(unittest.TestCase):
    def test_cpp_unit_tests_pass(self):
        r = subprocess.run([str(UNIT_BIN)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("0 failure(s)", r.stdout)


@unittest.skipUnless(ensure_built(), "simulator did not build")
class TestCliBehavior(unittest.TestCase):
    def test_default_5000_streams_runs(self):
        with tempfile.TemporaryDirectory() as td:
            r = run_sim(["--cameras", "5000", "--duration", "20"], td)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue((Path(td) / "simulation_metrics.txt").exists())

    def test_unknown_flag_fails(self):
        with tempfile.TemporaryDirectory() as td:
            r = run_sim(["--bogus-flag"], td)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("unknown option", r.stderr)

    def test_zero_cameras_fails(self):
        with tempfile.TemporaryDirectory() as td:
            r = run_sim(["--cameras", "0"], td)
            self.assertNotEqual(r.returncode, 0)

    def test_negative_duration_fails(self):
        with tempfile.TemporaryDirectory() as td:
            r = run_sim(["--duration", "-5"], td)
            self.assertNotEqual(r.returncode, 0)

    def test_help_exits_zero(self):
        r = subprocess.run([str(SIM_BIN), "--help"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0)
        self.assertIn("Usage: bsim", r.stdout)


@unittest.skipUnless(ensure_built(), "simulator did not build")
class TestMetricsOutput(unittest.TestCase):
    def test_metrics_file_has_all_keys(self):
        with tempfile.TemporaryDirectory() as td:
            r = run_sim(["--cameras", "200", "--duration", "10"], td)
            self.assertEqual(r.returncode, 0, r.stderr)
            metrics = parse_metrics(Path(td) / "simulation_metrics.txt")
            for key in REQUIRED_KEYS:
                self.assertIn(key, metrics, f"missing metric key: {key}")
            n = float(metrics["n_cameras"])
            self.assertEqual(n, 200.0)
            self.assertAlmostEqual(float(metrics["duration_seconds"]), 10.0, places=3)

    def test_json_metrics_valid(self):
        import json
        with tempfile.TemporaryDirectory() as td:
            r = run_sim(["--cameras", "50", "--duration", "5"], td)
            self.assertEqual(r.returncode, 0, r.stderr)
            data = json.loads((Path(td) / "simulation_metrics.json").read_text())
            self.assertEqual(data["n_cameras"], 50)
            self.assertTrue(0 < data["mean_ingress_gbps"] < 100)

    def test_csv_timeseries_shape(self):
        import csv
        with tempfile.TemporaryDirectory() as td:
            r = run_sim(["--cameras", "50", "--duration", "5"], td)
            self.assertEqual(r.returncode, 0, r.stderr)
            with open(Path(td) / "timeseries.csv") as f:
                rows = list(csv.DictReader(f))
            expected = int(5.0 / 0.01)
            self.assertLessEqual(abs(len(rows) - expected), 1)
            self.assertTrue(all(0.0 <= float(x["continuity"]) <= 1.0 for x in rows))

    def test_positive_ingress_egress(self):
        with tempfile.TemporaryDirectory() as td:
            r = run_sim(["--cameras", "100", "--duration", "5"], td)
            self.assertEqual(r.returncode, 0, r.stderr)
            metrics = parse_metrics(Path(td) / "simulation_metrics.txt")
            self.assertGreater(float(metrics["mean_ingress_gbps"]), 0.0)
            self.assertGreater(float(metrics["mean_egress_gbps"]), 0.0)

    def test_no_metrics_flag(self):
        with tempfile.TemporaryDirectory() as td:
            r = run_sim(["--cameras", "20", "--duration", "2", "--no-metrics",
                         "--no-csv"], td)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertFalse((Path(td) / "simulation_metrics.txt").exists())
            self.assertFalse((Path(td) / "timeseries.csv").exists())


@unittest.skipUnless(ensure_built(), "simulator did not build")
class TestDeterminism(unittest.TestCase):
    def test_same_seed_identical_output(self):
        with tempfile.TemporaryDirectory() as td1, tempfile.TemporaryDirectory() as td2:
            r1 = run_sim(["--cameras", "200", "--duration", "8", "--seed", "42"], td1)
            r2 = run_sim(["--cameras", "200", "--duration", "8", "--seed", "42"], td2)
            self.assertEqual(r1.returncode, 0, r1.stderr)
            self.assertEqual(r2.returncode, 0, r2.stderr)
            m1 = (Path(td1) / "simulation_metrics.txt").read_bytes()
            m2 = (Path(td2) / "simulation_metrics.txt").read_bytes()
            self.assertEqual(m1, m2)


@unittest.skipUnless(ensure_built(), "simulator did not build")
class TestQos(unittest.TestCase):
    def test_default_config_keeps_continuity(self):
        with tempfile.TemporaryDirectory() as td:
            r = run_sim(["--cameras", "1000", "--duration", "30", "--seed", "42"],
                        td)
            self.assertEqual(r.returncode, 0, r.stderr)
            metrics = parse_metrics(Path(td) / "simulation_metrics.txt")
            self.assertAlmostEqual(float(metrics["mean_continuity_ratio"]), 1.0,
                                   places=6)
            self.assertAlmostEqual(float(metrics["drop_ratio"]), 0.0, places=6)


if __name__ == "__main__":
    unittest.main(verbosity=2)