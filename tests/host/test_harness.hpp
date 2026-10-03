#pragma once

// Minimal self-contained test harness: TEST / XFAIL_TEST registration plus
// CHECK macros. XFAIL_TEST marks a test that documents a known bug: it is
// expected to fail, and the run stays green while it does. Once the bug is
// fixed the test passes, the runner reports XPASS and FAILS the run so the
// marker gets removed (strict xfail, like pytest).

#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace harness {

struct Failure : std::exception {
  std::string message;
  explicit Failure(std::string m) : message(std::move(m)) {}
  const char *what() const noexcept override { return message.c_str(); }
};

struct Case {
  const char *name;
  std::function<void()> fn;
  bool xfail;
  const char *reason;
};

inline std::vector<Case> &registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Registrar {
  Registrar(const char *name, std::function<void()> fn, bool xfail, const char *reason) {
    registry().push_back({name, std::move(fn), xfail, reason});
  }
};

template <typename A, typename B>
std::string describe(const char *expr, const A &a, const B &b) {
  std::ostringstream os;
  os << expr << " (got " << a << " vs " << b << ")";
  return os.str();
}

inline std::string location(const char *file, int line) {
  return std::string(file) + ":" + std::to_string(line);
}

inline int run_all() {
  int passed = 0, failed = 0, xfailed = 0, xpassed = 0;
  for (const auto &c : registry()) {
    std::string error;
    bool ok = true;
    try {
      c.fn();
    } catch (const std::exception &e) {
      ok = false;
      error = e.what();
    }
    if (!c.xfail) {
      if (ok) {
        ++passed;
        std::printf("  PASS   %s\n", c.name);
      } else {
        ++failed;
        std::printf("  FAIL   %s\n         %s\n", c.name, error.c_str());
      }
    } else if (!ok) {
      ++xfailed;
      std::printf("  XFAIL  %s\n         known bug: %s\n         (%s)\n", c.name, c.reason,
                  error.c_str());
    } else {
      ++xpassed;
      std::printf("  XPASS  %s\n         this known bug now passes; remove its XFAIL marker\n",
                  c.name);
    }
  }
  std::printf("\n%d passed, %d failed, %d expected failures, %d unexpected passes\n", passed,
              failed, xfailed, xpassed);
  return (failed == 0 && xpassed == 0) ? 0 : 1;
}

} // namespace harness

#define HARNESS_CAT2(a, b) a##b
#define HARNESS_CAT(a, b) HARNESS_CAT2(a, b)

#define TEST(name)                                                                             \
  static void HARNESS_CAT(test_fn_, name)();                                                   \
  static harness::Registrar HARNESS_CAT(test_reg_, name)(#name, HARNESS_CAT(test_fn_, name),   \
                                                         false, "");                           \
  static void HARNESS_CAT(test_fn_, name)()

#define XFAIL_TEST(name, reason)                                                               \
  static void HARNESS_CAT(test_fn_, name)();                                                   \
  static harness::Registrar HARNESS_CAT(test_reg_, name)(#name, HARNESS_CAT(test_fn_, name),   \
                                                         true, reason);                        \
  static void HARNESS_CAT(test_fn_, name)()

#define CHECK(cond)                                                                            \
  do {                                                                                         \
    if (!(cond)) {                                                                             \
      throw harness::Failure(harness::location(__FILE__, __LINE__) +                           \
                             ": CHECK failed: " #cond);                                        \
    }                                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                                         \
  do {                                                                                         \
    const auto &harness_va = (a);                                                              \
    const auto &harness_vb = (b);                                                              \
    if (!(harness_va == harness_vb)) {                                                         \
      throw harness::Failure(harness::location(__FILE__, __LINE__) + ": CHECK_EQ failed: " +   \
                             harness::describe(#a " == " #b, harness_va, harness_vb));         \
    }                                                                                          \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                  \
  do {                                                                                         \
    const double harness_va = (a);                                                             \
    const double harness_vb = (b);                                                             \
    if (std::fabs(harness_va - harness_vb) > (eps)) {                                          \
      throw harness::Failure(harness::location(__FILE__, __LINE__) + ": CHECK_NEAR failed: " + \
                             harness::describe(#a " ~= " #b, harness_va, harness_vb));         \
    }                                                                                          \
  } while (0)
