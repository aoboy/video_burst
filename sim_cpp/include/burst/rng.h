#pragma once

#include <cstdint>
#include <random>

namespace bursts {

// Deterministic pseudo-random numbers for the simulation.
// NOTE: intentionally independent of numpy — cross-validation tests use
// tolerances rather than bit-exact equality (see python/compare_with_python.py).
class Rng {
 public:
  explicit Rng(uint32_t seed = 42);

  void Seed(uint32_t seed);

  // Uniform in [0, 1).
  double Uniform();
  // Uniform in [lo, hi).
  double Uniform(double lo, double hi);
  // Gaussian.
  double Normal(double mean, double stddev);
  // Lomax / Pareto II with shape `shape` (scale 1) — matches the burst
  // duration sampling of the python simulator's numpy RandomState.pareto.
  double Pareto(double shape);

 private:
  std::mt19937_64 engine_;
  std::uniform_real_distribution<double> uni_{0.0, 1.0};
};

}  // namespace bursts