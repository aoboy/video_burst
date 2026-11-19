#pragma once

#include "burst/adaptation_engine.h"
#include "burst/burst_detector.h"
#include "burst/config.h"
#include "burst/pi_controller.h"
#include "burst/rng.h"
#include "burst/stream.h"

#include <string>
#include <vector>

namespace bursts {

// Result metrics of a full simulation run. Field names mirror the metrics
// dictionary of cam_stream_sim.py so outputs are directly comparable.
struct SimMetrics {
  double duration_seconds = 0.0;
  int n_cameras = 0;
  double nic_bandwidth_gbps = 0.0;

  // Traffic
  double mean_ingress_gbps = 0.0;
  double max_ingress_gbps = 0.0;
  double mean_egress_gbps = 0.0;

  // Bursts
  double max_concurrent_bursts = 0.0;
  double mean_concurrent_bursts = 0.0;

  // Buffers
  double max_total_buffer_gb = 0.0;
  double mean_total_buffer_gb = 0.0;
  double max_emergency_pool_gb = 0.0;
  double mean_emergency_pool_gb = 0.0;

  // Quality of service
  double mean_continuity_ratio = 0.0;
  double min_continuity_ratio = 0.0;
  double mean_adaptation_level = 0.0;
  double max_adaptation_level = 0.0;

  // Drops
  double total_dropped_bits = 0.0;
  double total_dropped_gb = 0.0;
  double drop_ratio = 0.0;
  int streams_with_drops = 0;
  int max_frames_dropped_per_stream = 0;
  double mean_frames_dropped_per_stream = 0.0;
};

// Discrete-event fixed-timestep simulation of the burst-resilient camera
// stream processing system. Mirrors CameraStreamSimulator in cam_stream_sim.py.
class CameraStreamSimulator {
 public:
  explicit CameraStreamSimulator(const SimulationConfig& cfg);

  // Execute one simulation timestep. Exposed for unit tests.
  void Step();

  // Run the full simulation and compute metrics.
  SimMetrics Run();

  void PrintMetrics(const SimMetrics& m) const;

  // Writes simulation_metrics.txt, simulation_metrics.json, timeseries.csv and
  // stream_samples.csv into cfg.output_dir (created if needed).
  void WriteReports(const SimMetrics& m) const;

  const SimulationConfig& config() const { return cfg_; }
  const SimMetrics& metrics() const { return metrics_; }

  // --- Exposed helpers (documented for direct unit testing) ---

  void UpdateBurstStates();
  double ComputeInstantaneousBitrate(const StreamState& s);
  double ApplyAdaptationToBitrate(const StreamState& s, double raw) const;
  // Ingests bits into the per-stream buffer + emergency pool; excess is dropped.
  // Returns bits actually accepted.
  double Ingest(StreamState& s, double bits);
  // Drains the buffer at the given rate (bits/s), main buffer before emergency.
  void DrainBuffers(StreamState& s, double drain_rate);
  void RunController(StreamState& s);

 private:
  void RecordPerStreamHistory();

  SimulationConfig cfg_;
  Rng rng_;

  // System capacity
  double nic_bandwidth_ = 0.0;
  double per_stream_buffer_ = 0.0;
  double emergency_pool_total_ = 0.0;
  double emergency_pool_used_ = 0.0;

  // Subsystems
  BurstDetector burst_detector_;
  PIController pi_controller_;
  AdaptationEngine adaptation_engine_;

  // Simulation & streams
  std::vector<StreamState> streams_;
  double current_time_ = 0.0;
  double total_bits_received_ = 0.0;
  double total_bits_dropped_ = 0.0;
  double total_bits_served_ = 0.0;

  // Global history
  std::vector<double> time_hist_;
  std::vector<double> ingress_hist_;      // bits per step
  std::vector<double> egress_hist_;       // attempted bits per step
  std::vector<double> buffer_hist_;       // bits
  std::vector<double> emergency_hist_;    // bits
  std::vector<int> burst_count_hist_;
  std::vector<int> adapt_count_hist_;
  std::vector<int> dropped_frames_hist_;
  std::vector<double> continuity_hist_;

  // Per-stream history (sampled like the python simulator, every ~100 ms).
  bool record_all_ = false;
  std::vector<int> recorded_streams_;     // stream ids with recorded history
  std::vector<std::vector<double>> stream_occupancy_hist_;
  std::vector<std::vector<double>> stream_bitrate_hist_;
  std::vector<std::vector<int>> stream_adaptation_hist_;

  SimMetrics metrics_;
};

}  // namespace bursts