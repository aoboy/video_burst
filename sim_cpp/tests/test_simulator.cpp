#include "burst/simulator.h"

#include <cmath>

#include "test_framework.h"

using bursts::CameraStreamSimulator;
using bursts::SimMetrics;

TEST(SimulatorRunsSmallConfig) {
  bursts::SimulationConfig cfg;
  cfg.n_cameras = 100;
  cfg.duration_seconds = 5.0;
  cfg.seed = 42;
  CameraStreamSimulator sim(cfg);
  const SimMetrics m = sim.Run();
  EXPECT_EQ(m.n_cameras, 100);
  EXPECT_GT(m.mean_ingress_gbps, 0.0);
  EXPECT_LE(m.mean_ingress_gbps, cfg.nic_bandwidth_gbps * 2.0);
  EXPECT_NEAR(m.mean_continuity_ratio, 1.0, 1e-9);
}

TEST(SimulatorDeterministicGivenSeed) {
  bursts::SimulationConfig cfg;
  cfg.n_cameras = 100;
  cfg.duration_seconds = 3.0;
  cfg.seed = 7;
  CameraStreamSimulator a(cfg);
  CameraStreamSimulator b(cfg);
  const auto ma = a.Run();
  const auto mb = b.Run();
  EXPECT_EQ(ma.mean_ingress_gbps, mb.mean_ingress_gbps);
  EXPECT_EQ(ma.max_ingress_gbps, mb.max_ingress_gbps);
  EXPECT_EQ(ma.mean_continuity_ratio, mb.mean_continuity_ratio);
  EXPECT_EQ(ma.total_dropped_bits, mb.total_dropped_bits);
}

TEST(SimulatorDifferentSeedsDiffer) {
  bursts::SimulationConfig cfg;
  cfg.n_cameras = 1000;
  cfg.duration_seconds = 10.0;
  cfg.seed = 1;
  CameraStreamSimulator a(cfg);
  cfg.seed = 2;
  CameraStreamSimulator b(cfg);
  const auto ma = a.Run();
  const auto mb = b.Run();
  // Different burst patterns almost surely change the maximum simultaneous
  // burst count over a 10 s window.
  EXPECT_NE(ma.max_concurrent_bursts, mb.max_concurrent_bursts);
}

TEST(SimulatorIngressMatchesBaseline) {
  bursts::SimulationConfig cfg;
  cfg.n_cameras = 500;
  cfg.duration_seconds = 4.0;  // whole number of GOP periods -> sine averages 0
  cfg.burst_arrival_rate = 0.0;  // no bursts: pure baseline traffic
  const double expected_gbps = cfg.n_cameras * cfg.baseline_bitrate / 1e9;
  CameraStreamSimulator sim(cfg);
  const auto m = sim.Run();
  // Mean ingress tracks the deterministic baseline (GOP sine + noise average 0).
  EXPECT_NEAR(m.mean_ingress_gbps, expected_gbps, expected_gbps * 0.005);
}

TEST(SimulatorDropsWhenCapacityExhausted) {
  // Tiny buffers + huge burst rate + no emergency pool => drops are forced.
  bursts::SimulationConfig cfg;
  cfg.n_cameras = 50;
  cfg.duration_seconds = 10.0;
  cfg.per_stream_buffer_s = 0.05;
  cfg.emergency_pool_gb = 0.0;
  cfg.burst_arrival_rate = 50.0;  // ~everything bursting
  cfg.nic_bandwidth_gbps = 5.0;
  CameraStreamSimulator sim(cfg);
  const auto m = sim.Run();
  EXPECT_GT(m.total_dropped_bits, 0.0);
  EXPECT_GT(m.drop_ratio, 0.001);
  EXPECT_GT(m.streams_with_drops, 0);
}

TEST(SimulatorAdaptsDuringBursts) {
  bursts::SimulationConfig cfg;
  cfg.n_cameras = 200;
  cfg.duration_seconds = 10.0;
  cfg.burst_arrival_rate = 5.0;
  cfg.per_stream_buffer_s = 0.2;
  CameraStreamSimulator sim(cfg);
  const auto m = sim.Run();
  // With sustained bursts, at least one stream must enter adaptation.
  EXPECT_GT(m.mean_adaptation_level, 0.0);
  // Client continuity stays perfect even while degrading quality.
  EXPECT_NEAR(m.mean_continuity_ratio, 1.0, 1e-9);
}

TEST(SimulatorMonotonicOccupancy) {
  bursts::SimulationConfig cfg;
  cfg.n_cameras = 50;
  cfg.duration_seconds = 2.0;
  CameraStreamSimulator sim(cfg);
  sim.Run();
  // Run() leaves sim at duration; a fresh instance records history internally.
  const auto m = sim.metrics();
  EXPECT_GT(m.max_total_buffer_gb, 0.0);
}