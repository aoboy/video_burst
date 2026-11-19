#include "burst/burst_detector.h"

#include "test_framework.h"

using bursts::BurstDetector;
using bursts::StreamState;

TEST(BurstDetectorInitializesEwma) {
  StreamState s;
  s.ewma_slow = 0.0;
  s.ewma_fast = 0.0;
  BurstDetector det;
  const auto res = det.Update(s, 3'000'000.0);
  EXPECT_EQ(s.ewma_slow, 3'000'000.0);
  EXPECT_EQ(s.ewma_fast, 3'000'000.0);
  EXPECT_FALSE(res.burst_detected);
  EXPECT_FALSE(res.recovery_detected);
}

TEST(BurstDetectorSteadyStateNoFlag) {
  StreamState s;
  s.ewma_slow = 3'000'000.0;
  s.ewma_fast = 3'000'000.0;
  BurstDetector det;
  const auto res = det.Update(s, 3'000'000.0);
  EXPECT_FALSE(res.burst_detected);
  EXPECT_FALSE(res.recovery_detected);
}

TEST(BurstDetectorFlagsBurst) {
  StreamState s;
  s.ewma_slow = 3'000'000.0;
  s.ewma_fast = 3'000'000.0;
  BurstDetector det(0.01, 0.2, 2.0, 1.3);
  bool detected = false;
  for (int i = 0; i < 200; ++i) {
    const auto res = det.Update(s, 15'000'000.0);  // 5x baseline
    detected = detected || res.burst_detected;
  }
  EXPECT_TRUE(detected);
}

TEST(BurstDetectorFlagsRecovery) {
  StreamState s;
  s.ewma_slow = 3'000'000.0;
  s.ewma_fast = 3'000'000.0;
  s.is_bursting = true;
  BurstDetector det(0.01, 0.2, 2.0, 1.3);
  bool recovered = false;
  for (int i = 0; i < 500; ++i) {
    const auto res = det.Update(s, 3'000'000.0);  // drop back to baseline
    recovered = recovered || res.recovery_detected;
  }
  EXPECT_TRUE(recovered);
}

TEST(BurstDetectorNoRecoveryWhenNotBursting) {
  StreamState s;
  s.ewma_slow = 3'000'000.0;
  s.ewma_fast = 3'000'000.0;
  s.is_bursting = false;
  BurstDetector det;
  for (int i = 0; i < 600; ++i) {
    const auto res = det.Update(s, 3'000'000.0);
    EXPECT_FALSE(res.recovery_detected);
  }
}