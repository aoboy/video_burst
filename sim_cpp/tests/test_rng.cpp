#include "burst/rng.h"

#include <vector>

#include "test_framework.h"

using bursts::Rng;

TEST(RngDeterministic) {
  Rng a(12345);
  Rng b(12345);
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(a.Uniform(), b.Uniform());
  }
}

TEST(RngDifferentSeedsDiffer) {
  Rng a(1);
  Rng b(2);
  bool any_diff = false;
  for (int i = 0; i < 100; ++i) {
    if (a.Uniform() != b.Uniform()) {
      any_diff = true;
      break;
    }
  }
  EXPECT_TRUE(any_diff);
}

TEST(RngUniformBounds) {
  Rng r(7);
  for (int i = 0; i < 10000; ++i) {
    const double u = r.Uniform();
    EXPECT_TRUE(u >= 0.0 && u < 1.0);
  }
}

TEST(RngUniformRange) {
  Rng r(99);
  for (int i = 0; i < 1000; ++i) {
    const double u = r.Uniform(3.0, 10.0);
    EXPECT_TRUE(u >= 3.0 && u < 10.0);
  }
}

TEST(RngNormalStats) {
  Rng r(2026);
  const int n = 200000;
  double sum = 0.0;
  double sum_sq = 0.0;
  for (int i = 0; i < n; ++i) {
    const double x = r.Normal(5.0, 2.0);
    sum += x;
    sum_sq += x * x;
  }
  const double mean = sum / n;
  const double var = sum_sq / n - mean * mean;
  EXPECT_NEAR(mean, 5.0, 0.05);
  EXPECT_NEAR(var, 4.0, 0.15);
}

TEST(RngPareto) {
  Rng r(11);
  double sum = 0.0;
  int huge = 0;
  const int n = 100000;
  for (int i = 0; i < n; ++i) {
    const double x = r.Pareto(2.0);
    EXPECT_TRUE(x > 0.0);
    sum += x;
    if (x > 100.0) ++huge;  // heavy tail should appear
  }
  // Lomax(shape=2) distribution: E[x] = 1/(shape-1) = 1, with a heavy tail.
  EXPECT_NEAR(sum / n, 1.0, 0.15);
  EXPECT_TRUE(huge > 0);
}

TEST(RngReseedResets) {
  Rng r(42);
  const double first = r.Uniform();
  r.Seed(42);
  EXPECT_EQ(first, r.Uniform());
}