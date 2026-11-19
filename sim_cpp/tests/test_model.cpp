#include "burst/model.h"

#include "test_framework.h"

using bursts::SimulationConfig;
namespace model = bursts::model;

TEST(ModelTimeToOverflowExample) {
  // Docs Part III, 3.2 example: K=200, bbar=3 Mbps, alpha=5, m=6 Mb, Me=512 Gb
  //   T_overflow = (200*6e6 + 512e9) / (200*3e6*4) ~= 213 s
  const double t = model::TimeToOverflow(200.0, 3e6, 5.0, 6e6, 512e9);
  EXPECT_NEAR(t, 213.33, 1.0);
}

TEST(ModelRequiredReduction) {
  EXPECT_NEAR(model::RequiredReduction(5.0), 0.8, 1e-12);
  EXPECT_NEAR(model::RequiredReduction(3.0), 2.0 / 3.0, 1e-12);
  EXPECT_EQ(model::RequiredReduction(1.0), 0.0);
}

TEST(ModelExpectedConcurrentBursts) {
  // N=5000, lambda_on=0.001, lambda_off=0.1 -> E[K] ~= 50
  const double e = model::ExpectedConcurrentBursts(5000.0, 0.001, 0.1);
  EXPECT_NEAR(e, 5000.0 * 0.001 / 0.101, 1e-6);
  EXPECT_NEAR(e, 49.5, 1.0);
}

TEST(ModelProbConcurrentBurstsTiny) {
  // Docs Part III, 7.1: P(K > 200) with N=5000, p=0.01 is ~1e-18.
  const double p = model::ProbConcurrentBurstsGreaterThan(5000.0, 0.01, 200.0);
  EXPECT_LT(p, 1e-15);
  EXPECT_GT(p, 0.0);
}

TEST(ModelProbConcurrentBurstsSanity) {
  // N=10, p=0.5, P(K>5) should be ~ 0.377.
  const double p = model::ProbConcurrentBurstsGreaterThan(10.0, 0.5, 5.0);
  EXPECT_NEAR(p, 0.376953125, 1e-9);
}

TEST(ModelControlLoop) {
  // For Kp=0.5, Ki=0.1: wn=sqrt(0.1), zeta=0.5/(2*sqrt(0.1)).
  EXPECT_NEAR(model::NaturalFrequency(0.1), std::sqrt(0.1), 1e-12);
  EXPECT_NEAR(model::DampingRatio(0.5, 0.1), 0.5 / (2.0 * std::sqrt(0.1)), 1e-12);
  EXPECT_NEAR(model::DampingRatio(0.5, 0.1), 0.7905694, 1e-6);
  // Settling time ~ 6/Kp steps; at Kp=0.5 -> 12 steps == 120 ms @ 100 Hz.
  EXPECT_NEAR(model::SettlingTimeSteps(0.5), 12.0, 1e-12);
}

TEST(ModelEnergyBudget) {
  // Docs Part III, 5.3: M_total=512 GB, D_max=5 s -> 2560 GB*s (bytes).
  // Model returns bits*seconds; convert the doc's bytes*seconds to bits.
  const double budget = model::BurstEnergyBudget(512.0 * 8e9, 5.0);
  EXPECT_NEAR(budget, 2560.0 * 8e9, 1.0);
}

TEST(ModelEvaluateReport) {
  SimulationConfig cfg;
  const auto r = model::Evaluate(cfg, 200.0, 5.0, 10.0);
  EXPECT_GT(r.time_to_overflow_s, 0.0);
  EXPECT_NEAR(r.required_reduction, 0.8, 1e-9);
  EXPECT_GT(r.expected_concurrent_bursts, 0.0);
  EXPECT_GT(r.natural_frequency, 0.0);
  EXPECT_GT(r.damping_ratio, 0.0);
  EXPECT_GT(r.settling_steps, 0.0);
  EXPECT_GT(r.burst_energy_consumed, 0.0);
}