#pragma once

#include "burst/stream.h"

namespace bursts {

// Maps the PI controller output to concrete codec parameters
// (CRF offset / resolution scale / temporal decimation / shedding).
// Mirrors AdaptationEngine in cam_stream_sim.py.
class AdaptationEngine {
 public:
  // Applies adaptation based on a reduction demand in [0.0, 1.0].
  void Apply(StreamState& stream, double reduction);

 private:
  void Restore(StreamState& stream);
};

}  // namespace bursts