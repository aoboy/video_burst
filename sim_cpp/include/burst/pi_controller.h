#pragma once

#include "burst/stream.h"

namespace bursts {

// Proportional-Integral controller for bitrate adaptation.
// Mirrors PIController in cam_stream_sim.py.
class PIController {
 public:
  PIController(double kp, double ki, double setpoint, double dt,
               double max_integral = 0.3);

  // Computes the desired bitrate reduction factor in [0, 1]
  // (0 = no reduction, 1 = full reduction). Updates the stream's integral term
  // with anti-windup clamping.
  double Step(StreamState& stream);

 private:
  double kp_;
  double ki_;
  double setpoint_;
  double dt_;
  double max_integral_;
};

}  // namespace bursts