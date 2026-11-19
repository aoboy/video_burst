#include "burst/rng.h"

#include <algorithm>
#include <cmath>

namespace bursts {

Rng::Rng(uint32_t seed) { Seed(seed); }

void Rng::Seed(uint32_t seed) { engine_.seed(seed); }

double Rng::Uniform() { return uni_(engine_); }

double Rng::Uniform(double lo, double hi) { return lo + (hi - lo) * uni_(engine_); }

double Rng::Normal(double mean, double stddev) {
  std::normal_distribution<double> gauss{0.0, 1.0};
  return mean + stddev * gauss(engine_);
}

double Rng::Pareto(double shape) {
  // Mirrors numpy RandomState.pareto: Lomax / Pareto II,
  // x = (1 - U)^(-1/shape) - 1  (support x >= 0).
  double u = uni_(engine_);
  u = std::max(u, 1e-15);  // avoid log(0)/inf at U == 0
  return std::pow(1.0 - u, -1.0 / shape) - 1.0;
}

}  // namespace bursts