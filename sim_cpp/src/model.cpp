#include "burst/model.h"

#include <cmath>
#include <cstdio>

namespace bursts {
namespace model {

double TimeToOverflow(double k, double bbar_bps, double alpha,
                      double per_stream_buffer_bits, double emergency_pool_bits) {
  const double numerator = k * per_stream_buffer_bits + emergency_pool_bits;
  const double denominator = k * bbar_bps * (alpha - 1.0);
  return denominator > 0.0 ? numerator / denominator : 0.0;
}

double RequiredReduction(double alpha) {
  return alpha > 1.0 ? (alpha - 1.0) / alpha : 0.0;
}

double ExpectedConcurrentBursts(double n, double lambda_on, double lambda_off) {
  const double denom = lambda_on + lambda_off;
  return denom > 0.0 ? n * lambda_on / denom : 0.0;
}

double ProbConcurrentBurstsGreaterThan(double n, double p, double k) {
  if (k < 0.0) return 1.0;
  const long long n_ll = static_cast<long long>(std::llround(n));
  const long long k_ll = static_cast<long long>(std::llround(k));
  if (n_ll <= k_ll) return 0.0;
  if (p <= 0.0) return 0.0;
  if (p >= 1.0) return k_ll < n_ll ? 1.0 : 0.0;

  const double log_p = std::log(p);
  const double log_q = std::log1p(-p);
  const double log_n_fact = std::lgamma(n_ll + 1.0);

  // Log-PMF of Binomial(n, p) evaluated in log space to avoid overflow.
  auto log_pmf = [&](long long j) -> double {
    return log_n_fact - std::lgamma(j + 1.0) - std::lgamma(n_ll - j + 1.0) +
           j * log_p + (n_ll - j) * log_q;
  };

  // Log-sum-exp over the tail j = k+1 .. n.
  double max_log = -INFINITY;
  for (long long j = k_ll + 1; j <= n_ll; ++j) {
    const double lg = log_pmf(j);
    if (lg > max_log) max_log = lg;
  }
  if (!std::isfinite(max_log)) return 0.0;

  double acc = 0.0;
  for (long long j = k_ll + 1; j <= n_ll; ++j) {
    acc += std::exp(log_pmf(j) - max_log);
  }
  return std::exp(max_log) * acc;
}

double NaturalFrequency(double ki) { return std::sqrt(ki); }

double DampingRatio(double kp, double ki) {
  const double denom = 2.0 * std::sqrt(ki);
  return denom > 0.0 ? kp / denom : 0.0;
}

double SettlingTimeSteps(double kp) {
  return kp > 0.0 ? 6.0 / kp : 0.0;
}

double BurstEnergyBudget(double total_pool_bits, double d_max_s) {
  return total_pool_bits * d_max_s;
}

double BurstEnergyConsumed(double k, double bbar_bps, double alpha, double w_s) {
  return k * bbar_bps * (alpha - 1.0) * w_s;
}

ModelReport Evaluate(const SimulationConfig& cfg, double k_bursty, double alpha,
                     double w_s) {
  const double bbar = cfg.baseline_bitrate;
  const double per_stream_bits = bbar * cfg.per_stream_buffer_s;
  const double pool_bits = cfg.emergency_pool_gb * 8e9;
  const double m_total_bits = cfg.n_cameras * per_stream_bits + pool_bits;
  const double d_max = 5.0;

  ModelReport r;
  r.time_to_overflow_s =
      TimeToOverflow(k_bursty, bbar, alpha, per_stream_bits, pool_bits);
  r.required_reduction = RequiredReduction(alpha);
  r.expected_concurrent_bursts =
      ExpectedConcurrentBursts(cfg.n_cameras, cfg.burst_arrival_rate,
                               cfg.burst_recovery_rate);
  r.prob_greater_than_200 = ProbConcurrentBurstsGreaterThan(
      cfg.n_cameras, cfg.burst_arrival_rate / (cfg.burst_arrival_rate + cfg.burst_recovery_rate), 200.0);
  r.natural_frequency = NaturalFrequency(cfg.pi_ki);
  r.damping_ratio = DampingRatio(cfg.pi_kp, cfg.pi_ki);
  r.settling_steps = SettlingTimeSteps(cfg.pi_kp);
  r.settling_ms = r.settling_steps / cfg.pi_hz * 1000.0;
  r.burst_energy_budget = BurstEnergyBudget(m_total_bits, d_max);
  r.burst_energy_consumed = BurstEnergyConsumed(k_bursty, bbar, alpha, w_s);
  return r;
}

void PrintModel(const ModelReport& r) {
  std::printf("=== Modeling report (Part III of the design doc) ===\n");
  std::printf("  Time to overflow (no adaptation):  %.1f s\n", r.time_to_overflow_s);
  std::printf("  Required reduction for stability:  %.2f (%.0f%%)\n",
              r.required_reduction, r.required_reduction * 100.0);
  std::printf("  E[concurrent bursts]:              %.1f\n", r.expected_concurrent_bursts);
  std::printf("  P(K > 200):                        %.3g\n", r.prob_greater_than_200);
  std::printf("  PI natural frequency (w_n):        %.4f rad/s\n", r.natural_frequency);
  std::printf("  PI damping ratio (zeta):           %.4f\n", r.damping_ratio);
  std::printf("  PI settling time:                  %.1f steps (~%.0f ms @ 100 Hz)\n",
              r.settling_steps, r.settling_ms);
  std::printf("  Burst energy budget:               %.3g bit*s\n", r.burst_energy_budget);
  std::printf("  Burst energy consumed (K,alpha,W): %.3g bit*s\n", r.burst_energy_consumed);
  std::printf("===================================================\n");
}

}  // namespace model
}  // namespace bursts