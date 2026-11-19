# Burst-Resilient Camera Stream Processing

Design, modeling, and simulation of a single-server video ingestion system
that keeps client feeds continuous while a subset of camera streams
experiences traffic bursts.

## The Problem

A single physical server ingests, processes, and persists up to
**N = 5000** concurrent H.265 camera streams. At arbitrary times, a subset of
**K < N** streams bursts — their bitrate jumps by a factor **α** (typically
3×–10×) for a window **W** — pushing aggregate demand beyond the server's
sustainable capacity. Without protection, buffers overflow, frames drop, and
clients lose their live feeds.

The requirement: **zero visible interruption** for clients
(latency ≤ 2 s normal, ≤ 5 s during a burst), on one box, with no cloud
offload.

## The Approach — a Three-Tier Defense

1. **Temporal Absorption (buffering)** — per-stream ring buffers (~2 s each)
   plus a large shared emergency pool (64 GB) absorb bursts for tens of
   seconds while slower countermeasures engage.
2. **Dynamic Adaptation (control)** — a dual-EWMA burst detector flags
   streams whose fast bitrate estimate diverges from the slow baseline; a
   PI controller, running at 100 Hz against a 40% buffer-occupancy setpoint,
   commands staged codec adaptation:
   CRF increase → resolution downscale → frame-rate decimation → shedding.
3. **Intelligent Shedding (prioritization)** — client-facing streams (P0) are
   never dropped; analytics (P1) and archive (P2) data are shed first.

> Core insight: decouple ingestion from client serving. Bursts are absorbed
> in RAM while the adaptation engine throttles ingress bitrate *before*
> buffers exhaust. Clients see slightly delayed or lower-quality video,
> never an interruption.

## Repository Layout

```
├── cam_stream_sim.py        # Python reference simulator (numpy/matplotlib)
├── docs/
│   ├── BURST_RESILIENT_CAMERA_SYSTEM.md   # merged ARCHITECTURE + DESIGN + MODELING
│   └── BURST_RESILIENT_CAMERA_SYSTEM.pdf
├── sim_cpp/                # C++17 port of the simulator
│   ├── include/burst/      #   core library headers
│   ├── src/                #   library + `bsim` CLI
│   ├── tests/              #   ~40 C++ unit checks (dependency-free harness)
│   ├── python/             #   CLI black-box tests, cross-validation, plotting
│   └── output/             #   sample run artifacts
├── output/                 # sample artifacts from the Python simulator
└── kimi_web_output.txt     # original chat transcript this project was recovered from
```

## Quick Start

### Python reference simulator

```sh
# Baseline scenario: 5000 cameras, 300 s
python3 cam_stream_sim.py --cameras 5000 --burst-cameras 200 --duration 300 --plot

# Constrained-NIC stress test
python3 cam_stream_sim.py --cameras 1000 --burst-cameras 500 --nic 40 --duration 300 --plot
```

Requires Python 3 with `numpy` and `matplotlib`. Metrics land in
`output/simulation_metrics.txt`, plots in `output/cam_stream_sim.png`.

### C++ simulator

```sh
cd sim_cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

build/bsim --cameras 5000 --duration 300 --seed 42
build/bsim --model            # analytical model report (Part III formulas)
```

Outputs (in `sim_cpp/output/`, override with `--outdir`):
`simulation_metrics.txt` (same keys/format as the Python simulator),
`simulation_metrics.json`, `timeseries.csv`, `stream_samples.csv`.
CLI flags mirror the Python script (`-n -k -d --nic --buffer-s
--emergency-gb --burst-min --burst-max --burst-rate --burst-recovery
--kp --ki --seed ...`).

## Testing & Validation

```sh
cd sim_cpp
make -f makefile.sim test      # C++ unit tests + Python CLI black-box tests
make -f makefile.sim compare  # cross-validate C++ vs cam_stream_sim.py
ctest --test-dir build        # C++ suite through CTest
```

- **C++ unit tests** cover the RNG, burst detection, PI control (including
  the anti-windup clamp), staged adaptation, buffer/pool accounting, the
  analytical formulas, and end-to-end determinism.
- **Python black-box tests** validate the CLI contract, metrics files, CSV
  shape, seed determinism, and the QoS guarantee: on the default config the
  simulation must end with continuity = 1.0 and zero dropped bits.
- **Cross-validation** runs both simulators with identical parameters and
  asserts aggregate statistics agree within tolerances (the two use
  independent RNGs, so comparison is statistical, not bit-exact).

## Documentation

`docs/BURST_RESILIENT_CAMERA_SYSTEM.md` is the full technical reference,
merged from the original ARCHITECTURE / DESIGN / MODELING documents:

- **Part I — Architecture**: ingress (RSS/DPDK, H.265 demux), buffer &
  adaptation layer, processing/transcode, storage & distribution, hardware
  layout, threading model, failure modes.
- **Part II — Design**: data structures (lock-free ring buffers, emergency
  pool, NAL descriptors), algorithms (dual-EWMA detection, PI control,
  bitrate→codec mapping, WFQ scheduling), per-stream state machine, API
  contracts, configuration parameters.
- **Part III — Modeling**: MMPP burst traffic, fluid-flow buffer dynamics,
  T_overflow ≈ 213 s absorption window (K=200, α=5), PI stability criteria,
  capacity planning, QoS guarantees.

## Provenance

The project originated as a Kimi chat transcript (`kimi_web_output.txt`)
that produced an initial README, ARCHITECTURE, DESIGN, MODELING, and the
Python simulator. The transcript was recovered into this repository; the
three technical documents were consolidated into
`docs/BURST_RESILIENT_CAMERA_SYSTEM.md`, the simulator was made portable
(local `./output`, wired-up `--burst-recovery` flag), and a full C++17 port
with test suites was added.
