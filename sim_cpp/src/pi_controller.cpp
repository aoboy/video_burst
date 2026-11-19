#include "burst/pi_controller.h"

#include <algorithm>

namespace bursts {

PIController::PIController(double kp, double ki, double setpoint, double dt,
                           double max_integral)
    : kp_(kp), ki_(ki), setpoint_(setpoint), dt_(dt), max_integral_(max_integral) {}

double PIController::Step(StreamState& stream) {
  const double occupancy_ratio = stream.OccupancyRatio();
  const double error = occupancy_ratio - setpoint_;

  // Proportional term
  const double p_term = kp_ * error;

  // Integral term with anti-windup clamping
  stream.integral_error += ki_ * error * dt_;
  stream.integral_error =
      std::max(-max_integral_, std::min(max_integral_, stream.integral_error));

  // Control output: desired bitrate reduction as fraction of current
  double reduction = p_term + stream.integral_error;
  reduction = std::max(0.0, std::min(1.0, reduction));

  stream.last_error = error;
  return reduction;
}

}  // namespace bursts