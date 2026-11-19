#include "burst/burst_detector.h"

namespace bursts {

BurstDetector::BurstDetector(double alpha_slow, double alpha_fast,
                             double burst_threshold, double recovery_threshold)
    : alpha_slow_(alpha_slow),
      alpha_fast_(alpha_fast),
      burst_threshold_(burst_threshold),
      recovery_threshold_(recovery_threshold) {}

BurstDetector::Result BurstDetector::Update(StreamState& stream,
                                            double instantaneous_bitrate) {
  // Initialize EWMAs on first call.
  if (stream.ewma_slow == 0.0) {
    stream.ewma_slow = instantaneous_bitrate;
    stream.ewma_fast = instantaneous_bitrate;
    return {false, false};
  }

  stream.ewma_slow =
      (1.0 - alpha_slow_) * stream.ewma_slow + alpha_slow_ * instantaneous_bitrate;
  stream.ewma_fast =
      (1.0 - alpha_fast_) * stream.ewma_fast + alpha_fast_ * instantaneous_bitrate;

  const double ratio = stream.ewma_slow > 0.0 ? stream.ewma_fast / stream.ewma_slow : 1.0;

  bool burst_detected = false;
  bool recovery_detected = false;

  if (ratio > burst_threshold_ && !stream.is_bursting) {
    burst_detected = true;
  } else if (ratio < recovery_threshold_ && stream.is_bursting) {
    recovery_detected = true;
  }

  return {burst_detected, recovery_detected};
}

}  // namespace bursts