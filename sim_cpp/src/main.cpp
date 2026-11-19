#include "burst/config.h"
#include "burst/model.h"
#include "burst/simulator.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void PrintUsage() {
  std::printf(
      "Usage: bsim [options]\n"
      "\n"
      "Camera Stream Burst Simulation (C++)\n"
      "Discrete-event simulation of the burst-resilient camera stream\n"
      "processing system. See docs/BURST_RESILIENT_CAMERA_SYSTEM.md.\n"
      "\n"
      "Options:\n"
      "  -n, --cameras N          Number of camera streams (default: 5000)\n"
      "  -k, --burst-cameras N    Expected max concurrent bursts (default: 200)\n"
      "  -d, --duration S         Simulation duration in seconds (default: 300)\n"
      "      --nic G              NIC bandwidth in Gbps (default: 25)\n"
      "      --buffer-s S         Per-stream buffer duration in seconds (2)\n"
      "      --emergency-gb G     Emergency pool size in GB (64)\n"
      "      --burst-min F        Minimum burst factor (3)\n"
      "      --burst-max F        Maximum burst factor (10)\n"
      "      --burst-rate F       Burst arrival rate / stream / s (0.001)\n"
      "      --burst-recovery F   Burst recovery rate / s (0.1)\n"
      "      --kp F               PI proportional gain (0.5)\n"
      "      --ki F               PI integral gain (0.1)\n"
      "      --pi-hz F            Controller frequency (100)\n"
      "      --setpoint F         Target buffer occupancy (0.4)\n"
      "      --seed N             Random seed (42)\n"
      "      --outdir DIR         Output directory (output)\n"
      "      --record-all-streams Record per-stream history for all streams\n"
      "      --no-metrics         Do not write metrics files\n"
      "      --no-csv             Do not write CSV time series\n"
      "  -v, --verbose            Print per-step progress\n"
      "      --model              Print analytical model report and exit\n"
      "  -h, --help               Show this help\n"
      "\n"
      "Examples:\n"
      "  bsim --cameras 5000 --duration 300\n"
      "  bsim --cameras 1000 --duration 120 --nic 40 --seed 7 -v\n"
      "  bsim --model\n");
}

struct Args {
  bursts::SimulationConfig cfg;
  bool show_model = false;
  bool have_err = false;
};

template <typename T>
bool ParseNum(const std::string& s, T* out) {
  try {
    if constexpr (std::is_same_v<T, int> || std::is_same_v<T, unsigned int>) {
      auto v = std::stoll(s);
      *out = static_cast<T>(v);
    } else {
      *out = std::stod(s);
    }
    return true;
  } catch (...) {
    return false;
  }
}

Args ParseArgs(int argc, char** argv) {
  Args a;
  const std::vector<std::string> keys{
      "cameras", "burst-cameras", "duration", "nic",     "buffer-s",
      "emergency-gb", "burst-min", "burst-max", "burst-rate", "burst-recovery",
      "kp",     "ki",            "pi-hz",    "setpoint", "seed",
      "outdir"};

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    const bool is_flag = !arg.empty() && arg[0] == '-';

    if (arg == "-h" || arg == "--help") {
      PrintUsage();
      std::exit(0);
    }
    if (arg == "--model") {
      a.show_model = true;
      continue;
    }
    if (arg == "-v" || arg == "--verbose") {
      a.cfg.verbose = true;
      continue;
    }
    if (arg == "--record-all-streams") {
      a.cfg.record_all_streams = true;
      continue;
    }
    if (arg == "--no-metrics") {
      a.cfg.write_metrics = false;
      continue;
    }
    if (arg == "--no-csv") {
      a.cfg.write_csv = false;
      continue;
    }

    std::string val;
    std::string matched_key;
    for (const std::string& k : keys) {
      const std::string full = "--" + k;
      if (arg == full || arg.rfind(full + "=", 0) == 0) {
        val.clear();
        if (arg.rfind(full + "=", 0) == 0) {
          val = arg.substr(full.size() + 1);
        } else if (i + 1 < argc) {
          val = argv[++i];
        } else {
          break;
        }
        matched_key = k;
        break;
      }
    }
    if (!matched_key.empty()) {
      const std::string k = matched_key;
      if (k == "cameras") {
        if (!ParseNum(val, &a.cfg.n_cameras)) {
          std::fprintf(stderr, "error: invalid --cameras '%s'\n", val.c_str());
          a.have_err = true;
        }
      } else if (k == "burst-cameras") ParseNum(val, &a.cfg.max_burst_cameras);
      else if (k == "duration") ParseNum(val, &a.cfg.duration_seconds);
      else if (k == "nic") ParseNum(val, &a.cfg.nic_bandwidth_gbps);
      else if (k == "buffer-s") ParseNum(val, &a.cfg.per_stream_buffer_s);
      else if (k == "emergency-gb") ParseNum(val, &a.cfg.emergency_pool_gb);
      else if (k == "burst-min") ParseNum(val, &a.cfg.burst_factor_min);
      else if (k == "burst-max") ParseNum(val, &a.cfg.burst_factor_max);
      else if (k == "burst-rate") ParseNum(val, &a.cfg.burst_arrival_rate);
      else if (k == "burst-recovery") ParseNum(val, &a.cfg.burst_recovery_rate);
      else if (k == "kp") ParseNum(val, &a.cfg.pi_kp);
      else if (k == "ki") ParseNum(val, &a.cfg.pi_ki);
      else if (k == "pi-hz") ParseNum(val, &a.cfg.pi_hz);
      else if (k == "setpoint") ParseNum(val, &a.cfg.controller_setpoint);
      else if (k == "seed") ParseNum(val, &a.cfg.seed);
      else if (k == "outdir") a.cfg.output_dir = val;
      continue;
    }

    // Short flags: -n -k -d
    if (arg == "-n" && i + 1 < argc) {
      if (!ParseNum(std::string(argv[++i]), &a.cfg.n_cameras)) a.have_err = true;
      continue;
    }
    if (arg == "-k" && i + 1 < argc) {
      if (!ParseNum(std::string(argv[++i]), &a.cfg.max_burst_cameras)) a.have_err = true;
      continue;
    }
    if (arg == "-d" && i + 1 < argc) {
      if (!ParseNum(std::string(argv[++i]), &a.cfg.duration_seconds)) a.have_err = true;
      continue;
    }

    std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
    a.have_err = true;
  }

  if (a.cfg.n_cameras <= 0) {
    std::fprintf(stderr, "error: --cameras must be positive\n");
    a.have_err = true;
  }
  if (a.cfg.duration_seconds <= 0.0) {
    std::fprintf(stderr, "error: --duration must be positive\n");
    a.have_err = true;
  }
  if (a.cfg.pi_hz <= 0.0) {
    std::fprintf(stderr, "error: --pi-hz must be positive\n");
    a.have_err = true;
  }
  if (a.cfg.burst_factor_min > a.cfg.burst_factor_max) {
    std::fprintf(stderr, "error: --burst-min must be <= --burst-max\n");
    a.have_err = true;
  }
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  const Args a = ParseArgs(argc, argv);
  if (a.have_err) {
    PrintUsage();
    return 2;
  }

  if (a.show_model) {
    const bursts::model::ModelReport r =
        bursts::model::Evaluate(a.cfg);
    bursts::model::PrintModel(r);
    return 0;
  }

  bursts::CameraStreamSimulator sim(a.cfg);
  const bursts::SimMetrics m = sim.Run();
  sim.PrintMetrics(m);
  if (a.cfg.write_metrics || a.cfg.write_csv) {
    sim.WriteReports(m);
  }
  return 0;
}