#pragma once

namespace bursts {

// Mutable state of a single camera stream.
// Mirrors the StreamState dataclass in cam_stream_sim.py.
struct StreamState {
  int stream_id = 0;

  // Bitrate dynamics
  double baseline_bitrate = 3'000'000.0;
  double current_bitrate = 3'000'000.0;
  double burst_factor = 1.0;

  // Burst model (Markov)
  bool is_bursting = false;
  double burst_start_time = -1.0;
  double burst_duration = 0.0;

  // Buffer state (bits)
  double buffer_capacity = 0.0;
  double buffer_occupancy = 0.0;
  unsigned long long buffer_write_idx = 0;
  unsigned long long buffer_read_idx = 0;

  // Emergency pool
  bool emergency_allocated = false;
  double emergency_occupancy = 0.0;

  // PI controller
  double integral_error = 0.0;
  double last_error = 0.0;

  // Adaptation state (0=none, 1=CRF, 2=resolution, 3=temporal, 4=shed)
  int adaptation_level = 0;
  double crf_offset = 0.0;
  int resolution_scale = 1;  // 1=full, 2=half, 4=quarter
  bool drop_b_frames = false;
  bool drop_p_frames = false;

  // Statistics
  unsigned long long frames_received = 0;
  unsigned long long frames_dropped = 0;
  unsigned long long frames_served = 0;
  double total_dropped_bits = 0.0;
  unsigned long long bytes_received = 0;
  unsigned long long bytes_served = 0;
  int continuity_gaps = 0;
  double last_served_time = 0.0;

  // Dual EWMA for burst detection
  double ewma_slow = 0.0;
  double ewma_fast = 0.0;

  // Occupancy as fraction of capacity (0.0 - 1.0).
  double OccupancyRatio() const {
    return buffer_capacity > 0.0 ? buffer_occupancy / buffer_capacity : 0.0;
  }

  // Total buffered bits (per-stream ring + emergency allocation).
  double TotalOccupancy() const { return buffer_occupancy + emergency_occupancy; }
};

}  // namespace bursts