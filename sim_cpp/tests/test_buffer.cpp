#include "burst/simulator.h"

#include "test_framework.h"

using bursts::CameraStreamSimulator;
using bursts::StreamState;

// Minimal config so the emergency pool math is easy to reason about.
bursts::SimulationConfig MakeConfig() {
  bursts::SimulationConfig cfg;
  cfg.n_cameras = 2;
  cfg.duration_seconds = 1.0;
  cfg.baseline_bitrate = 1'000'000.0;  // 1 Mbps
  cfg.per_stream_buffer_s = 0.1;       // 100'000 bits ring buffer
  cfg.emergency_pool_gb = 0.001;       // 8'000'000 bits pool
  cfg.burst_arrival_rate = 0.0;        // deterministic: no bursts
  cfg.seed = 1;
  return cfg;
}

TEST(IngestFitsInRingBuffer) {
  auto cfg = MakeConfig();
  CameraStreamSimulator sim(cfg);
  StreamState s;
  s.buffer_capacity = 100'000.0;
  s.buffer_occupancy = 0.0;
  const double accepted = sim.Ingest(s, 40'000.0);
  EXPECT_NEAR(accepted, 40'000.0, 1e-6);
  EXPECT_NEAR(s.buffer_occupancy, 40'000.0, 1e-6);
  EXPECT_EQ(s.frames_received, 1);
  EXPECT_EQ(s.frames_dropped, 0);
}

TEST(IngestSpillsToEmergencyPool) {
  auto cfg = MakeConfig();
  CameraStreamSimulator sim(cfg);
  StreamState s;
  s.buffer_capacity = 100'000.0;
  s.buffer_occupancy = 100'000.0;  // full ring
  const double accepted = sim.Ingest(s, 40'000.0);
  EXPECT_NEAR(accepted, 40'000.0, 1e-6);
  EXPECT_TRUE(s.emergency_allocated);
  EXPECT_NEAR(s.emergency_occupancy, 40'000.0, 1e-6);
  EXPECT_EQ(s.frames_dropped, 0);
}

TEST(IngestDropsWhenEverythingFull) {
  auto cfg = MakeConfig();
  cfg.emergency_pool_gb = 0.0;  // no emergency room at all
  CameraStreamSimulator sim(cfg);
  StreamState s;
  s.buffer_capacity = 100'000.0;
  s.buffer_occupancy = 100'000.0;
  const double accepted = sim.Ingest(s, 20'000.0);
  EXPECT_NEAR(accepted, 0.0, 1e-6);
  EXPECT_EQ(s.frames_dropped, 1);
  EXPECT_NEAR(s.total_dropped_bits, 20'000.0, 1e-6);
}

TEST(IngestAllocatesPartialEmergencyPool) {
  auto cfg = MakeConfig();
  cfg.emergency_pool_gb = 0.00001;  // 80'000 bits
  CameraStreamSimulator sim(cfg);
  StreamState s;
  s.buffer_capacity = 100'000.0;
  s.buffer_occupancy = 100'000.0;
  const double accepted = sim.Ingest(s, 100'000.0);  // pool only holds 80k
  EXPECT_NEAR(accepted, 80'000.0, 1e-6);
  EXPECT_TRUE(s.emergency_allocated);
  EXPECT_NEAR(s.emergency_occupancy, 80'000.0, 1e-6);
  EXPECT_EQ(s.frames_dropped, 1);  // the remaining 20k dropped
}

TEST(DrainMainBeforeEmergency) {
  auto cfg = MakeConfig();
  CameraStreamSimulator sim(cfg);
  StreamState s;
  s.buffer_capacity = 100'000.0;
  s.buffer_occupancy = 100'000.0;
  sim.Ingest(s, 50'000.0);  // fills emergency with 50k
  EXPECT_TRUE(s.emergency_allocated);

  // Drain 120k: main (100k) first, then 20k from emergency.
  sim.DrainBuffers(s, 120'000.0);
  EXPECT_NEAR(s.buffer_occupancy, 0.0, 1e-6);
  EXPECT_NEAR(s.emergency_occupancy, 30'000.0, 1e-6);
}

TEST(DrainReleasesEmptyEmergency) {
  auto cfg = MakeConfig();
  CameraStreamSimulator sim(cfg);
  StreamState s;
  s.buffer_capacity = 100'000.0;
  s.buffer_occupancy = 100'000.0;  // full ring -> next ingest goes to emergency
  sim.Ingest(s, 50'000.0);         // emergency holds 50k
  EXPECT_TRUE(s.emergency_allocated);
  sim.DrainBuffers(s, 150'000.0);  // 100k main + 50k emergency
  EXPECT_FALSE(s.emergency_allocated);
  EXPECT_EQ(s.emergency_occupancy, 0.0);
}