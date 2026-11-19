#pragma once

// Minimal dependency-free test framework with xUnit-style output.
// Define tests with TEST(name), use EXPECT_* macros, link test_main.cpp.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tf {

inline std::vector<std::pair<std::string, std::function<void()>>>& Registry() {
  static std::vector<std::pair<std::string, std::function<void()>>> r;
  return r;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    Registry().emplace_back(name, std::move(fn));
  }
};

inline int g_checks = 0;
inline int g_failures = 0;

inline void Report(bool ok, const char* expr, const char* file, int line,
                   const std::string& extra = "") {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("    [FAIL] %s:%d  %s%s\n", file, line, expr,
                extra.empty() ? "" : extra.c_str());
  }
}

inline int RunAll() {
  std::printf("Running %zu test suite(s)\n", Registry().size());
  for (auto& [name, fn] : Registry()) {
    std::printf("[ RUN  ] %s\n", name.c_str());
    const int before = g_failures;
    fn();
    if (g_failures == before) {
      std::printf("[  OK  ] %s\n", name.c_str());
    } else {
      std::printf("[FAILED] %s\n", name.c_str());
    }
  }
  std::printf("\n%zu suite(s), %d check(s), %d failure(s)\n", Registry().size(),
              g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

}  // namespace tf

#define TEST(name)                                                       \
  static void test_##name();                                             \
  static ::tf::Registrar registrar_##name(#name, test_##name);           \
  static void test_##name()

#define EXPECT_TRUE(x) ::tf::Report(static_cast<bool>(x), #x, __FILE__, __LINE__)
#define EXPECT_FALSE(x) \
  ::tf::Report(!static_cast<bool>(x), "!" #x, __FILE__, __LINE__)
#define EXPECT_EQ(a, b) \
  ::tf::Report((a) == (b), #a " == " #b, __FILE__, __LINE__)
#define EXPECT_NE(a, b) \
  ::tf::Report((a) != (b), #a " != " #b, __FILE__, __LINE__)
#define EXPECT_GT(a, b) ::tf::Report((a) > (b), #a " > " #b, __FILE__, __LINE__)
#define EXPECT_GE(a, b) ::tf::Report((a) >= (b), #a " >= " #b, __FILE__, __LINE__)
#define EXPECT_LT(a, b) ::tf::Report((a) < (b), #a " < " #b, __FILE__, __LINE__)
#define EXPECT_LE(a, b) ::tf::Report((a) <= (b), #a " <= " #b, __FILE__, __LINE__)
#define EXPECT_NEAR(a, b, tol)                                          \
  ::tf::Report(std::fabs((a) - (b)) <= (tol), #a " ~= " #b, __FILE__,   \
               __LINE__)
