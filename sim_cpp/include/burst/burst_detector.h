#pragma once

#include "burst/stream.h"

namespace bursts {

// Detects burst onset/offset using a dual EWMA (slow baseline + fast instant).
// Mirrors BurstDetector in cam_stream_sim.py.
class BurstDetector {
 public:
  struct Result {
    bool burst_detected;
    bool recovery_detected;
  };

  BurstDetector(double alpha_slow = 0.01, double alpha_fast = 0.2,
                double burst_threshold = 2.0, double recovery_threshold = 1.3);

  // Updates the stream's EWMAs and reports whether a burst boundary was crossed.
  // Returns (burst_detected, recovery_detected).
  Result Update(StreamState& stream, double instantaneous_bitrate);

 private:
  double alpha_slow_;
  double alpha_fast_;
  double burst_threshold_;
  double recovery_threshold_;
};

}  // namespace bursts