#pragma once

#include <string>

namespace bursts {

// Simulation configuration.
// Field names match SimulationConfig in cam_stream_sim.py.
struct SimulationConfig {
  // Problem size
  int n_cameras = 5000;
  int max_burst_cameras = 200;
  double duration_seconds = 300.0;
  double dt = 0.01;  // 10 ms time step == 100 Hz

  // Traffic / burst model (Markov-modulated)
  double baseline_bitrate = 3'000'000.0;  // 3 Mbps per stream (H.265 1080p30)
  double burst_factor_min = 3.0;
  double burst_factor_max = 10.0;
  double burst_arrival_rate = 0.001;   // per stream per second (lambda_on)
  double burst_recovery_rate = 0.1;    // per second (lambda_off)

  // Buffering
  double per_stream_buffer_s = 2.0;    // seconds of baseline bitrate per stream
  double emergency_pool_gb = 64.0;
  double nic_bandwidth_gbps = 25.0;

  // PI controller
  double pi_kp = 0.5;
  double pi_ki = 0.1;
  double pi_hz = 100.0;
  double controller_setpoint = 0.4;    // 40% target occupancy

  // Reproducibility / recording
  int seed = 42;
  bool verbose = false;
  bool record_all_streams = false;     // default: record 5 sample streams only
  std::string output_dir = "output";
  bool write_metrics = true;
  bool write_csv = true;
};

}  // namespace bursts