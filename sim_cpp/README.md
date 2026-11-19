# Burst-Resilient Camera Stream Processing System — C++ Simulation

C++17 implementation of the burst-resilient camera stream processing system
described in `docs/BURST_RESILIENT_CAMERA_SYSTEM.md`, using the discrete-event
simulator `cam_stream_sim.py` as the behavioural reference.

The simulation models `N` concurrent H.265 camera streams (Markov-modulated
bursts, GOP variation, Gaussian noise), hierarchical buffering (per-stream
ring buffer + shared emergency pool), dual-EWMA burst detection, a
proportional-integral (PI) controller driving a staged codec-adaptation
engine, and a weighted drain (egress) path that keeps client continuity.

## Layout

```
sim_cpp/
├── CMakeLists.txt              # canonical build (CMake >= 3.16)
├── makefile.sim                # convenience wrapper: make -f makefile.sim <tgt>
├── include/burst/              # public headers (config, stream, subsystems)
├── src/                        # implementation + CLI (bsim)
├── tests/                      # C++ unit tests (dependency-free harness)
├── python/                     # test / validation / plotting scripts
└── output/                     # generated metrics + time series (after a run)
```

## Build

```sh
cd sim_cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
# or, equivalently:
make -f makefile.sim build
```

## Run

```sh
build/bsim --cameras 5000 --duration 300 --seed 42
build/bsim --cameras 1000 --duration 120 --nic 40 --seed 7 -v
build/bsim --model            # analytical model report (Part III formulas)
```

Outputs (in `output/` by default, override with `--outdir`):

- `simulation_metrics.txt` — same keys/format as `cam_stream_sim.py` (for diffs)
- `simulation_metrics.json` — machine-readable metrics
- `timeseries.csv` — aggregate time series (ingress, egress, buffers, bursts,
  adaptation, drops, continuity)
- `stream_samples.csv` — per-stream history for 5 sample streams
  (use `--record-all-streams` to record every stream)

Flags mirror the python CLI: `-n/--cameras -k/--burst-cameras -d/--duration
--nic --buffer-s --emergency-gb --burst-min --burst-max --burst-rate
--burst-recovery --kp --ki --pi-hz --setpoint --seed --outdir -v --model`.

## Testing

```sh
make -f makefile.sim unit        # C++ unit tests (tests/bsim_tests)
make -f makefile.sim pytests     # python black-box tests of the binary
make -f makefile.sim test        # both
ctest --test-dir build           # run the C++ suite through CTest too
python3 python/compare_with_python.py    # cross-validate vs cam_stream_sim.py
python3 python/plot_results.py           # plots from the CSVs
```

### C++ unit tests

`tests/` contains ~40 checks in a dependency-free harness
(`tests/test_framework.h`):

- `test_rng.cpp` — determinism, distribution sanity
- `test_burst_detector.cpp` — dual-EWMA burst/recovery detection
- `test_pi_controller.cpp` — proportional/integral behaviour, anti-windup clamp
- `test_adaptation_engine.cpp` — staged adaptation mapping and restore
- `test_buffer.cpp` — ingest → ring → emergency pool → drop → drain ordering
- `test_model.cpp` — analytical formulas (T_overflow, β, control-loop, energy)
- `test_simulator.cpp` — end-to-end run, determinism, forced-drop scenario

### Python tests

`python/test_cpp_sim.py` (stdlib `unittest`) builds the project if needed,
runs the binary and validates: CLI errors, metrics-file contract, JSON
validity, CSV shape, seed determinism, and QoS guarantees (continuity = 1,
no drops on the default config).

### Cross-validation

`python/compare_with_python.py` runs both simulators with identical parameters
and asserts that aggregate statistics agree within tolerances. Because the two
implementations use independent RNGs (`numpy.RandomState` vs `std::mt19937_64`),
comparison is statistical, not bit-exact.

## Notes / fidelity

- The C++ logic mirrors `cam_stream_sim.py` field-for-field, including the
  shared-emergency-pool accounting. One known quirk of the python reference is
  preserved intentionally: a stream that already owns an emergency allocation
  may, in a pathological fully-allocated pool scenario, be credited its own
  occupancy again when computing available emergency space.
- Pareto/EWMA/normal sampling use the C++ standard library, so the two
  implementations produce *distributions* with the same parameters, not the
  same streams of numbers.