// OmegaCalibration::DriftCompensator, driven through the time-injected
// update() overload so the dwell and rate-limit windows are exact and nothing
// sleeps.

#include <chrono>

#include "omega_calibration.hpp"
#include "test_harness.hpp"

using Cal = espp::OmegaCalibration;
using Drift = Cal::DriftCompensator;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace {

const Clock::time_point T0 = Clock::time_point{} + std::chrono::seconds(100);

Clock::time_point at(int ms) { return T0 + milliseconds(ms); }

// dwell 2 s, rate limit 500 ms, 10% blend, idle below 0.05.
Drift::Config test_config() {
  return Drift::Config{.idle_magnitude_threshold = 0.05f,
                       .idle_dwell = milliseconds(2000),
                       .blend_rate = 0.1f,
                       .min_nudge_interval = milliseconds(500)};
}

Cal::CenterResult center_at(float x, float y) {
  Cal::CenterResult c;
  c.center_x = x;
  c.center_y = y;
  return c;
}

} // namespace

TEST(drift_defaults_match_the_documented_tuning) {
  const Drift::Config c;
  CHECK_NEAR(c.idle_magnitude_threshold, 0.05f, 0.0);
  CHECK_EQ(c.idle_dwell, milliseconds(2000));
  CHECK_NEAR(c.blend_rate, 0.02f, 0.0);
  CHECK_EQ(c.min_nudge_interval, milliseconds(500));
}

TEST(drift_no_nudge_before_the_idle_dwell_elapses) {
  Drift d(test_config());
  Cal::CenterResult c = center_at(0.0f, 0.0f);
  CHECK(!d.update(10.0f, 10.0f, 0.0f, c, at(0))); // idle clock starts here
  CHECK(!d.update(10.0f, 10.0f, 0.0f, c, at(1000)));
  CHECK(!d.update(10.0f, 10.0f, 0.0f, c, at(1999)));
  CHECK_NEAR(c.center_x, 0.0f, 0.0);
  CHECK_NEAR(c.center_y, 0.0f, 0.0);
}

TEST(drift_nudges_toward_the_raw_position_once_the_dwell_has_elapsed) {
  Drift d(test_config());
  Cal::CenterResult c = center_at(0.0f, 0.0f);
  CHECK(!d.update(10.0f, -20.0f, 0.0f, c, at(0)));
  CHECK(d.update(10.0f, -20.0f, 0.0f, c, at(2000))); // exactly the dwell
  // center += (raw - center) * blend_rate, per axis.
  CHECK_NEAR(c.center_x, 1.0f, 1e-6);
  CHECK_NEAR(c.center_y, -2.0f, 1e-6);
}

TEST(drift_nudge_moves_the_center_down_when_the_raw_value_is_lower) {
  Drift d(test_config());
  Cal::CenterResult c = center_at(5.0f, 5.0f);
  d.update(0.0f, 5.0f, 0.0f, c, at(0));
  CHECK(d.update(0.0f, 5.0f, 0.0f, c, at(2500)));
  CHECK_NEAR(c.center_x, 4.5f, 1e-6); // 5 + (0 - 5) * 0.1
  CHECK_NEAR(c.center_y, 5.0f, 1e-6); // already at the raw value
}

TEST(drift_repeated_nudges_converge_geometrically_on_the_raw_value) {
  Drift d(test_config());
  Cal::CenterResult c = center_at(0.0f, 0.0f);
  d.update(10.0f, 0.0f, 0.0f, c, at(0));
  int t = 2000;
  for (int n = 1; n <= 3; ++n, t += 500) {
    CHECK(d.update(10.0f, 0.0f, 0.0f, c, at(t)));
    // After n nudges the remaining gap is 10 * 0.9^n.
    float gap = 10.0f;
    for (int i = 0; i < n; ++i) {
      gap *= 0.9f;
    }
    CHECK_NEAR(c.center_x, 10.0f - gap, 1e-4);
  }
}

TEST(drift_nudges_are_rate_limited_by_the_minimum_interval) {
  Drift d(test_config());
  Cal::CenterResult c = center_at(0.0f, 0.0f);
  d.update(10.0f, 0.0f, 0.0f, c, at(0));
  CHECK(d.update(10.0f, 0.0f, 0.0f, c, at(2000)));
  const float after_first = c.center_x;
  CHECK(!d.update(10.0f, 0.0f, 0.0f, c, at(2001)));
  CHECK(!d.update(10.0f, 0.0f, 0.0f, c, at(2499)));
  CHECK_NEAR(c.center_x, after_first, 0.0);        // blocked calls leave the center alone
  CHECK(d.update(10.0f, 0.0f, 0.0f, c, at(2500))); // exactly the interval
  CHECK(!d.update(10.0f, 0.0f, 0.0f, c, at(2999)));
  CHECK(d.update(10.0f, 0.0f, 0.0f, c, at(3000)));
}

TEST(drift_a_magnitude_equal_to_the_threshold_still_counts_as_idle) {
  Drift d(test_config());
  Cal::CenterResult c = center_at(0.0f, 0.0f);
  d.update(10.0f, 0.0f, 0.05f, c, at(0));
  CHECK(d.update(10.0f, 0.0f, 0.05f, c, at(2000))); // only strictly greater is motion
}

TEST(drift_motion_resets_the_idle_clock) {
  Drift d(test_config());
  Cal::CenterResult c = center_at(0.0f, 0.0f);
  d.update(10.0f, 0.0f, 0.0f, c, at(0));
  CHECK(!d.update(10.0f, 0.0f, 0.0f, c, at(1900)));
  CHECK(!d.update(10.0f, 0.0f, 0.5f, c, at(1950))); // stick moved
  // Idle again from 2000: the earlier 1.9 s of rest no longer counts.
  CHECK(!d.update(10.0f, 0.0f, 0.0f, c, at(2000)));
  CHECK(!d.update(10.0f, 0.0f, 0.0f, c, at(3999)));
  CHECK(d.update(10.0f, 0.0f, 0.0f, c, at(4000)));
}

TEST(drift_motion_never_nudges_even_after_a_long_rest) {
  Drift d(test_config());
  Cal::CenterResult c = center_at(0.0f, 0.0f);
  d.update(10.0f, 0.0f, 0.0f, c, at(0));
  CHECK(!d.update(10.0f, 0.0f, 0.5f, c, at(10000)));
  CHECK_NEAR(c.center_x, 0.0f, 0.0);
}

TEST(drift_motion_after_a_nudge_still_honors_the_dwell_again) {
  Drift d(test_config());
  Cal::CenterResult c = center_at(0.0f, 0.0f);
  d.update(10.0f, 0.0f, 0.0f, c, at(0));
  CHECK(d.update(10.0f, 0.0f, 0.0f, c, at(2000)));
  CHECK(!d.update(10.0f, 0.0f, 1.0f, c, at(2100))); // grabbed the stick
  CHECK(!d.update(10.0f, 0.0f, 0.0f, c, at(2200))); // released: dwell restarts
  CHECK(!d.update(10.0f, 0.0f, 0.0f, c, at(4199)));
  CHECK(d.update(10.0f, 0.0f, 0.0f, c, at(4200)));
}

TEST(drift_reset_clears_the_idle_clock) {
  Drift d(test_config());
  Cal::CenterResult c = center_at(0.0f, 0.0f);
  d.update(10.0f, 0.0f, 0.0f, c, at(0));
  d.reset();
  // Without the reset this call (2 s after the first) would nudge.
  CHECK(!d.update(10.0f, 0.0f, 0.0f, c, at(2000)));
  CHECK(!d.update(10.0f, 0.0f, 0.0f, c, at(3999)));
  CHECK(d.update(10.0f, 0.0f, 0.0f, c, at(4000)));
}

TEST(drift_reset_clears_the_rate_limit) {
  // A rate limit longer than the dwell, so only reset() can let the second
  // nudge through before the limit expires.
  Drift d({.idle_magnitude_threshold = 0.05f,
           .idle_dwell = milliseconds(1000),
           .blend_rate = 0.1f,
           .min_nudge_interval = milliseconds(5000)});
  Cal::CenterResult c = center_at(0.0f, 0.0f);
  d.update(10.0f, 0.0f, 0.0f, c, at(0));
  CHECK(d.update(10.0f, 0.0f, 0.0f, c, at(1000)));
  CHECK(!d.update(10.0f, 0.0f, 0.0f, c, at(2100))); // limited until 6000

  d.reset();
  d.update(10.0f, 0.0f, 0.0f, c, at(2200)); // idle restarts
  CHECK(d.update(10.0f, 0.0f, 0.0f, c, at(3200)));
}

TEST(drift_default_constructor_uses_the_default_tuning) {
  Drift d;
  Cal::CenterResult c = center_at(0.0f, 0.0f);
  d.update(100.0f, 0.0f, 0.0f, c, at(0));
  CHECK(!d.update(100.0f, 0.0f, 0.0f, c, at(1999)));
  CHECK(d.update(100.0f, 0.0f, 0.0f, c, at(2000)));
  CHECK_NEAR(c.center_x, 2.0f, 1e-5); // 2% blend
}

TEST(drift_clock_overload_starts_the_idle_window_on_the_first_call) {
  // The real-clock overload delegates to the injected one with steady_clock::now(),
  // so the first idle call can never nudge.
  Drift d;
  Cal::CenterResult c = center_at(0.0f, 0.0f);
  CHECK(!d.update(10.0f, 0.0f, 0.0f, c));
  CHECK(!d.update(10.0f, 0.0f, 0.5f, c));
  CHECK_NEAR(c.center_x, 0.0f, 0.0);
}
