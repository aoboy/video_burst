#include "burst/simulator.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <string>

#include "burst/constants.h"

namespace bursts {

namespace {

double mean(const std::vector<double>& v) {
  return v.empty() ? 0.0
                   : std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}
double mean_int(const std::vector<int>& v) {
  return v.empty() ? 0.0
                   : static_cast<double>(
                         std::accumulate(v.begin(), v.end(), 0LL)) /
                         v.size();
}
int max_int(const std::vector<int>& v) {
  return v.empty() ? 0 : *std::max_element(v.begin(), v.end());
}
double max_d(const std::vector<double>& v) {
  return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end());
}
void ensure_dir(const std::string& path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
}

}  // namespace

CameraStreamSimulator::CameraStreamSimulator(const SimulationConfig& cfg)
    : cfg_(cfg),
      rng_(static_cast<uint32_t>(cfg.seed)),
      burst_detector_(),
      pi_controller_(cfg.pi_kp, cfg.pi_ki, cfg.controller_setpoint, cfg.dt) {
  nic_bandwidth_ = cfg.nic_bandwidth_gbps * 1e9;
  per_stream_buffer_ = cfg.baseline_bitrate * cfg.per_stream_buffer_s;
  emergency_pool_total_ = cfg.emergency_pool_gb * 8e9;
  emergency_pool_used_ = 0.0;

  streams_.reserve(static_cast<size_t>(cfg.n_cameras));
  for (int i = 0; i < cfg.n_cameras; ++i) {
    StreamState s;
    s.stream_id = i;
    s.baseline_bitrate = cfg.baseline_bitrate;
    s.current_bitrate = cfg.baseline_bitrate;
    s.buffer_capacity = per_stream_buffer_;
    s.buffer_occupancy = per_stream_buffer_ * 0.2;  // start at 20% like python
    s.ewma_slow = cfg.baseline_bitrate;
    s.ewma_fast = cfg.baseline_bitrate;
    streams_.push_back(s);
  }

  // Choose which streams get per-stream history recording.
  record_all_ = cfg.record_all_streams;
  std::vector<int> candidate;
  if (record_all_) {
    for (int i = 0; i < cfg.n_cameras; ++i) candidate.push_back(i);
  } else {
    const int n = cfg.n_cameras;
    const int sample[5] = {0, n / 4, n / 2, 3 * n / 4, n - 1};
    for (int idx : sample) {
      if (idx >= 0 && idx < n) candidate.push_back(idx);
    }
  }
  for (int idx : candidate) {
    if (std::find(recorded_streams_.begin(), recorded_streams_.end(), idx) ==
        recorded_streams_.end()) {
      recorded_streams_.push_back(idx);
    }
  }
  if (!recorded_streams_.empty()) {
    stream_occupancy_hist_.resize(recorded_streams_.size());
    stream_bitrate_hist_.resize(recorded_streams_.size());
    stream_adaptation_hist_.resize(recorded_streams_.size());
  }
}

void CameraStreamSimulator::UpdateBurstStates() {
  for (StreamState& s : streams_) {
    if (!s.is_bursting) {
      const double p_burst =
          1.0 - std::exp(-cfg_.burst_arrival_rate * cfg_.dt);
      if (rng_.Uniform() < p_burst) {
        s.is_bursting = true;
        s.burst_start_time = current_time_;
        s.burst_duration = 2.0 + rng_.Pareto(2.0) * 2.0;
        s.burst_factor = rng_.Uniform(cfg_.burst_factor_min, cfg_.burst_factor_max);
      }
    } else {
      if (current_time_ - s.burst_start_time >= s.burst_duration) {
        s.is_bursting = false;
        s.burst_factor = 1.0;
        s.burst_start_time = -1.0;
      }
    }
  }
}

double CameraStreamSimulator::ComputeInstantaneousBitrate(
    const StreamState& s) {
  const double base = s.baseline_bitrate * s.burst_factor;
  const double gop_variation =
      kGopVariationAmplitude * base *
      std::sin(2.0 * M_PI * current_time_ / kGopPeriodSeconds);
  const double noise = rng_.Normal(0.0, kNoiseFraction * base);
  double bitrate = base + gop_variation + noise;
  return bitrate < kMinBitrateBps ? kMinBitrateBps : bitrate;
}

double CameraStreamSimulator::ApplyAdaptationToBitrate(const StreamState& s,
                                                       double raw) const {
  double effective = raw;
  if (s.crf_offset > 0.0) {
    const double factor = 1.0 - (s.crf_offset / 6.0) * 0.2;
    effective *= std::max(0.3, factor);
  }
  if (s.resolution_scale == kResolutionScaleHalf) {
    effective *= 0.5;
  } else if (s.resolution_scale == kResolutionScaleQuarter) {
    effective *= 0.25;
  }
  if (s.drop_b_frames && !s.drop_p_frames) {
    effective *= 0.8;
  } else if (s.drop_p_frames) {
    effective *= 0.4;
  }
  return effective;
}

double CameraStreamSimulator::Ingest(StreamState& s, double bits) {
  s.frames_received += 1;
  s.bytes_received += static_cast<unsigned long long>(bits / 8.0);

  const double available = s.buffer_capacity - s.buffer_occupancy;
  if (bits <= available) {
    s.buffer_occupancy += bits;
    return bits;
  }

  // Fill what we can in the per-stream buffer.
  double accepted = available < 0.0 ? 0.0 : available;
  s.buffer_occupancy = s.buffer_capacity;
  double remaining = bits - accepted;

  // Try the shared emergency pool.
  if (remaining > 0.0 && !s.emergency_allocated) {
    if (emergency_pool_used_ + remaining <= emergency_pool_total_) {
      s.emergency_allocated = true;
      s.emergency_occupancy = remaining;
      emergency_pool_used_ += remaining;
      accepted += remaining;
      remaining = 0.0;
    } else if (emergency_pool_used_ < emergency_pool_total_) {
      const double partial = emergency_pool_total_ - emergency_pool_used_;
      s.emergency_allocated = true;
      s.emergency_occupancy = partial;
      emergency_pool_used_ += partial;
      accepted += partial;
      remaining -= partial;
    }
  } else if (remaining > 0.0 && s.emergency_allocated) {
    double emergency_available =
        emergency_pool_total_ - emergency_pool_used_ + s.emergency_occupancy;
    const double can_take = std::min(remaining, emergency_available);
    s.emergency_occupancy += can_take;
    emergency_pool_used_ += can_take;
    accepted += can_take;
    remaining -= can_take;
  }

  // Drop the rest.
  if (remaining > 0.0) {
    s.frames_dropped += 1;
    s.total_dropped_bits += static_cast<double>(static_cast<long long>(remaining));
    total_bits_dropped_ += remaining;
  }

  return accepted;
}

void CameraStreamSimulator::DrainBuffers(StreamState& s, double drain_rate) {
  const double drain_from_main = std::min(s.buffer_occupancy, drain_rate);
  s.buffer_occupancy -= drain_from_main;
  s.bytes_served += static_cast<unsigned long long>(drain_from_main / 8.0);
  s.frames_served += 1;

  double remaining_drain = drain_rate - drain_from_main;

  if (remaining_drain > 0.0 && s.emergency_allocated) {
    const double drain_from_emergency =
        std::min(s.emergency_occupancy, remaining_drain);
    s.emergency_occupancy -= drain_from_emergency;
    emergency_pool_used_ -= drain_from_emergency;
    s.bytes_served += static_cast<unsigned long long>(drain_from_emergency / 8.0);

    if (s.emergency_occupancy <= 0.0) {
      s.emergency_allocated = false;
      s.emergency_occupancy = 0.0;
    }
  }

  s.last_served_time = current_time_;
}

void CameraStreamSimulator::RunController(StreamState& s) {
  const double reduction = pi_controller_.Step(s);
  adaptation_engine_.Apply(s, reduction);
}

void CameraStreamSimulator::Step() {
  UpdateBurstStates();

  double total_ingress = 0.0;
  double total_egress = 0.0;
  double total_buffer = 0.0;
  int burst_count = 0;
  int adaptation_count = 0;
  int dropped_frames = 0;

  // Phase 1: ingress + burst detection
  for (StreamState& s : streams_) {
    const double raw_bitrate = ComputeInstantaneousBitrate(s);

    BurstDetector::Result det = burst_detector_.Update(s, raw_bitrate);
    (void)det;  // results handled by the Markov model (UpdateBurstStates)

    const double effective_bitrate = ApplyAdaptationToBitrate(s, raw_bitrate);
    s.current_bitrate = effective_bitrate;

    const double bits_this_step = effective_bitrate * cfg_.dt;
    const double accepted = Ingest(s, bits_this_step);
    (void)accepted;

    total_ingress += bits_this_step;
    total_buffer += s.TotalOccupancy();

    if (s.is_bursting) ++burst_count;
    if (s.adaptation_level > 0) ++adaptation_count;
    // Note: python tracks a cumulative per-stream counter, so drop count here
    // is the running total; keep identical semantics for the history.
    dropped_frames += static_cast<int>(s.frames_dropped);
  }

  // Phase 2: controller (runs at PI_HZ)
  const double controller_interval = 1.0 / cfg_.pi_hz;
  const int controller_every =
      static_cast<int>(controller_interval / cfg_.dt);
  const int step_index = static_cast<int>(current_time_ / cfg_.dt);
  if (controller_every <= 1 || step_index % controller_every == 0) {
    for (StreamState& s : streams_) {
      RunController(s);
    }
  }

  // Phase 3: egress (drain at sustainable rate, faster when occupancy > high WM)
  const double sustainable_per_stream = nic_bandwidth_ / cfg_.n_cameras;
  for (StreamState& s : streams_) {
    double drain_rate = sustainable_per_stream;
    if (s.OccupancyRatio() > kWatermarkHigh) {
      drain_rate *= 1.5;
    }
    DrainBuffers(s, drain_rate * cfg_.dt);
    total_egress += drain_rate * cfg_.dt;
  }

  // History
  time_hist_.push_back(current_time_);
  ingress_hist_.push_back(total_ingress);
  egress_hist_.push_back(total_egress);
  buffer_hist_.push_back(total_buffer);
  emergency_hist_.push_back(emergency_pool_used_);
  burst_count_hist_.push_back(burst_count);
  adapt_count_hist_.push_back(adaptation_count);
  dropped_frames_hist_.push_back(dropped_frames);

  int continuous_streams = 0;
  for (const StreamState& s : streams_) {
    if (current_time_ - s.last_served_time < 2.0 * cfg_.dt) {
      ++continuous_streams;
    }
  }
  continuity_hist_.push_back(static_cast<double>(continuous_streams) / cfg_.n_cameras);

  RecordPerStreamHistory();

  current_time_ += cfg_.dt;
}

void CameraStreamSimulator::RecordPerStreamHistory() {
  if (recorded_streams_.empty()) return;
  if ((static_cast<int>(current_time_ * 10.0) % 10) != 0) return;

  for (size_t j = 0; j < recorded_streams_.size(); ++j) {
    StreamState& s = streams_[static_cast<size_t>(recorded_streams_[j])];
    stream_occupancy_hist_[j].push_back(s.OccupancyRatio());
    stream_bitrate_hist_[j].push_back(s.current_bitrate / 1e6);  // Mbps
    stream_adaptation_hist_[j].push_back(s.adaptation_level);
  }
}

SimMetrics CameraStreamSimulator::Run() {
  const long long n_steps =
      static_cast<long long>(cfg_.duration_seconds / cfg_.dt);

  std::printf("Starting simulation: N=%d, Duration=%.0fs, dt=%.3fs\n",
              cfg_.n_cameras, cfg_.duration_seconds, cfg_.dt);
  std::printf("NIC Bandwidth: %.1f Gbps\n", cfg_.nic_bandwidth_gbps);
  std::printf("Per-stream buffer: %.2f Mb\n", per_stream_buffer_ / 1e6);
  std::printf("Emergency pool: %.0f GB\n", cfg_.emergency_pool_gb);
  std::printf("\n");

  for (long long step = 0; step < n_steps; ++step) {
    Step();
    if (cfg_.verbose && step % 1000 == 0) {
      const double ingress_gbps =
          ingress_hist_.back() / cfg_.dt / 1e9;
      const double egress_gbps = egress_hist_.back() / cfg_.dt / 1e9;
      const int bursts = burst_count_hist_.back();
      const double cont = continuity_hist_.back();
      std::printf("[%6.2fs] Ingress: %5.2f Gbps | Egress: %5.2f Gbps | "
                  "Bursts: %4d | Continuity: %.4f\n",
                  current_time_, ingress_gbps, egress_gbps, bursts, cont);
    }
  }

  // Metrics computation (mirrors _compute_metrics in cam_stream_sim.py)
  SimMetrics m;
  m.duration_seconds = cfg_.duration_seconds;
  m.n_cameras = cfg_.n_cameras;
  m.nic_bandwidth_gbps = cfg_.nic_bandwidth_gbps;

  m.mean_ingress_gbps = mean(ingress_hist_) / cfg_.dt / 1e9;
  m.max_ingress_gbps = max_d(ingress_hist_) / cfg_.dt / 1e9;
  m.mean_egress_gbps = mean(egress_hist_) / cfg_.dt / 1e9;

  m.max_concurrent_bursts = max_int(burst_count_hist_);
  m.mean_concurrent_bursts = mean_int(burst_count_hist_);

  m.max_total_buffer_gb = max_d(buffer_hist_) / 8e9;
  m.mean_total_buffer_gb = mean(buffer_hist_) / 8e9;
  m.max_emergency_pool_gb = max_d(emergency_hist_) / 8e9;
  m.mean_emergency_pool_gb = mean(emergency_hist_) / 8e9;

  m.mean_continuity_ratio = mean(continuity_hist_);
  m.min_continuity_ratio = continuity_hist_.empty()
                               ? 0.0
                               : *std::min_element(continuity_hist_.begin(),
                                                   continuity_hist_.end());

  m.mean_adaptation_level = mean_int(adapt_count_hist_) / cfg_.n_cameras;
  m.max_adaptation_level =
      static_cast<double>(max_int(adapt_count_hist_)) / cfg_.n_cameras;

  m.total_dropped_bits = total_bits_dropped_;
  m.total_dropped_gb = total_bits_dropped_ / 8e9;
  const double total_ingress_bits =
      std::accumulate(ingress_hist_.begin(), ingress_hist_.end(), 0.0);
  m.drop_ratio = total_ingress_bits > 0.0 ? total_bits_dropped_ / total_ingress_bits : 0.0;

  m.streams_with_drops = 0;
  m.max_frames_dropped_per_stream = 0;
  double total_frames_dropped = 0.0;
  for (const StreamState& s : streams_) {
    if (s.frames_dropped > 0) ++m.streams_with_drops;
    m.max_frames_dropped_per_stream =
        std::max<int>(m.max_frames_dropped_per_stream,
                      static_cast<int>(s.frames_dropped));
    total_frames_dropped += static_cast<double>(s.frames_dropped);
  }
  m.mean_frames_dropped_per_stream =
      streams_.empty() ? 0.0 : total_frames_dropped / streams_.size();

  metrics_ = m;
  return m;
}

void CameraStreamSimulator::PrintMetrics(const SimMetrics& m) const {
  std::printf("\n");
  std::printf("==============================================================\n");
  std::printf("SIMULATION RESULTS\n");
  std::printf("==============================================================\n");

  std::printf("\n--- Traffic ---\n");
  std::printf("  Mean Ingress:     %.2f Gbps\n", m.mean_ingress_gbps);
  std::printf("  Max Ingress:      %.2f Gbps\n", m.max_ingress_gbps);
  std::printf("  NIC Capacity:     %.2f Gbps\n", m.nic_bandwidth_gbps);
  std::printf("  Mean Egress:      %.2f Gbps\n", m.mean_egress_gbps);

  std::printf("\n--- Bursts ---\n");
  std::printf("  Max Concurrent:   %.0f\n", m.max_concurrent_bursts);
  std::printf("  Mean Concurrent:  %.2f\n", m.mean_concurrent_bursts);

  std::printf("\n--- Buffers ---\n");
  std::printf("  Max Total Buffer: %.2f GB\n", m.max_total_buffer_gb);
  std::printf("  Mean Total Buffer:%.2f GB\n", m.mean_total_buffer_gb);
  std::printf("  Max Emergency:    %.2f GB\n", m.max_emergency_pool_gb);

  std::printf("\n--- Quality of Service ---\n");
  std::printf("  Mean Continuity:  %.4f%%\n", m.mean_continuity_ratio * 100.0);
  std::printf("  Min Continuity:   %.4f%%\n", m.min_continuity_ratio * 100.0);
  std::printf("  Mean Adaptation:  %.2f%% of streams\n",
              m.mean_adaptation_level * 100.0);

  std::printf("\n--- Drops ---\n");
  std::printf("  Total Dropped:    %.4f GB\n", m.total_dropped_gb);
  std::printf("  Drop Ratio:       %.4f%%\n", m.drop_ratio * 100.0);
  std::printf("  Streams Affected: %d / %d\n", m.streams_with_drops,
              cfg_.n_cameras);

  std::printf("\n");
  std::printf("==============================================================\n");
}

void CameraStreamSimulator::WriteReports(const SimMetrics& m) const {
  ensure_dir(cfg_.output_dir);

  if (cfg_.write_metrics) {
    const std::string txt_path = cfg_.output_dir + "/simulation_metrics.txt";
    std::FILE* f = std::fopen(txt_path.c_str(), "w");
    if (f) {
      std::fprintf(f, "Camera Stream Burst Simulation - Metrics\n");
      std::fprintf(f, "==================================================\n\n");
      std::fprintf(f, "duration_seconds: %.6f\n", m.duration_seconds);
      std::fprintf(f, "n_cameras: %d\n", m.n_cameras);
      std::fprintf(f, "nic_bandwidth_gbps: %.6f\n", m.nic_bandwidth_gbps);
      std::fprintf(f, "mean_ingress_gbps: %.6f\n", m.mean_ingress_gbps);
      std::fprintf(f, "max_ingress_gbps: %.6f\n", m.max_ingress_gbps);
      std::fprintf(f, "mean_egress_gbps: %.6f\n", m.mean_egress_gbps);
      std::fprintf(f, "max_concurrent_bursts: %.0f\n", m.max_concurrent_bursts);
      std::fprintf(f, "mean_concurrent_bursts: %.6f\n", m.mean_concurrent_bursts);
      std::fprintf(f, "max_total_buffer_gb: %.6f\n", m.max_total_buffer_gb);
      std::fprintf(f, "mean_total_buffer_gb: %.6f\n", m.mean_total_buffer_gb);
      std::fprintf(f, "max_emergency_pool_gb: %.6f\n", m.max_emergency_pool_gb);
      std::fprintf(f, "mean_emergency_pool_gb: %.6f\n", m.mean_emergency_pool_gb);
      std::fprintf(f, "mean_continuity_ratio: %.6f\n", m.mean_continuity_ratio);
      std::fprintf(f, "min_continuity_ratio: %.6f\n", m.min_continuity_ratio);
      std::fprintf(f, "mean_adaptation_level: %.6f\n", m.mean_adaptation_level);
      std::fprintf(f, "max_adaptation_level: %.6f\n", m.max_adaptation_level);
      std::fprintf(f, "total_dropped_bits: %.0f\n", m.total_dropped_bits);
      std::fprintf(f, "total_dropped_gb: %.6f\n", m.total_dropped_gb);
      std::fprintf(f, "drop_ratio: %.6f\n", m.drop_ratio);
      std::fprintf(f, "streams_with_drops: %d\n", m.streams_with_drops);
      std::fprintf(f, "max_frames_dropped_per_stream: %d\n",
                   m.max_frames_dropped_per_stream);
      std::fprintf(f, "mean_frames_dropped_per_stream: %.6f\n",
                   m.mean_frames_dropped_per_stream);
      std::fclose(f);
      std::printf("Metrics saved to: %s\n", txt_path.c_str());
    }

    const std::string json_path = cfg_.output_dir + "/simulation_metrics.json";
    std::FILE* jf = std::fopen(json_path.c_str(), "w");
    if (jf) {
      std::fprintf(jf, "{\n");
      std::fprintf(jf, "  \"duration_seconds\": %.6f,\n", m.duration_seconds);
      std::fprintf(jf, "  \"n_cameras\": %d,\n", m.n_cameras);
      std::fprintf(jf, "  \"nic_bandwidth_gbps\": %.6f,\n", m.nic_bandwidth_gbps);
      std::fprintf(jf, "  \"mean_ingress_gbps\": %.6f,\n", m.mean_ingress_gbps);
      std::fprintf(jf, "  \"max_ingress_gbps\": %.6f,\n", m.max_ingress_gbps);
      std::fprintf(jf, "  \"mean_egress_gbps\": %.6f,\n", m.mean_egress_gbps);
      std::fprintf(jf, "  \"max_concurrent_bursts\": %.0f,\n",
                   m.max_concurrent_bursts);
      std::fprintf(jf, "  \"mean_concurrent_bursts\": %.6f,\n",
                   m.mean_concurrent_bursts);
      std::fprintf(jf, "  \"max_total_buffer_gb\": %.6f,\n", m.max_total_buffer_gb);
      std::fprintf(jf, "  \"mean_total_buffer_gb\": %.6f,\n", m.mean_total_buffer_gb);
      std::fprintf(jf, "  \"max_emergency_pool_gb\": %.6f,\n",
                   m.max_emergency_pool_gb);
      std::fprintf(jf, "  \"mean_emergency_pool_gb\": %.6f,\n",
                   m.mean_emergency_pool_gb);
      std::fprintf(jf, "  \"mean_continuity_ratio\": %.6f,\n",
                   m.mean_continuity_ratio);
      std::fprintf(jf, "  \"min_continuity_ratio\": %.6f,\n",
                   m.min_continuity_ratio);
      std::fprintf(jf, "  \"mean_adaptation_level\": %.6f,\n",
                   m.mean_adaptation_level);
      std::fprintf(jf, "  \"max_adaptation_level\": %.6f,\n", m.max_adaptation_level);
      std::fprintf(jf, "  \"total_dropped_bits\": %.0f,\n", m.total_dropped_bits);
      std::fprintf(jf, "  \"total_dropped_gb\": %.6f,\n", m.total_dropped_gb);
      std::fprintf(jf, "  \"drop_ratio\": %.6f,\n", m.drop_ratio);
      std::fprintf(jf, "  \"streams_with_drops\": %d,\n", m.streams_with_drops);
      std::fprintf(jf, "  \"max_frames_dropped_per_stream\": %d,\n",
                   m.max_frames_dropped_per_stream);
      std::fprintf(jf, "  \"mean_frames_dropped_per_stream\": %.6f\n",
                   m.mean_frames_dropped_per_stream);
      std::fprintf(jf, "}\n");
      std::fclose(jf);
    }
  }

  if (cfg_.write_csv) {
    // Aggregate time series.
    const std::string csv_path = cfg_.output_dir + "/timeseries.csv";
    std::FILE* f = std::fopen(csv_path.c_str(), "w");
    if (f) {
      std::fprintf(f,
                   "t,ingress_gbps,egress_gbps,buffer_gb,emergency_gb,"
                   "bursts,adaptations,dropped_frames,continuity\n");
      for (size_t i = 0; i < time_hist_.size(); ++i) {
        std::fprintf(f, "%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%d,%.6f\n",
                     time_hist_[i], ingress_hist_[i] / cfg_.dt / 1e9,
                     egress_hist_[i] / cfg_.dt / 1e9,
                     buffer_hist_[i] / 8e9, emergency_hist_[i] / 8e9,
                     burst_count_hist_[i], adapt_count_hist_[i],
                     dropped_frames_hist_[i], continuity_hist_[i]);
      }
      std::fclose(f);
    }

    // Per-stream sample time series (python-plot-compatible, ~100 ms sampling).
    const std::string scsv_path = cfg_.output_dir + "/stream_samples.csv";
    std::FILE* sf = std::fopen(scsv_path.c_str(), "w");
    if (sf) {
      std::fprintf(sf, "t,stream_id,bitrate_mbps,occupancy_pct,adaptation_level\n");
      for (size_t j = 0; j < recorded_streams_.size(); ++j) {
        for (size_t k = 0; k < stream_bitrate_hist_[j].size(); ++k) {
          std::fprintf(sf, "%.3f,%d,%.6f,%.6f,%d\n", k * 0.1,
                       recorded_streams_[j], stream_bitrate_hist_[j][k],
                       stream_occupancy_hist_[j][k] * 100.0,
                       stream_adaptation_hist_[j][k]);
        }
      }
      std::fclose(sf);
    }
  }
}

}  // namespace bursts