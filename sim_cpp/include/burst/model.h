#pragma once

#include "burst/config.h"

namespace bursts {
namespace model {

// Analytical formulas from Part III of docs/BURST_RESILIENT_CAMERA_SYSTEM.md.
// These are pure functions validated in tests/test_model.cpp.

// Time to buffer overflow without adaptation (seconds):
//   T_overflow = (K*m + M_e) / (K * bbar * (alpha - 1))
// All bit units must be consistent (bits).
double TimeToOverflow(double k, double bbar_bps, double alpha,
                      double per_stream_buffer_bits, double emergency_pool_bits);

// Minimum sustained reduction factor to stop buffer growth: (alpha - 1)/alpha.
double RequiredReduction(double alpha);

// Steady-state expected number of concurrent bursts for a two-state
// Markov-modulated model: E[K] = N * lambda_on / (lambda_on + lambda_off).
double ExpectedConcurrentBursts(double n, double lambda_on, double lambda_off);

// P(K > k) for N independent streams each bursting with probability p.
// Exact binomial tail (log-space, overflow safe).
double ProbConcurrentBurstsGreaterThan(double n, double p, double k);

// Second-order control loop analysis (each stream buffer is an integrator):
//   H(s) = (Kp*s + Ki) / (s^2 + Kp*s + Ki)
double NaturalFrequency(double ki);          // omega_n = sqrt(Ki)
double DampingRatio(double kp, double ki);   // zeta = Kp / (2*sqrt(Ki))
double SettlingTimeSteps(double kp);         // T_s ~= 6 / Kp (controller steps)

// Burst energy budget and consumption (bit^2/bit = product of inputs).
double BurstEnergyBudget(double total_pool_bits, double d_max_s);
double BurstEnergyConsumed(double k, double bbar_bps, double alpha, double w_s);

// Self-contained model report for a representative burst scenario.
struct ModelReport {
  double time_to_overflow_s;
  double required_reduction;
  double expected_concurrent_bursts;
  double prob_greater_than_200;
  double natural_frequency;
  double damping_ratio;
  double settling_steps;
  double settling_ms;
  double burst_energy_budget;
  double burst_energy_consumed;
};

// Uses N, baseline bitrate, emergency pool and PI gains from cfg; the fitting
// parameters (K, alpha, W) describe the representative scenario.
ModelReport Evaluate(const SimulationConfig& cfg, double k_bursty = 200.0,
                     double alpha = 5.0, double w_s = 10.0);

void PrintModel(const ModelReport& r);

}  // namespace model
}  // namespace bursts