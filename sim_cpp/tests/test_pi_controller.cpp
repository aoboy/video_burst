#include "burst/pi_controller.h"

#include "test_framework.h"

using bursts::PIController;
using bursts::StreamState;

// Helper: stream already at the given occupancy ratio.
StreamState MakeStream(double capacity, double occupancy) {
  StreamState s;
  s.buffer_capacity = capacity;
  s.buffer_occupancy = occupancy;
  return s;
}

TEST(PIControllerZeroErrorZeroReduction) {
  StreamState s = MakeStream(100.0, 40.0);  // at 40% setpoint
  PIController c(0.5, 0.1, 0.4, 0.01, 0.3);
  const double r = c.Step(s);
  EXPECT_NEAR(r, 0.0, 1e-6);
  EXPECT_NEAR(s.integral_error, 0.0, 1e-9);
}

TEST(PIControllerProportionalOnly) {
  StreamState s = MakeStream(100.0, 60.0);  // 60% occupancy, 20pp error
  PIController c(0.5, 0.1, 0.4, 0.01, 0.3);
  double r = c.Step(s);
  // p = 0.5*0.2 = 0.1, plus one step of integral = ki*error*dt = 0.0002
  EXPECT_NEAR(r, 0.1 + 0.1 * 0.2 * 0.01, 1e-9);
  EXPECT_GT(s.integral_error, 0.0);

  // Second step adds a little integral -> reduction > pure proportional.
  double r2 = c.Step(s);
  EXPECT_GT(r2, r);
}

TEST(PIControllerClampedAtZero) {
  StreamState s = MakeStream(100.0, 20.0);  // below setpoint -> negative error
  PIController c(0.5, 0.1, 0.4, 0.01, 0.3);
  double r = c.Step(s);
  EXPECT_EQ(r, 0.0);
}

TEST(PIControllerOutputClampedToOne) {
  StreamState s = MakeStream(100.0, 100.0);  // full
  PIController c(2.0, 2.0, 0.4, 0.01, 0.3);
  double r = 0.0;
  for (int i = 0; i < 200; ++i) {
    s.buffer_occupancy = 100.0;
    r = c.Step(s);
  }
  EXPECT_EQ(r, 1.0);
}

TEST(PIControllerAntiWindup) {
  StreamState s = MakeStream(100.0, 100.0);
  PIController c(0.5, 0.1, 0.4, 0.01, 0.3);
  c.Step(s);
  double max_integral = 0.0;
  for (int i = 0; i < 1000; ++i) {
    s.buffer_occupancy = 100.0;
    c.Step(s);
    if (s.integral_error > max_integral) max_integral = s.integral_error;
  }
  EXPECT_LE(max_integral, 0.3 + 1e-9);
  EXPECT_GE(s.integral_error, -0.3 - 1e-9);
}

TEST(PIControllerIntegralRecovers) {
  StreamState s = MakeStream(100.0, 30.0);  // below setpoint
  s.integral_error = 0.2;                    // existing windup from prior burst
  PIController c(0.5, 0.1, 0.4, 0.01, 0.3);
  // Each step drains ki*|error|*dt = 0.1*0.1*0.01 = 1e-4 from the integral,
  // so 0.2 needs ~2000 steps; use 3000 for headroom.
  for (int i = 0; i < 3000; ++i) {
    c.Step(s);
  }
  EXPECT_LT(s.integral_error, 1e-3);  // windup has been unwound
}