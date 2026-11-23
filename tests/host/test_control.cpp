// components/omega_control: the pure core of main.cpp's mouse and XAC polling
// loops. Time is injected through StepInput::now (a base time_point plus
// offsets) and every hardware effect goes through a fake Ports that records
// the calls in order, so nothing sleeps and nothing touches USB.
//
// Expected values come from the constants in omega_control.hpp and the default
// DeviceSettings / DriftCompensator::Config, not from running the code.

#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <system_error>
#include <vector>

#include "omega_calibration.hpp"
#include "omega_control.hpp"
#include "test_harness.hpp"

namespace oc = espp::omega_control;
using Cal = espp::OmegaCalibration;
using oc::Clock;
using oc::NudgeDirection;
using std::chrono::milliseconds;

namespace {

const Clock::time_point T0 = Clock::time_point{} + std::chrono::seconds(100);

Clock::time_point at(int ms) { return T0 + milliseconds(ms); }

// The "away from the user" sign for the Y axis, honouring INVERT_NUDGE_Y.
constexpr float FORWARD_Y = oc::INVERT_NUDGE_Y ? 1.0f : -1.0f;

oc::StepInput input(float x, float y, int ms, bool recal = false, float raw_x = 0.0f,
                    float raw_y = 0.0f) {
  oc::StepInput in;
  in.stick = {x, y, raw_x, raw_y};
  in.now = at(ms);
  in.recalibration_requested = recal;
  // DeviceSettings defaults (100 ms hold, 0.5 enter, 0.8 exit), but with nudge
  // clicks on: they are off by default and these tests exercise them.
  in.settings.nudge_enabled = true;
  return in;
}

// An input whose sensor read failed: the sample is the stale previous one.
oc::StepInput failed_read(float x, float y, int ms, float raw_x = 0.0f, float raw_y = 0.0f) {
  oc::StepInput in = input(x, y, ms, false, raw_x, raw_y);
  in.stick.read_ok = false;
  return in;
}

// A mouse state that has taken its first sample (stick at rest) at T0.
oc::MouseState fresh_mouse() {
  oc::MouseState s;
  oc::mouse_step(s, input(0.0f, 0.0f, 0));
  return s;
}

// Expected movement for a stick axis value: truncate, then clamp to +/-127.
std::int8_t expected_delta(float axis) {
  float v = std::trunc(axis * oc::SENSITIVITY);
  v = std::fmin(127.0f, std::fmax(-127.0f, v));
  return static_cast<std::int8_t>(v);
}

// Drives the state into a held nudge at t = 100 ms with the stick at (x, y).
oc::StepOutput enter_nudge(oc::MouseState &s, float x, float y) {
  return oc::mouse_step(s, input(x, y, 100));
}

struct FakePorts : oc::Ports {
  std::vector<std::string> calls;
  std::vector<oc::MouseReport> mouse;
  std::vector<oc::XacReport> xac;
  int observed{0};
  bool sweep_ok{true};
  Cal::CenterResult sweep_center;
  Cal::RangeResult sweep_range;
  oc::StickSample post_sweep;
  Clock::time_point clock{};
  Cal::CenterResult last_applied;
  float live_offset_x{0.0f};
  float live_offset_y{0.0f};

  void send_mouse(const oc::MouseReport &r) override {
    calls.push_back("send_mouse");
    mouse.push_back(r);
  }
  void send_xac(const oc::XacReport &r) override {
    calls.push_back("send_xac");
    xac.push_back(r);
  }
  bool run_sweep(Cal::CenterResult &c, Cal::RangeResult &r) override {
    calls.push_back("run_sweep");
    if (sweep_ok) {
      c = sweep_center;
      r = sweep_range;
    }
    return sweep_ok;
  }
  void save_to_flash() override { calls.push_back("save"); }
  void apply_drift(Cal::CenterResult &c, const Cal::RangeResult &) override {
    calls.push_back("apply_drift");
    last_applied = c;
  }
  // Not recorded in `calls`, so the exact-sequence checks are unaffected.
  void refresh_manual_offset(Cal::CenterResult &c) override {
    c.manual_offset_x = live_offset_x;
    c.manual_offset_y = live_offset_y;
  }
  oc::StickSample read_stick() override {
    calls.push_back("read_stick");
    return post_sweep;
  }
  oc::Clock::time_point now() override { return clock; }
  void sensor_fault_changed(bool faulted, Cal::SensorFaultMonitor::Fault, std::uint32_t) override {
    calls.push_back(faulted ? "fault" : "recovered");
  }
  // Counted apart from `calls` so the exact-sequence checks below are unaffected.
  void sensor_observed_healthy() override { ++observed; }
  // Kept apart from `calls` too: a running calibration reports every 50 ms.
  std::vector<Cal::NudgeCalibrationReport> nudge_reports;
  void nudge_calibration_report(const Cal::NudgeCalibrationReport &r) override {
    nudge_reports.push_back(r);
  }

  int count(const std::string &name) const {
    int n = 0;
    for (const auto &c : calls) {
      if (c == name) {
        ++n;
      }
    }
    return n;
  }
};

const std::vector<std::string> RECAL_SEQUENCE_MOUSE = {"send_mouse", "run_sweep", "read_stick",
                                                       "send_mouse"};
const std::vector<std::string> RECAL_SEQUENCE_XAC = {"send_xac", "run_sweep", "read_stick",
                                                     "send_xac"};

} // namespace

// ---- classify_nudge ---------------------------------------------------------

TEST(control_classify_nudge_picks_the_dominant_axis) {
  CHECK(oc::classify_nudge(0.9f, 0.1f) == NudgeDirection::RIGHT);
  CHECK(oc::classify_nudge(-0.9f, 0.1f) == NudgeDirection::LEFT);
  CHECK(oc::classify_nudge(0.1f, FORWARD_Y * 0.9f) == NudgeDirection::FORWARD);
  CHECK(oc::classify_nudge(0.1f, -FORWARD_Y * 0.9f) == NudgeDirection::BACK);
}

TEST(control_classify_nudge_axis_tie_goes_to_x) {
  CHECK(oc::classify_nudge(0.5f, 0.5f) == NudgeDirection::RIGHT);
  CHECK(oc::classify_nudge(0.5f, -0.5f) == NudgeDirection::RIGHT);
  CHECK(oc::classify_nudge(-0.5f, 0.5f) == NudgeDirection::LEFT);
  CHECK(oc::classify_nudge(-0.5f, -0.5f) == NudgeDirection::LEFT);
}

// ---- nudge window -----------------------------------------------------------

// The detector measures the rise over a trailing nudge_hold_time window on
// every poll, so a push is caught however it lines up with the polls, and a
// rise spread over longer than the window (moving the cursor) is not.
TEST(control_nudge_enters_on_the_poll_the_rise_reaches_the_threshold) {
  oc::MouseState s = fresh_mouse();
  const auto out = oc::mouse_step(s, input(0.0f, FORWARD_Y * 0.9f, 30));
  CHECK(s.nudge == NudgeDirection::FORWARD);
  CHECK_EQ(out.mouse.buttons, 0x01);
}

TEST(control_nudge_ignores_a_rise_spread_over_longer_than_the_window) {
  oc::MouseState s = fresh_mouse();
  // 0.9 over 400 ms: at most about 0.23 in any 100 ms window, under 0.5.
  for (int t = 10; t <= 400; t += 10) {
    oc::mouse_step(s, input(0.0f, FORWARD_Y * 0.9f * static_cast<float>(t) / 400.0f, t));
    CHECK(s.nudge == NudgeDirection::NONE);
  }
}

TEST(control_nudge_push_that_straddles_polls_still_enters_with_a_high_threshold) {
  // 0 to 0.9 over 60 ms, starting at t = 70: block windows at 100 ms would have
  // seen two halves of it. The trailing window sees all of it.
  oc::MouseState s;
  oc::StepInput in = input(0.0f, 0.0f, 0);
  in.settings.nudge_distance_threshold = 0.75f;
  oc::mouse_step(s, in);
  for (int t = 10; t <= 200; t += 10) {
    const float m = t <= 70 ? 0.0f : std::fmin(0.9f, 0.9f * static_cast<float>(t - 70) / 60.0f);
    in = input(0.0f, FORWARD_Y * m, t);
    in.settings.nudge_distance_threshold = 0.75f;
    oc::mouse_step(s, in);
  }
  CHECK(s.nudge == NudgeDirection::FORWARD);
}

TEST(control_nudge_enters_once_the_window_elapsed_and_delta_is_over_threshold) {
  oc::MouseState s = fresh_mouse();
  const auto out = enter_nudge(s, 0.0f, FORWARD_Y * 0.9f);
  CHECK(s.nudge == NudgeDirection::FORWARD);
  CHECK_EQ(out.mouse.buttons, 0x01);
}

TEST(control_nudge_ignores_a_sub_threshold_delta) {
  oc::MouseState s = fresh_mouse();
  const auto out = enter_nudge(s, 0.0f, FORWARD_Y * 0.3f); // delta 0.3 < 0.5
  CHECK(s.nudge == NudgeDirection::NONE);
  CHECK_EQ(out.mouse.buttons, 0);
  // Still plain cursor motion.
  CHECK_EQ(out.mouse.dy, expected_delta(FORWARD_Y * 0.3f));
}

TEST(control_nudge_exits_only_below_the_exit_threshold_after_the_minimum_hold) {
  oc::MouseState s = fresh_mouse();
  enter_nudge(s, 0.0f, FORWARD_Y * 0.9f);
  CHECK(s.nudge == NudgeDirection::FORWARD);

  // Held for less than nudge_hold_time: even a released stick does not drop the nudge yet.
  oc::mouse_step(s, input(0.0f, 0.0f, 150));
  CHECK(s.nudge == NudgeDirection::FORWARD);

  // Next boundary (200 ms) with magnitude 0.85, above the 0.8 exit threshold.
  oc::mouse_step(s, input(0.0f, FORWARD_Y * 0.85f, 200));
  CHECK(s.nudge == NudgeDirection::FORWARD);

  // Next boundary (300 ms) with magnitude 0.79, below it.
  oc::mouse_step(s, input(0.0f, FORWARD_Y * 0.79f, 300));
  CHECK(s.nudge == NudgeDirection::NONE);
}

// ---- per-report output ------------------------------------------------------

TEST(control_forward_sets_button_1_right_sets_button_2_none_clears) {
  {
    oc::MouseState s = fresh_mouse();
    const auto out = enter_nudge(s, 0.0f, FORWARD_Y * 0.9f);
    CHECK_EQ(out.mouse.buttons, 0x01);
  }
  oc::MouseState s = fresh_mouse();
  auto out = enter_nudge(s, 0.9f, 0.0f);
  CHECK(s.nudge == NudgeDirection::RIGHT);
  CHECK_EQ(out.mouse.buttons, 0x02);
  // Release at the next boundary: buttons clear.
  out = oc::mouse_step(s, input(0.0f, 0.0f, 200));
  CHECK(s.nudge == NudgeDirection::NONE);
  CHECK_EQ(out.mouse.buttons, 0);
}

TEST(control_no_cursor_motion_while_nudging) {
  oc::MouseState s = fresh_mouse();
  const auto out = enter_nudge(s, 0.9f, 0.0f);
  CHECK_EQ(out.mouse.dx, 0);
  CHECK_EQ(out.mouse.dy, 0);
  const auto later = oc::mouse_step(s, input(0.9f, 0.3f, 110));
  CHECK_EQ(later.mouse.dx, 0);
  CHECK_EQ(later.mouse.dy, 0);
}

TEST(control_none_state_motion_is_stick_times_sensitivity) {
  oc::MouseState s = fresh_mouse();
  // Magnitude 0.47: under the 0.5 enter threshold, so plain cursor motion.
  const auto out = oc::mouse_step(s, input(0.4f, -0.25f, 10));
  CHECK_EQ(out.mouse.dx, expected_delta(0.4f));
  CHECK_EQ(out.mouse.dy, expected_delta(-0.25f));
  CHECK_EQ(out.mouse.wheel, 0);
  CHECK_EQ(out.mouse.buttons, 0);
  CHECK(out.send_report);
}

TEST(control_none_state_motion_clamps_to_127) {
  oc::MouseState s = fresh_mouse();
  // 150 counts before clamping; nudge clicks off, since a jump this big is a nudge.
  oc::StepInput in = input(10.0f, -10.0f, 10);
  in.settings.nudge_enabled = false;
  const auto out = oc::mouse_step(s, in);
  CHECK_EQ(out.mouse.dx, 127);
  CHECK_EQ(out.mouse.dy, -127);
}

TEST(control_scroll_left_and_back_tick_every_divider_poll_only) {
  for (const bool left : {true, false}) {
    oc::MouseState s = fresh_mouse();
    const float x = left ? -0.9f : 0.0f;
    const float y = left ? 0.0f : -FORWARD_Y * 0.9f;
    const std::int8_t expected = left ? oc::SCROLL_STEP : -oc::SCROLL_STEP;
    // The entry poll counts as poll 1; stay inside the next hold window.
    for (int poll = 1; poll <= 2 * oc::SCROLL_POLL_DIVIDER; ++poll) {
      const auto out = oc::mouse_step(s, input(x, y, 100 + (poll - 1) * 10));
      CHECK(s.nudge == (left ? NudgeDirection::LEFT : NudgeDirection::BACK));
      const bool tick = poll % oc::SCROLL_POLL_DIVIDER == 0;
      CHECK_EQ(out.mouse.wheel, tick ? expected : 0);
      CHECK_EQ(out.mouse.buttons, 0);
    }
  }
}

TEST(control_scroll_count_resets_on_nudge_entry) {
  oc::MouseState s = fresh_mouse();
  enter_nudge(s, -0.9f, 0.0f); // LEFT, poll 1
  oc::mouse_step(s, input(-0.9f, 0.0f, 110));
  oc::mouse_step(s, input(-0.9f, 0.0f, 120));
  CHECK_EQ(s.scroll_poll_count, 3);
  oc::mouse_step(s, input(0.0f, 0.0f, 200)); // released: NONE
  CHECK(s.nudge == NudgeDirection::NONE);
  // Re-enter at the next boundary; the count restarts from the entry poll.
  oc::mouse_step(s, input(-0.9f, 0.0f, 300));
  CHECK(s.nudge == NudgeDirection::LEFT);
  CHECK_EQ(s.scroll_poll_count, 1);
  for (int poll = 2; poll <= oc::SCROLL_POLL_DIVIDER; ++poll) {
    const auto out = oc::mouse_step(s, input(-0.9f, 0.0f, 300 + (poll - 1) * 10));
    CHECK_EQ(out.mouse.wheel, poll == oc::SCROLL_POLL_DIVIDER ? oc::SCROLL_STEP : 0);
  }
}

// ---- recalibration ----------------------------------------------------------

TEST(control_mouse_recalibration_while_nudge_forward_sends_neutral_around_the_sweep) {
  FakePorts ports;
  ports.clock = at(5000);
  oc::MouseState s;
  oc::mouse_tick(s, input(0.0f, 0.0f, 0), ports);
  oc::mouse_tick(s, input(0.0f, FORWARD_Y * 0.9f, 100), ports);
  CHECK(s.nudge == NudgeDirection::FORWARD);
  CHECK_EQ(ports.mouse.back().buttons, 0x01); // the button is held going in
  ports.calls.clear();
  ports.mouse.clear();

  oc::mouse_tick(s, input(0.0f, FORWARD_Y * 0.9f, 110, true), ports);

  CHECK(ports.calls == RECAL_SEQUENCE_MOUSE);
  CHECK_EQ(ports.mouse.size(), size_t{2});
  for (const auto &r : ports.mouse) {
    CHECK_EQ(r.buttons, 0);
    CHECK_EQ(r.dx, 0);
    CHECK_EQ(r.dy, 0);
    CHECK_EQ(r.wheel, 0);
  }
  CHECK(s.nudge == NudgeDirection::NONE);
}

TEST(control_mouse_recalibration_step_is_neutral_and_skips_nudge_and_drift) {
  oc::MouseState s = fresh_mouse();
  enter_nudge(s, 0.0f, FORWARD_Y * 0.9f);
  const auto out = oc::mouse_step(s, input(0.9f, 0.0f, 200, true));
  CHECK(out.send_report);
  CHECK(out.run_recalibration);
  CHECK(!out.drift_applied);
  CHECK(!out.save_calibration);
  CHECK_EQ(out.mouse.buttons, 0);
  CHECK(s.nudge == NudgeDirection::NONE);
  CHECK_EQ(s.scroll_poll_count, 0);
}

TEST(control_xac_recalibration_sends_neutral_around_the_sweep) {
  FakePorts ports;
  ports.clock = at(5000);
  oc::XacState s;
  oc::xac_tick(s, input(0.7f, -0.6f, 0), ports);
  CHECK_NEAR(ports.xac.back().x, 0.7f, 1e-6);
  CHECK_NEAR(ports.xac.back().y, -0.6f, 1e-6);
  ports.calls.clear();
  ports.xac.clear();

  oc::xac_tick(s, input(0.7f, -0.6f, 5, true), ports);

  CHECK(ports.calls == RECAL_SEQUENCE_XAC);
  CHECK_EQ(ports.xac.size(), size_t{2});
  for (const auto &r : ports.xac) {
    CHECK_NEAR(r.x, 0.0f, 0.0);
    CHECK_NEAR(r.y, 0.0f, 0.0);
  }
}

TEST(control_recalibration_restarts_the_nudge_window_at_the_post_sweep_sample) {
  FakePorts ports;
  ports.clock = at(1000);
  ports.post_sweep = {0.0f, FORWARD_Y * 0.9f, 0.0f, 0.0f}; // stick still held after the sweep
  oc::MouseState s;
  oc::mouse_tick(s, input(0.0f, 0.0f, 0), ports);
  oc::mouse_tick(s, input(0.0f, 0.0f, 500, true), ports);
  // The trailing window restarts at the post-sweep sample, so the stick still
  // held there has not risen at all. (With the pre-sweep rest of 0 still in
  // the window this would wrongly enter.)
  CHECK_EQ(s.recent.size(), size_t{1});
  CHECK(s.recent.front().at == at(1000));
  CHECK_NEAR(s.recent.front().magnitude, 0.9f, 1e-6);
  oc::mouse_step(s, input(0.0f, FORWARD_Y * 0.9f, 1050));
  CHECK(s.nudge == NudgeDirection::NONE);
  oc::mouse_step(s, input(0.0f, FORWARD_Y * 0.9f, 1100));
  CHECK(s.nudge == NudgeDirection::NONE);
}

TEST(control_recalibration_resets_the_drift_compensator) {
  // Idle since t = 0; the sweep would have let a nudge through at t = 2000 if
  // the idle timer still spanned it.
  FakePorts ports;
  ports.clock = at(1950);
  oc::MouseState s;
  oc::mouse_tick(s, input(0.0f, 0.0f, 0, false, 5.0f, 5.0f), ports);
  oc::mouse_tick(s, input(0.0f, 0.0f, 1900, true, 5.0f, 5.0f), ports);
  ports.calls.clear();

  oc::mouse_tick(s, input(0.0f, 0.0f, 2000, false, 5.0f, 5.0f), ports); // idle restarts
  CHECK_EQ(ports.count("apply_drift"), 0);
  oc::mouse_tick(s, input(0.0f, 0.0f, 3999, false, 5.0f, 5.0f), ports);
  CHECK_EQ(ports.count("apply_drift"), 0);
  oc::mouse_tick(s, input(0.0f, 0.0f, 4000, false, 5.0f, 5.0f), ports);
  CHECK_EQ(ports.count("apply_drift"), 1);
}

TEST(control_recalibration_success_stores_the_new_calibration) {
  FakePorts ports;
  ports.clock = at(1000);
  ports.sweep_center.center_x = 12.5f;
  ports.sweep_center.deadzone = 0.07f;
  ports.sweep_range.usable_radius = 40.0f;
  oc::MouseState s;
  s.common.center.center_x = 1.0f;
  oc::mouse_tick(s, input(0.0f, 0.0f, 0), ports);
  oc::mouse_tick(s, input(0.0f, 0.0f, 10, true), ports);
  CHECK_NEAR(s.common.center.center_x, 12.5f, 0.0);
  CHECK_NEAR(s.common.center.deadzone, 0.07f, 1e-9);
  CHECK_NEAR(s.common.range.usable_radius, 40.0f, 0.0);
}

TEST(control_recalibration_failure_still_sends_neutral_and_keeps_the_old_calibration) {
  FakePorts ports;
  ports.sweep_ok = false;
  ports.sweep_center.center_x = 99.0f; // must not be adopted
  ports.clock = at(1000);
  oc::MouseState s;
  s.common.center.center_x = 1.0f;
  s.common.range.usable_radius = 30.0f;
  oc::mouse_tick(s, input(0.0f, FORWARD_Y * 0.9f, 0), ports);
  ports.calls.clear();
  ports.mouse.clear();

  oc::mouse_tick(s, input(0.0f, FORWARD_Y * 0.9f, 10, true), ports);

  CHECK(ports.calls == RECAL_SEQUENCE_MOUSE);
  CHECK_EQ(ports.mouse.front().buttons, 0);
  CHECK_EQ(ports.mouse.back().buttons, 0);
  CHECK_NEAR(s.common.center.center_x, 1.0f, 0.0);
  CHECK_NEAR(s.common.range.usable_radius, 30.0f, 0.0);
  CHECK(s.nudge == NudgeDirection::NONE);
}

TEST(control_xac_recalibration_failure_keeps_the_old_calibration) {
  FakePorts ports;
  ports.sweep_ok = false;
  ports.sweep_center.center_x = 99.0f;
  oc::XacState s;
  s.common.center.center_x = 1.0f;
  oc::xac_tick(s, input(0.0f, 0.0f, 0, true), ports);
  CHECK(ports.calls == RECAL_SEQUENCE_XAC);
  CHECK_NEAR(s.common.center.center_x, 1.0f, 0.0);
}

// ---- drift ------------------------------------------------------------------

TEST(control_drift_applies_after_the_dwell_and_pushes_the_new_center) {
  FakePorts ports;
  const oc::DriftCompensator::Config dc; // defaults: 2 s dwell, 2% blend
  oc::MouseState s;
  oc::mouse_tick(s, input(0.0f, 0.0f, 0, false, 5.0f, 5.0f), ports);
  oc::mouse_tick(s, input(0.0f, 0.0f, 1999, false, 5.0f, 5.0f), ports);
  CHECK_EQ(ports.count("apply_drift"), 0);
  ports.calls.clear();

  oc::mouse_tick(s, input(0.0f, 0.0f, 2000, false, 5.0f, 5.0f), ports);

  const std::vector<std::string> expected = {"send_mouse", "apply_drift"};
  CHECK(ports.calls == expected);
  CHECK_NEAR(ports.last_applied.center_x, 5.0f * dc.blend_rate, 1e-6);
  CHECK_NEAR(s.common.center.center_y, 5.0f * dc.blend_rate, 1e-6);
}

TEST(control_drift_uses_the_live_manual_offset_after_a_live_clear) {
  // Cached trim 0.4, then the console clears it. The stick rests at raw 0,
  // which is the new effective center, so drift must not move the center and
  // the output must stay neutral.
  FakePorts ports;
  ports.live_offset_x = 0.0f;
  ports.live_offset_y = 0.0f;

  oc::MouseState m;
  m.common.center.manual_offset_x = 0.4f;
  m.common.center.manual_offset_y = 0.4f;
  for (int t = 0; t <= 6000; t += 500) {
    oc::mouse_tick(m, input(0.0f, 0.0f, t, false, 0.0f, 0.0f), ports);
  }
  CHECK(ports.count("apply_drift") > 0);
  CHECK_NEAR(m.common.center.center_x, 0.0f, 0.0);
  CHECK_NEAR(m.common.center.center_y, 0.0f, 0.0);
  CHECK_NEAR(m.common.center.manual_offset_x, 0.0f, 0.0);
  for (const auto &r : ports.mouse) {
    CHECK_EQ(r.dx, 0);
    CHECK_EQ(r.dy, 0);
  }

  oc::XacState x;
  x.common.center.manual_offset_x = 0.4f;
  x.common.center.manual_offset_y = 0.4f;
  for (int t = 0; t <= 6000; t += 500) {
    oc::xac_tick(x, input(0.0f, 0.0f, t, false, 0.0f, 0.0f), ports);
  }
  CHECK_NEAR(x.common.center.center_x, 0.0f, 0.0);
  CHECK_NEAR(x.common.center.center_y, 0.0f, 0.0);
  for (const auto &r : ports.xac) {
    CHECK_NEAR(r.x, 0.0f, 0.0);
    CHECK_NEAR(r.y, 0.0f, 0.0);
  }
}

TEST(control_drift_respects_the_minimum_interval) {
  FakePorts ports;
  oc::MouseState s;
  oc::mouse_tick(s, input(0.0f, 0.0f, 0, false, 5.0f, 5.0f), ports);
  oc::mouse_tick(s, input(0.0f, 0.0f, 2000, false, 5.0f, 5.0f), ports);
  oc::mouse_tick(s, input(0.0f, 0.0f, 2499, false, 5.0f, 5.0f), ports);
  CHECK_EQ(ports.count("apply_drift"), 1);
  oc::mouse_tick(s, input(0.0f, 0.0f, 2500, false, 5.0f, 5.0f), ports);
  CHECK_EQ(ports.count("apply_drift"), 2);
}

TEST(control_drift_is_not_applied_while_the_stick_is_moving) {
  FakePorts ports;
  oc::MouseState s;
  // Magnitude 0.1 is above the 0.05 idle threshold but far below any nudge.
  for (int ms = 0; ms <= 6000; ms += 100) {
    oc::mouse_tick(s, input(0.1f, 0.0f, ms, false, 5.0f, 5.0f), ports);
  }
  CHECK_EQ(ports.count("apply_drift"), 0);
  CHECK_NEAR(s.common.center.center_x, 0.0f, 0.0);
}

TEST(control_mouse_saves_to_flash_on_every_nth_drift_nudge_only) {
  FakePorts ports;
  oc::MouseState s;
  oc::mouse_tick(s, input(0.0f, 0.0f, 0, false, 5.0f, 5.0f), ports);
  for (int k = 1; k <= 2 * oc::SAVE_EVERY_N_NUDGES; ++k) {
    ports.calls.clear();
    oc::mouse_tick(s, input(0.0f, 0.0f, 2000 + 500 * (k - 1), false, 5.0f, 5.0f), ports);
    CHECK_EQ(ports.count("apply_drift"), 1);
    CHECK_EQ(ports.count("save"), k % oc::SAVE_EVERY_N_NUDGES == 0 ? 1 : 0);
  }
  CHECK_EQ(s.common.drift_nudge_count, 2 * oc::SAVE_EVERY_N_NUDGES);
}

TEST(control_xac_saves_to_flash_on_every_nth_drift_nudge_only) {
  FakePorts ports;
  oc::XacState s;
  oc::xac_tick(s, input(0.0f, 0.0f, 0, false, 5.0f, 5.0f), ports);
  for (int k = 1; k <= 2 * oc::SAVE_EVERY_N_NUDGES; ++k) {
    ports.calls.clear();
    oc::xac_tick(s, input(0.0f, 0.0f, 2000 + 500 * (k - 1), false, 5.0f, 5.0f), ports);
    const std::vector<std::string> expected =
        k % oc::SAVE_EVERY_N_NUDGES == 0
            ? std::vector<std::string>{"send_xac", "apply_drift", "save"}
            : std::vector<std::string>{"send_xac", "apply_drift"};
    CHECK(ports.calls == expected);
  }
}

TEST(control_xac_report_mirrors_the_mapped_stick) {
  oc::XacState s;
  const auto out = oc::xac_step(s, input(0.25f, -0.75f, 0));
  CHECK(out.send_report);
  CHECK_NEAR(out.xac.x, 0.25f, 1e-6);
  CHECK_NEAR(out.xac.y, -0.75f, 1e-6);
}

TEST(control_xac_drift_is_not_applied_while_the_stick_is_moving) {
  FakePorts ports;
  oc::XacState s;
  for (int ms = 0; ms <= 6000; ms += 100) {
    oc::xac_tick(s, input(0.1f, 0.0f, ms, false, 5.0f, 5.0f), ports);
  }
  CHECK_EQ(ports.count("apply_drift"), 0);
}

// ---- nudge on/off switch (DeviceSettings::nudge_enabled) --------------------

namespace {

oc::StepInput nudge_off(oc::StepInput in) {
  in.settings.nudge_enabled = false;
  return in;
}

} // namespace

TEST(control_mouse_nudge_disabled_never_starts_a_nudge) {
  // Each push would nudge with the switch on...
  struct Push {
    float x, y;
    NudgeDirection when_on;
  };
  const Push pushes[] = {{0.9f, 0.0f, NudgeDirection::RIGHT},
                         {-0.9f, 0.0f, NudgeDirection::LEFT},
                         {0.0f, FORWARD_Y * 0.9f, NudgeDirection::FORWARD},
                         {0.0f, -FORWARD_Y * 0.9f, NudgeDirection::BACK}};
  for (const auto &p : pushes) {
    oc::MouseState on = fresh_mouse();
    enter_nudge(on, p.x, p.y);
    CHECK(on.nudge == p.when_on);

    // ...and with it off the same pushes only move the cursor, however long
    // they are held.
    FakePorts ports;
    oc::MouseState s = fresh_mouse();
    for (int ms = 100; ms <= 500; ms += 10) {
      oc::mouse_tick(s, nudge_off(input(p.x, p.y, ms)), ports);
      CHECK(s.nudge == NudgeDirection::NONE);
      CHECK_EQ(ports.mouse.back().buttons, 0);
      CHECK_EQ(ports.mouse.back().wheel, 0);
      CHECK_EQ(ports.mouse.back().dx, expected_delta(p.x));
      CHECK_EQ(ports.mouse.back().dy, expected_delta(p.y));
    }
  }
}

TEST(control_mouse_turning_nudge_off_mid_hold_releases_at_once) {
  FakePorts ports;
  oc::MouseState s = fresh_mouse();
  oc::mouse_tick(s, input(0.0f, FORWARD_Y * 0.9f, 100), ports);
  CHECK(s.nudge == NudgeDirection::FORWARD);
  CHECK_EQ(ports.mouse.back().buttons, 0x01);

  // Switched off between window boundaries with the stick still pushed: the
  // button comes up on this tick and the cursor follows the stick again.
  oc::mouse_tick(s, nudge_off(input(0.0f, FORWARD_Y * 0.9f, 150)), ports);
  CHECK(s.nudge == NudgeDirection::NONE);
  CHECK_EQ(ports.mouse.back().buttons, 0);
  CHECK_EQ(ports.mouse.back().dy, expected_delta(FORWARD_Y * 0.9f));
}

TEST(control_mouse_turning_nudge_off_mid_scroll_stops_the_wheel) {
  oc::MouseState s = fresh_mouse();
  enter_nudge(s, -0.9f, 0.0f);
  CHECK(s.nudge == NudgeDirection::LEFT);
  for (int ms = 110; ms <= 300; ms += 10) {
    const auto out = oc::mouse_step(s, nudge_off(input(-0.9f, 0.0f, ms)));
    CHECK(s.nudge == NudgeDirection::NONE);
    CHECK_EQ(out.mouse.wheel, 0);
    CHECK_EQ(out.mouse.buttons, 0);
    CHECK_EQ(out.mouse.dx, expected_delta(-0.9f));
  }
}

TEST(control_mouse_turning_nudge_back_on_does_not_nudge_from_a_stale_reading) {
  oc::MouseState s = fresh_mouse();
  // Pushed far while off, across several windows.
  for (int ms = 100; ms <= 300; ms += 10) {
    oc::mouse_step(s, nudge_off(input(0.9f, 0.0f, ms)));
  }
  // Back on with the stick still there: no change in magnitude, no nudge.
  for (int ms = 310; ms <= 500; ms += 10) {
    oc::mouse_step(s, input(0.9f, 0.0f, ms));
    CHECK(s.nudge == NudgeDirection::NONE);
  }
  // A fresh push from rest nudges as usual.
  for (int ms = 510; ms <= 600; ms += 10) {
    oc::mouse_step(s, input(0.0f, 0.0f, ms));
  }
  oc::mouse_step(s, input(0.9f, 0.0f, 700));
  CHECK(s.nudge == NudgeDirection::RIGHT);
}

TEST(control_mouse_re_enabling_mid_window_does_not_click_from_movement_made_while_off) {
  oc::MouseState s = fresh_mouse(); // rest baseline at t=0, 100 ms windows
  // Pushed far while off, entirely inside the first window.
  for (int ms = 10; ms <= 90; ms += 10) {
    oc::mouse_step(s, nudge_off(input(0.9f, 0.0f, ms)));
  }
  // Switched back on at the window boundary with the stick still there and
  // no new gesture: the movement made while off must not become a click.
  for (int ms = 100; ms <= 300; ms += 10) {
    const auto out = oc::mouse_step(s, input(0.9f, 0.0f, ms));
    CHECK(s.nudge == NudgeDirection::NONE);
    CHECK_EQ(out.mouse.buttons, 0);
  }
  // A fresh push from rest still nudges.
  for (int ms = 310; ms <= 550; ms += 10) {
    oc::mouse_step(s, input(0.0f, 0.0f, ms));
  }
  oc::mouse_step(s, input(0.9f, 0.0f, 560));
  CHECK(s.nudge == NudgeDirection::RIGHT);
}

TEST(control_xac_output_ignores_the_nudge_switch) {
  // XAC mode has no nudge clicks: the same inputs give the same reports.
  FakePorts on_ports;
  FakePorts off_ports;
  oc::XacState on;
  oc::XacState off;
  const float xs[] = {0.0f, 0.9f, 0.9f, -0.9f, 0.0f, 0.3f};
  int ms = 0;
  for (const float x : xs) {
    for (int k = 0; k < 15; ++k, ms += 10) {
      oc::xac_tick(on, input(x, -x, ms), on_ports);
      oc::xac_tick(off, nudge_off(input(x, -x, ms)), off_ports);
    }
  }
  CHECK_EQ(on_ports.xac.size(), off_ports.xac.size());
  for (size_t i = 0; i < on_ports.xac.size(); ++i) {
    CHECK_EQ(on_ports.xac[i].x, off_ports.xac[i].x);
    CHECK_EQ(on_ports.xac[i].y, off_ports.xac[i].y);
  }
  CHECK_EQ(off_ports.xac.back().x, 0.3f);
}

// ---- sensor fault (D21) -----------------------------------------------------

TEST(control_mouse_sensor_fault_sends_neutral_reports_after_three_failed_reads) {
  FakePorts ports;
  oc::MouseState s;
  oc::mouse_tick(s, input(0.3f, 0.0f, 0), ports);
  CHECK_EQ(ports.mouse.back().dx, expected_delta(0.3f));

  // Two failed reads are a bus glitch: the stale deflection still goes out.
  oc::mouse_tick(s, failed_read(0.3f, 0.0f, 10), ports);
  oc::mouse_tick(s, failed_read(0.3f, 0.0f, 20), ports);
  CHECK_EQ(ports.mouse.back().dx, expected_delta(0.3f));
  CHECK_EQ(ports.count("fault"), 0);

  // The third trips the monitor: neutral from here on, logged once.
  for (int ms = 30; ms <= 500; ms += 10) {
    oc::mouse_tick(s, failed_read(0.3f, 0.0f, ms), ports);
    const auto &r = ports.mouse.back();
    CHECK_EQ(r.buttons, 0);
    CHECK_EQ(r.dx, 0);
    CHECK_EQ(r.dy, 0);
    CHECK_EQ(r.wheel, 0);
  }
  CHECK_EQ(ports.count("fault"), 1);
  CHECK_EQ(ports.count("send_mouse"), 3 + 48);
}

// ---- sensor observed (D22) --------------------------------------------------

TEST(control_mouse_reports_the_sensor_observed_after_ten_good_reads) {
  FakePorts ports;
  oc::MouseState s;
  for (int i = 0; i < 9; i++) {
    oc::mouse_tick(s, input(0.0f, 0.0f, i * 10), ports);
  }
  CHECK_EQ(ports.observed, 0);
  oc::mouse_tick(s, input(0.0f, 0.0f, 90), ports);
  CHECK_EQ(ports.observed, 1);
  // Once per boot, however long it runs.
  for (int i = 10; i < 100; i++) {
    oc::mouse_tick(s, input(0.0f, 0.0f, i * 10), ports);
  }
  CHECK_EQ(ports.observed, 1);
}

TEST(control_xac_reports_the_sensor_observed_after_twenty_good_reads) {
  FakePorts ports;
  oc::XacState s;
  for (int i = 0; i < 19; i++) {
    oc::xac_tick(s, input(0.0f, 0.0f, i * 5), ports);
  }
  CHECK_EQ(ports.observed, 0);
  oc::xac_tick(s, input(0.0f, 0.0f, 95), ports);
  CHECK_EQ(ports.observed, 1);
}

TEST(control_mouse_a_failed_read_restarts_the_observation_count) {
  FakePorts ports;
  oc::MouseState s;
  for (int i = 0; i < 9; i++) {
    oc::mouse_tick(s, input(0.0f, 0.0f, i * 10), ports);
  }
  oc::mouse_tick(s, failed_read(0.0f, 0.0f, 90), ports); // a glitch, not yet a fault
  for (int i = 10; i < 19; i++) {
    oc::mouse_tick(s, input(0.0f, 0.0f, i * 10), ports);
  }
  CHECK_EQ(ports.observed, 0);
  oc::mouse_tick(s, input(0.0f, 0.0f, 190), ports);
  CHECK_EQ(ports.observed, 1);
}

// The startup order the console depends on: a sensor that never answers is
// never reported as observed, so a diagnostics request made before polling
// started and one made while the reads fail both see "not observed" / a fault.
TEST(control_mouse_a_sensor_that_never_answers_is_never_observed) {
  FakePorts ports;
  oc::MouseState s;
  for (int ms = 0; ms <= 2000; ms += 10) {
    oc::mouse_tick(s, failed_read(0.0f, 0.0f, ms), ports);
  }
  CHECK_EQ(ports.count("fault"), 1);
  CHECK_EQ(ports.observed, 0);
}

TEST(control_mouse_observed_follows_a_recovery_from_a_boot_time_fault) {
  FakePorts ports;
  oc::MouseState s;
  for (int ms = 0; ms < 100; ms += 10) {
    oc::mouse_tick(s, failed_read(0.0f, 0.0f, ms), ports);
  }
  CHECK_EQ(ports.count("fault"), 1);
  for (int ms = 100; ms < 300; ms += 10) {
    oc::mouse_tick(s, input(0.0f, 0.0f, ms), ports);
  }
  CHECK_EQ(ports.count("recovered"), 1);
  CHECK_EQ(ports.observed, 1);
}

TEST(control_mouse_sensor_fault_releases_a_held_nudge) {
  FakePorts ports;
  oc::MouseState s = fresh_mouse();
  oc::mouse_tick(s, input(0.9f, 0.0f, 100), ports);
  CHECK(s.nudge == NudgeDirection::RIGHT);
  CHECK_EQ(ports.mouse.back().buttons, 0x02);

  for (int k = 1; k <= 3; ++k) {
    oc::mouse_tick(s, failed_read(0.9f, 0.0f, 100 + 10 * k), ports);
  }
  CHECK(s.nudge == NudgeDirection::NONE);
  CHECK_EQ(ports.mouse.back().buttons, 0);
}

TEST(control_mouse_nudge_and_drift_are_skipped_while_faulted) {
  FakePorts ports;
  oc::MouseState s;
  oc::mouse_tick(s, input(0.0f, 0.0f, 0, false, 5.0f, 5.0f), ports);
  // The stale sample jumps to a big deflection; with the monitor tripped it
  // must neither start a nudge nor let the drift compensator run (the idle
  // dwell of 2 s would otherwise apply a correction).
  for (int ms = 10; ms <= 3000; ms += 10) {
    oc::mouse_tick(s, failed_read(0.9f, 0.0f, ms, 5.0f, 5.0f), ports);
  }
  CHECK(s.nudge == NudgeDirection::NONE);
  CHECK_EQ(ports.count("apply_drift"), 0);
  CHECK_EQ(ports.count("save"), 0);
  CHECK_EQ(s.common.drift_nudge_count, 0);
  for (const auto &r : ports.mouse) {
    CHECK_EQ(r.buttons, 0);
  }
}

TEST(control_mouse_recovery_resumes_output_without_a_bogus_nudge) {
  FakePorts ports;
  oc::MouseState s;
  oc::mouse_tick(s, input(0.0f, 0.0f, 0), ports);
  for (int ms = 10; ms <= 30; ms += 10) {
    oc::mouse_tick(s, failed_read(0.0f, 0.0f, ms), ports);
  }
  CHECK_EQ(ports.count("fault"), 1);

  // The stick is now held deflected. Ten good reads clear the fault; the
  // reports stay neutral until the tenth.
  int ms = 40;
  for (int k = 1; k < 10; ++k, ms += 10) {
    oc::mouse_tick(s, input(0.4f, 0.0f, ms, false, 1.0f + k, 0.0f), ports);
    CHECK_EQ(ports.mouse.back().dx, 0);
  }
  CHECK_EQ(ports.count("recovered"), 0);
  oc::mouse_tick(s, input(0.4f, 0.0f, ms, false, 11.0f, 0.0f), ports);
  CHECK_EQ(ports.count("recovered"), 1);
  CHECK_EQ(ports.mouse.back().dx, expected_delta(0.4f));
}

TEST(control_mouse_recovery_does_not_nudge_from_the_pre_fault_baseline) {
  FakePorts ports;
  oc::MouseState s;
  oc::mouse_tick(s, input(0.0f, 0.0f, 0), ports);
  for (int ms = 10; ms <= 30; ms += 10) {
    oc::mouse_tick(s, failed_read(0.0f, 0.0f, ms), ports);
  }
  // Held at 0.9 throughout recovery and after.
  for (int k = 0; k < 10; ++k) {
    oc::mouse_tick(s, input(0.9f, 0.0f, 40 + 10 * k, false, 1.0f + k, 0.0f), ports);
  }
  CHECK_EQ(ports.count("recovered"), 1);
  for (int k = 1; k <= 30; ++k) {
    oc::mouse_tick(s, input(0.9f, 0.0f, 130 + 10 * k, false, 20.0f + k, 0.0f), ports);
    CHECK(s.nudge == NudgeDirection::NONE);
    CHECK_EQ(ports.mouse.back().buttons, 0);
  }
}

TEST(control_xac_sensor_fault_centers_the_gamepad_after_six_failed_reads) {
  FakePorts ports;
  oc::XacState s;
  oc::xac_tick(s, input(0.5f, -0.5f, 0), ports);
  CHECK_NEAR(ports.xac.back().x, 0.5f, 1e-6);

  // The XAC loop ticks at 5 ms, so six failed reads (30 ms) trip it.
  for (int k = 1; k <= 5; ++k) {
    oc::xac_tick(s, failed_read(0.5f, -0.5f, 5 * k), ports);
    CHECK_NEAR(ports.xac.back().x, 0.5f, 1e-6);
  }
  CHECK_EQ(ports.count("fault"), 0);
  for (int k = 6; k <= 400; ++k) {
    oc::xac_tick(s, failed_read(0.5f, -0.5f, 5 * k), ports);
    CHECK_NEAR(ports.xac.back().x, 0.0f, 0.0);
    CHECK_NEAR(ports.xac.back().y, 0.0f, 0.0);
  }
  CHECK_EQ(ports.count("fault"), 1);
}

TEST(control_xac_drift_is_not_applied_while_faulted) {
  FakePorts ports;
  oc::XacState s;
  oc::xac_tick(s, input(0.0f, 0.0f, 0, false, 5.0f, 5.0f), ports);
  for (int ms = 5; ms <= 3000; ms += 5) {
    oc::xac_tick(s, failed_read(0.0f, 0.0f, ms, 5.0f, 5.0f), ports);
  }
  CHECK_EQ(ports.count("apply_drift"), 0);
  CHECK_EQ(s.common.drift_nudge_count, 0);
}

TEST(control_xac_recovery_resumes_normal_output_after_twenty_good_reads) {
  FakePorts ports;
  oc::XacState s;
  oc::xac_tick(s, input(0.0f, 0.0f, 0), ports);
  for (int k = 1; k <= 6; ++k) {
    oc::xac_tick(s, failed_read(0.0f, 0.0f, 5 * k), ports);
  }
  CHECK_EQ(ports.count("fault"), 1);
  for (int k = 1; k < 20; ++k) {
    oc::xac_tick(s, input(0.25f, 0.5f, 30 + 5 * k, false, 1.0f + k, 0.0f), ports);
    CHECK_NEAR(ports.xac.back().x, 0.0f, 0.0);
  }
  CHECK_EQ(ports.count("recovered"), 0);
  oc::xac_tick(s, input(0.25f, 0.5f, 130, false, 30.0f, 0.0f), ports);
  CHECK_EQ(ports.count("recovered"), 1);
  CHECK_NEAR(ports.xac.back().x, 0.25f, 1e-6);
  CHECK_NEAR(ports.xac.back().y, 0.5f, 1e-6);
}

TEST(control_recalibration_still_runs_while_the_sensor_is_faulted) {
  FakePorts ports;
  oc::MouseState s;
  oc::mouse_tick(s, input(0.0f, 0.0f, 0), ports);
  for (int ms = 10; ms <= 40; ms += 10) {
    oc::mouse_tick(s, failed_read(0.0f, 0.0f, ms), ports);
  }
  ports.calls.clear();
  oc::StepInput recal = failed_read(0.0f, 0.0f, 50);
  recal.recalibration_requested = true;
  oc::mouse_tick(s, recal, ports);
  CHECK(ports.calls == RECAL_SEQUENCE_MOUSE);
}

// ---- output hold (D22 rule 1: neutral during a firmware update) --------------

namespace {

oc::StepInput held(oc::StepInput in) {
  in.output_hold = true;
  return in;
}

} // namespace

TEST(control_mouse_output_hold_sends_neutral_and_releases_a_held_nudge) {
  FakePorts ports;
  oc::MouseState s = fresh_mouse();
  oc::mouse_tick(s, input(0.9f, 0.0f, 100), ports);
  CHECK(s.nudge == NudgeDirection::RIGHT);
  CHECK_EQ(ports.mouse.back().buttons, 0x02);

  // The very first held tick already reports neutral, and so does every later
  // one, however far the stick is pushed.
  for (int ms = 110; ms <= 200; ms += 10) {
    oc::mouse_tick(s, held(input(0.9f, 0.0f, ms)), ports);
    CHECK(s.nudge == NudgeDirection::NONE);
    CHECK_EQ(ports.mouse.back().buttons, 0);
    CHECK_EQ(ports.mouse.back().dx, 0);
    CHECK_EQ(ports.mouse.back().dy, 0);
    CHECK_EQ(ports.mouse.back().wheel, 0);
  }
  CHECK_EQ(ports.count("run_sweep"), 0);
  CHECK_EQ(ports.count("apply_drift"), 0);
}

TEST(control_mouse_output_hold_pre_empts_a_recalibration_request) {
  FakePorts ports;
  oc::MouseState s = fresh_mouse();
  oc::mouse_tick(s, held(input(0.0f, 0.0f, 10, /*recal=*/true)), ports);
  CHECK_EQ(ports.count("run_sweep"), 0);
  CHECK_EQ(ports.calls.size(), size_t{1});
  CHECK_EQ(ports.mouse.back().buttons, 0);
}

TEST(control_mouse_output_hold_release_resumes_without_a_bogus_nudge) {
  FakePorts ports;
  oc::MouseState s = fresh_mouse();
  // The stick is pushed over while held; the first free tick must move the
  // cursor rather than see a jump from the pre-hold reading as a nudge.
  oc::mouse_tick(s, held(input(0.4f, 0.0f, 50)), ports);
  oc::mouse_tick(s, input(0.4f, 0.0f, 60), ports);
  CHECK(s.nudge == NudgeDirection::NONE);
  CHECK_EQ(ports.mouse.back().dx, expected_delta(0.4f));
}

TEST(control_xac_output_hold_centers_the_gamepad_until_released) {
  FakePorts ports;
  oc::XacState s;
  oc::xac_tick(s, input(0.7f, -0.5f, 0), ports);
  CHECK_EQ(ports.xac.back().x, 0.7f);

  oc::xac_tick(s, held(input(0.7f, -0.5f, 5)), ports);
  CHECK_EQ(ports.xac.back().x, 0.0f);
  CHECK_EQ(ports.xac.back().y, 0.0f);
  oc::xac_tick(s, held(input(0.7f, -0.5f, 10, /*recal=*/true)), ports);
  CHECK_EQ(ports.xac.back().x, 0.0f);
  CHECK_EQ(ports.count("run_sweep"), 0);

  oc::xac_tick(s, input(0.7f, -0.5f, 15), ports);
  CHECK_EQ(ports.xac.back().x, 0.7f);
  CHECK_EQ(ports.xac.back().y, -0.5f);
}

// ---- needs_calibration (D26) ------------------------------------------------

namespace {

oc::StepInput uncalibrated(oc::StepInput in) {
  in.needs_calibration = true;
  return in;
}

} // namespace

TEST(control_holds_output_covers_updates_and_a_missing_calibration) {
  oc::StepInput in = input(0.0f, 0.0f, 0);
  CHECK(!oc::holds_output(in));
  CHECK(oc::holds_output(held(in)));
  CHECK(oc::holds_output(uncalibrated(in)));
  // A recalibration ends a needs-calibration hold but waits out an update.
  CHECK(!oc::holds_output(uncalibrated(input(0.0f, 0.0f, 0, /*recal=*/true))));
  CHECK(oc::holds_output(held(input(0.0f, 0.0f, 0, /*recal=*/true))));
}

TEST(control_mouse_needs_calibration_sends_neutral_however_far_the_stick_is) {
  FakePorts ports;
  oc::MouseState s = fresh_mouse();
  // Noise or a push: with no usable calibration nothing reaches the host, not
  // a movement, a nudge click, or a drift correction.
  for (int ms = 10; ms <= 3000; ms += 10) {
    const float x = (ms / 10) % 2 ? 0.9f : -0.9f;
    oc::mouse_tick(s, uncalibrated(input(x, 0.3f, ms)), ports);
    CHECK_EQ(ports.mouse.back().buttons, 0);
    CHECK_EQ(ports.mouse.back().dx, 0);
    CHECK_EQ(ports.mouse.back().dy, 0);
    CHECK_EQ(ports.mouse.back().wheel, 0);
  }
  CHECK(s.nudge == NudgeDirection::NONE);
  CHECK_EQ(ports.count("run_sweep"), 0);
  CHECK_EQ(ports.count("apply_drift"), 0);
  CHECK_EQ(ports.count("save"), 0);
}

TEST(control_mouse_needs_calibration_keeps_the_sensor_monitor_running) {
  // The update confirm checks read the sensor state, so a board waiting for
  // calibration must still report its sensor as observed and healthy.
  FakePorts ports;
  oc::MouseState s = fresh_mouse();
  for (int ms = 10; ms <= 500; ms += 10) {
    oc::mouse_tick(s, uncalibrated(input(0.0f, 0.0f, ms, false, 1.0f + ms * 1e-3f, 2.0f)), ports);
  }
  CHECK_EQ(ports.observed, 1);
  CHECK_EQ(ports.count("fault"), 0);
}

TEST(control_mouse_needs_calibration_lets_a_recalibration_run) {
  FakePorts ports;
  ports.sweep_range.usable_radius = 40.0f;
  oc::MouseState s = fresh_mouse();
  oc::mouse_tick(s, uncalibrated(input(0.0f, 0.0f, 10, /*recal=*/true)), ports);
  CHECK(ports.calls == RECAL_SEQUENCE_MOUSE);
  CHECK_NEAR(s.common.range.usable_radius, 40.0f, 0.0);
}

TEST(control_mouse_resumes_once_the_needs_calibration_flag_clears) {
  FakePorts ports;
  oc::MouseState s = fresh_mouse();
  oc::mouse_tick(s, uncalibrated(input(0.4f, 0.0f, 50)), ports);
  CHECK_EQ(ports.mouse.back().dx, 0);
  // The caller clears the flag after a successful sweep: the next tick moves
  // the cursor, with no nudge from the reading taken during the hold.
  oc::mouse_tick(s, input(0.4f, 0.0f, 60), ports);
  CHECK(s.nudge == NudgeDirection::NONE);
  CHECK_EQ(ports.mouse.back().dx, expected_delta(0.4f));
}

TEST(control_mouse_failed_recalibration_leaves_the_hold_to_the_caller) {
  // A failed (or unusable) sweep changes nothing; the caller keeps
  // needs_calibration set, so the following ticks stay neutral.
  FakePorts ports;
  ports.sweep_ok = false;
  oc::MouseState s = fresh_mouse();
  s.common.range.usable_radius = 0.055f;
  oc::mouse_tick(s, uncalibrated(input(0.0f, 0.0f, 10, /*recal=*/true)), ports);
  CHECK_EQ(ports.count("run_sweep"), 1);
  CHECK_NEAR(s.common.range.usable_radius, 0.055f, 0.0);
  oc::mouse_tick(s, uncalibrated(input(0.9f, 0.0f, 20)), ports);
  CHECK_EQ(ports.mouse.back().dx, 0);
}

TEST(control_xac_needs_calibration_centers_the_gamepad) {
  FakePorts ports;
  oc::XacState s;
  oc::xac_tick(s, uncalibrated(input(0.7f, -0.5f, 0)), ports);
  CHECK_EQ(ports.xac.back().x, 0.0f);
  CHECK_EQ(ports.xac.back().y, 0.0f);
  oc::xac_tick(s, uncalibrated(input(0.7f, -0.5f, 5, /*recal=*/true)), ports);
  CHECK_EQ(ports.count("run_sweep"), 1);
  oc::xac_tick(s, input(0.7f, -0.5f, 10), ports);
  CHECK_EQ(ports.xac.back().x, 0.7f);
}

TEST(recalibration_gate_refuses_a_second_request_until_the_sweep_finishes) {
  oc::RecalibrationGate gate;
  std::error_code ec;
  CHECK(!gate.take()); // nothing requested yet
  CHECK(gate.request(ec));
  CHECK(!ec);

  // Pending, not yet taken: refused.
  CHECK(!gate.request(ec));
  CHECK(ec == std::errc::device_or_resource_busy);

  // The loop takes it once; the sweep is now running and still busy.
  CHECK(gate.take());
  CHECK(!gate.take());
  ec.clear();
  CHECK(!gate.request(ec));
  CHECK(ec == std::errc::device_or_resource_busy);
  CHECK(!gate.take()); // the refused request did not queue a second sweep

  gate.finish();
  ec.clear();
  CHECK(gate.request(ec));
  CHECK(!ec);
  CHECK(gate.take());
}

TEST(recalibration_gate_accepts_a_finish_only_while_the_range_stage_samples) {
  using Stage = Cal::Stage;
  oc::RecalibrationGate gate;
  std::error_code ec;
  CHECK(!gate.request_finish()); // idle
  CHECK(gate.request(ec));
  CHECK(!gate.request_finish()); // requested, not running yet
  CHECK(gate.take());
  CHECK(!gate.request_finish()); // running, but the window is closed

  // The center stages keep the window closed.
  for (Stage s : {Stage::CENTER_SETTLE, Stage::CENTER_MEASURE, Stage::NOISE_SETTLE,
                  Stage::NOISE_MEASURE, Stage::RANGE_PREPARE}) {
    gate.sweep_stage(s);
    CHECK(!gate.request_finish());
  }
  CHECK(!gate.take_finish());

  gate.sweep_stage(Stage::RANGE_SWEEP);
  CHECK(!gate.take_finish()); // nothing asked yet
  CHECK(gate.request_finish());
  // A later report of the same stage keeps the request (only edges reset it).
  gate.sweep_stage(Stage::RANGE_SWEEP);
  CHECK(gate.take_finish());
  CHECK(!gate.take_finish()); // consumed once

  // Leaving the stage closes the window and drops an unconsumed request.
  CHECK(gate.request_finish());
  gate.sweep_stage(Stage::FINISHING);
  CHECK(!gate.take_finish());
  CHECK(!gate.request_finish());
}

TEST(recalibration_gate_finish_never_leaks_into_the_next_sweep) {
  oc::RecalibrationGate gate;
  std::error_code ec;
  CHECK(gate.request(ec));
  CHECK(gate.take());
  gate.set_finish_window(true);
  CHECK(gate.request_finish());
  gate.finish(); // the sweep ended (say it failed) before polling the request
  CHECK(!gate.take_finish());
  CHECK(!gate.request_finish()); // idle again

  CHECK(gate.request(ec));
  CHECK(gate.take());
  CHECK(!gate.request_finish()); // a new sweep starts with its window closed
  gate.sweep_stage(Cal::Stage::RANGE_SWEEP);
  CHECK(!gate.take_finish());
}

// ---- nudge calibration ------------------------------------------------------

namespace {

using Report = Cal::NudgeCalibrationReport;
using Status = Report::Status;

// The stick direction (unit vector) for a cardinal nudge.
void unit(NudgeDirection d, float &x, float &y) {
  x = d == NudgeDirection::RIGHT ? 1.0f : d == NudgeDirection::LEFT ? -1.0f : 0.0f;
  y = d == NudgeDirection::FORWARD ? FORWARD_Y : d == NudgeDirection::BACK ? -FORWARD_Y : 0.0f;
}

/// One push: rest, a linear rise to `peak`, a hold, a linear fall, rest again.
struct Push {
  NudgeDirection dir{NudgeDirection::FORWARD};
  float peak{0.8f};
  int rise_ms{60};
  int hold_ms{150};
  int fall_ms{60};
};

/// The stick position `t` ms after the push began (0 before and after it).
void push_at(const Push &p, int t, float &x, float &y) {
  float m = 0.0f;
  if (t <= 0) {
    m = 0.0f;
  } else if (t < p.rise_ms) {
    m = p.peak * static_cast<float>(t) / static_cast<float>(p.rise_ms);
  } else if (t < p.rise_ms + p.hold_ms) {
    m = p.peak;
  } else if (t < p.rise_ms + p.hold_ms + p.fall_ms) {
    const int into = t - p.rise_ms - p.hold_ms;
    m = p.peak * (1.0f - static_cast<float>(into) / static_cast<float>(p.fall_ms));
  }
  unit(p.dir, x, y);
  x *= m;
  y *= m;
}

int push_length(const Push &p) { return p.rise_ms + p.hold_ms + p.fall_ms + 10; }

// Raw readings carry a little noise, as a real sensor's do, so the stuck-sensor
// check (D21) does not trip during a long calibration.
oc::StepInput cal_input(float x, float y, int ms) {
  const float noise = 0.01f * static_cast<float>((ms / 10) % 7);
  oc::StepInput in = input(x, y, ms, false, 40.0f * x + noise, 40.0f * y - noise);
  in.settings.nudge_enabled = false; // the calibration does not depend on the switch
  return in;
}

/// Feeds `rest_ms` of rest and then the push through mouse_tick, 10 ms a tick,
/// from `t`. Returns the time after the push.
int drive_push(oc::MouseState &s, FakePorts &ports, int t, const Push &p, int rest_ms = 400) {
  for (int end = t + rest_ms; t < end; t += 10) {
    oc::mouse_tick(s, cal_input(0.0f, 0.0f, t), ports);
  }
  const int start = t;
  for (; t <= start + push_length(p); t += 10) {
    float x, y;
    push_at(p, t - start, x, y);
    oc::mouse_tick(s, cal_input(x, y, t), ports);
  }
  return t;
}

/// Starts a calibration at `t` with the stick at rest.
void start_calibration(oc::MouseState &s, FakePorts &ports, int t) {
  oc::StepInput in = cal_input(0.0f, 0.0f, t);
  in.nudge_calibration_start = true;
  oc::mouse_tick(s, in, ports);
}

std::vector<Report> final_reports(const FakePorts &ports) {
  std::vector<Report> out;
  for (const auto &r : ports.nudge_reports) {
    if (r.status != Status::RUNNING) {
      out.push_back(r);
    }
  }
  return out;
}

/// NUDGE_CAL_REPEATS pushes of each kind, in the order the calibration asks for them.
std::vector<Push> twelve(const std::array<Push, 4> &kinds) {
  std::vector<Push> out;
  for (const NudgeDirection d : oc::NUDGE_CAL_ORDER) {
    for (const auto &k : kinds) {
      if (k.dir == d) {
        for (int i = 0; i < oc::NUDGE_CAL_REPEATS; ++i) {
          out.push_back(k);
        }
      }
    }
  }
  return out;
}

// Different in each direction, as a real hand would be: back is the weakest
// and slowest.
const std::array<Push, 4> VARIED{{
    {NudgeDirection::FORWARD, 0.9f, 40, 150, 50},
    {NudgeDirection::RIGHT, 0.7f, 80, 150, 60},
    {NudgeDirection::BACK, 0.5f, 120, 150, 80},
    {NudgeDirection::LEFT, 0.8f, 60, 150, 60},
}};

} // namespace

TEST(control_window_rise_of_a_step_is_the_whole_step) {
  std::vector<oc::NudgeSample> trace{{0, 0.0f}};
  for (int t = 10; t <= 300; t += 10) {
    trace.push_back({t, 0.8f});
  }
  CHECK_NEAR(oc::window_rise(trace, milliseconds(100)), 0.8f, 1e-6);
}

TEST(control_window_rise_of_a_ramp_is_what_the_window_spans) {
  // 0.2 per 10 ms up to 0.8, then held: a 50 ms window spans the whole 40 ms
  // ramp, a 20 ms one two steps of it.
  std::vector<oc::NudgeSample> trace{{0, 0.0f}, {10, 0.2f}, {20, 0.4f}, {30, 0.6f}, {40, 0.8f}};
  for (int t = 50; t <= 300; t += 10) {
    trace.push_back({t, 0.8f});
  }
  CHECK_NEAR(oc::window_rise(trace, milliseconds(50)), 0.8f, 1e-6);
  CHECK_NEAR(oc::window_rise(trace, milliseconds(20)), 0.4f, 1e-6);
}

TEST(control_nudge_calibration_is_not_pulled_down_by_one_gentle_push) {
  // Eleven full pushes and one gentle one: the threshold follows the typical push.
  FakePorts ports;
  oc::MouseState s;
  start_calibration(s, ports, 0);
  int t = 10;
  for (int i = 0; i < oc::NUDGE_CAL_TOTAL; ++i) {
    const NudgeDirection d = oc::NUDGE_CAL_ORDER[i / oc::NUDGE_CAL_REPEATS];
    t = drive_push(s, ports, t, {d, i == 7 ? 0.4f : 0.95f, 50, 150, 60});
  }
  const auto finals = final_reports(ports);
  CHECK_EQ(finals.size(), size_t{1});
  CHECK(finals.front().status == Status::SUCCEEDED);
  CHECK_NEAR(finals.front().enter_threshold, oc::NUDGE_CAL_ENTER_MARGIN * 0.95f, 0.02);
}

TEST(control_nudge_calibration_median_of_an_even_count_is_the_middle_pair) {
  std::vector<float> v{0.9f, 0.5f, 0.7f, 0.8f};
  CHECK_NEAR(oc::median_of(v), 0.75f, 1e-6);
  std::vector<float> odd{0.3f, 0.9f, 0.6f};
  CHECK_NEAR(oc::median_of(odd), 0.6f, 1e-6);
}

TEST(control_nudge_calibration_of_full_nudges_sets_a_high_enter_threshold) {
  // Full pushes (peak 1.0, 50 ms rise) in every direction: the threshold lands
  // near the push, NUDGE_CAL_ENTER_MARGIN of it, not half of it.
  FakePorts ports;
  oc::MouseState s;
  start_calibration(s, ports, 0);
  int t = 10;
  for (const NudgeDirection d : oc::NUDGE_CAL_ORDER) {
    for (int i = 0; i < oc::NUDGE_CAL_REPEATS; ++i) {
      t = drive_push(s, ports, t, {d, 1.0f, 50, 150, 60});
    }
  }
  const auto finals = final_reports(ports);
  CHECK_EQ(finals.size(), size_t{1});
  CHECK(finals.front().status == Status::SUCCEEDED);
  CHECK(finals.front().enter_threshold >= 0.7f);
  CHECK(finals.front().enter_threshold <= oc::NUDGE_CAL_ENTER_MARGIN + 1e-6f);
}

TEST(control_nudge_calibration_full_run_succeeds_and_every_nudge_clicks_with_the_result) {
  FakePorts ports;
  oc::MouseState s;
  start_calibration(s, ports, 0);
  CHECK(s.nudge_calibration.running());
  CHECK(ports.nudge_reports.front().status == Status::RUNNING);
  CHECK_EQ(ports.nudge_reports.front().total, oc::NUDGE_CAL_TOTAL);
  CHECK_EQ(ports.nudge_reports.front().expected_direction,
           static_cast<std::uint8_t>(NudgeDirection::FORWARD));

  int t = 10;
  for (const auto &p : twelve(VARIED)) {
    t = drive_push(s, ports, t, p);
  }
  CHECK(!s.nudge_calibration.running());
  const auto finals = final_reports(ports);
  CHECK_EQ(finals.size(), size_t{1});
  const Report &r = finals.front();
  CHECK(r.status == Status::SUCCEEDED);
  CHECK_EQ(r.accepted, oc::NUDGE_CAL_TOTAL);
  // The median push rises 0.75 (between the 0.7 and 0.8 ones): the threshold
  // is NUDGE_CAL_ENTER_MARGIN of it, above the weak back push.
  CHECK_NEAR(r.enter_threshold, oc::NUDGE_CAL_ENTER_MARGIN * 0.75f, 0.02);
  CHECK_NEAR(r.exit_threshold, oc::NUDGE_CAL_EXIT_FRACTION * 0.5f, 1e-6);
  CHECK(r.hold_time_ms >= static_cast<std::uint32_t>(oc::NUDGE_CAL_MIN_WINDOW.count()));
  CHECK(r.hold_time_ms <= static_cast<std::uint32_t>(oc::NUDGE_CAL_MAX_WINDOW.count()));
  CHECK_EQ(r.hold_time_ms % 10, 0u);

  // Every report sent while it ran was neutral: the nudges did not click.
  for (const auto &m : ports.mouse) {
    CHECK_EQ(m.buttons, 0);
    CHECK_EQ(m.dx, 0);
    CHECK_EQ(m.dy, 0);
    CHECK_EQ(m.wheel, 0);
  }

  // Replayed through the real detector with the derived settings, every kind
  // of push at least as big as the threshold enters the right nudge whatever
  // the phase, and lets go; the weak back push is deliberately left out.
  oc::DeviceSettings tuned;
  tuned.nudge_enabled = true;
  tuned.nudge_distance_threshold = r.enter_threshold;
  tuned.nudge_exit_threshold = r.exit_threshold;
  tuned.nudge_hold_time = milliseconds(r.hold_time_ms);
  for (const auto &p : VARIED) {
    for (int phase = 0; phase < static_cast<int>(r.hold_time_ms); phase += 10) {
      oc::MouseState m;
      int now = 0;
      auto step = [&](float x, float y) {
        oc::StepInput in = input(x, y, now);
        in.settings = tuned;
        oc::mouse_step(m, in);
        now += 10;
      };
      for (int i = 0; i < 50 + phase / 10; ++i) {
        step(0.0f, 0.0f);
      }
      bool entered = false;
      bool wrong = false;
      for (int into = 0; into <= push_length(p); into += 10) {
        float x, y;
        push_at(p, into, x, y);
        step(x, y);
        entered = entered || m.nudge == p.dir;
        wrong = wrong || (m.nudge != NudgeDirection::NONE && m.nudge != p.dir);
      }
      for (int i = 0; i < 2 * static_cast<int>(r.hold_time_ms) / 10 + 2; ++i) {
        step(0.0f, 0.0f);
      }
      CHECK_EQ(entered, p.peak >= r.enter_threshold);
      CHECK(!wrong);
      CHECK(m.nudge == NudgeDirection::NONE);
    }
  }
}

TEST(control_nudge_calibration_counts_only_the_direction_asked_for_and_big_enough_pushes) {
  FakePorts ports;
  oc::MouseState s;
  start_calibration(s, ports, 0);
  int t = drive_push(s, ports, 10, {NudgeDirection::RIGHT, 0.8f}); // FORWARD is asked for
  auto last = ports.nudge_reports.back();
  CHECK(last.last_attempt == Report::Attempt::WRONG_DIRECTION);
  CHECK_EQ(last.last_direction, static_cast<std::uint8_t>(NudgeDirection::RIGHT));
  CHECK_EQ(last.accepted, 0);

  t = drive_push(s, ports, t, {NudgeDirection::FORWARD, 0.2f}); // under NUDGE_CAL_MIN_PEAK
  last = ports.nudge_reports.back();
  CHECK(last.last_attempt == Report::Attempt::TOO_SMALL);
  CHECK_EQ(last.accepted, 0);

  drive_push(s, ports, t, {NudgeDirection::FORWARD, 0.8f});
  last = ports.nudge_reports.back();
  CHECK(last.last_attempt == Report::Attempt::ACCEPTED);
  CHECK_EQ(last.accepted, 1);
  CHECK(s.nudge_calibration.running());
}

TEST(control_nudge_calibration_ignores_a_push_made_before_the_stick_settled) {
  FakePorts ports;
  oc::MouseState s;
  start_calibration(s, ports, 0);
  // Only 100 ms of rest before the push: shorter than NUDGE_CAL_REST_HOLD.
  int t = drive_push(s, ports, 10, {NudgeDirection::FORWARD, 0.8f}, 100);
  CHECK(ports.nudge_reports.back().last_attempt == Report::Attempt::NONE);
  CHECK_EQ(ports.nudge_reports.back().accepted, 0);
  // The ready flag goes up once it has rested long enough.
  for (int end = t + 400; t < end; t += 10) {
    oc::mouse_tick(s, cal_input(0.0f, 0.0f, t), ports);
  }
  CHECK(ports.nudge_reports.back().ready);
}

TEST(control_nudge_calibration_cancel_changes_nothing_and_the_cursor_moves_again) {
  FakePorts ports;
  oc::MouseState s;
  start_calibration(s, ports, 0);
  int t = drive_push(s, ports, 10, {NudgeDirection::FORWARD, 0.8f});
  oc::StepInput in = cal_input(0.0f, 0.0f, t);
  in.nudge_calibration_cancel = true;
  oc::mouse_tick(s, in, ports);
  CHECK(!s.nudge_calibration.running());
  const auto finals = final_reports(ports);
  CHECK_EQ(finals.size(), size_t{1});
  CHECK(finals.front().status == Status::CANCELLED);
  CHECK(!finals.front().message.empty());

  ports.mouse.clear();
  oc::mouse_tick(s, input(0.4f, 0.0f, t + 10), ports);
  CHECK_EQ(ports.mouse.back().dx, expected_delta(0.4f));
}

TEST(control_nudge_calibration_start_and_cancel_in_one_tick_end_cancelled) {
  FakePorts ports;
  oc::MouseState s;
  oc::StepInput in = cal_input(0.0f, 0.0f, 0);
  in.nudge_calibration_start = true;
  in.nudge_calibration_cancel = true;
  oc::mouse_tick(s, in, ports);
  CHECK(!s.nudge_calibration.running());
  CHECK_EQ(ports.nudge_reports.size(), size_t{2});
  CHECK(ports.nudge_reports[0].status == Status::RUNNING);
  CHECK(ports.nudge_reports[1].status == Status::CANCELLED);
}

TEST(control_nudge_calibration_cancel_without_one_running_reports_nothing) {
  FakePorts ports;
  oc::MouseState s;
  oc::StepInput in = cal_input(0.0f, 0.0f, 0);
  in.nudge_calibration_cancel = true;
  oc::mouse_tick(s, in, ports);
  CHECK(ports.nudge_reports.empty());
}

TEST(control_nudge_calibration_times_out_without_attempts) {
  FakePorts ports;
  oc::MouseState s;
  start_calibration(s, ports, 0);
  oc::mouse_tick(s, cal_input(0.0f, 0.0f, 29990), ports);
  CHECK(s.nudge_calibration.running());
  oc::mouse_tick(s, cal_input(0.0f, 0.0f, 30000), ports);
  CHECK(!s.nudge_calibration.running());
  const auto finals = final_reports(ports);
  CHECK_EQ(finals.size(), size_t{1});
  CHECK(finals.front().status == Status::FAILED);
}

TEST(control_nudge_calibration_fails_for_pushes_too_slow_to_tell_from_cursor_motion) {
  FakePorts ports;
  oc::MouseState s;
  start_calibration(s, ports, 0);
  int t = 10;
  // 0.6 over 4 s: even the longest window sees a rise of only about 0.075.
  const Push slow{NudgeDirection::FORWARD, 0.6f, 4000, 100, 100};
  for (const NudgeDirection d : oc::NUDGE_CAL_ORDER) {
    for (int i = 0; i < oc::NUDGE_CAL_REPEATS; ++i) {
      Push p = slow;
      p.dir = d;
      t = drive_push(s, ports, t, p);
    }
  }
  const auto finals = final_reports(ports);
  CHECK_EQ(finals.size(), size_t{1});
  CHECK(finals.front().status == Status::FAILED);
  CHECK(finals.front().message.find("too slow") != std::string::npos);
  CHECK_EQ(finals.front().enter_threshold, 0.0f);
}

TEST(control_nudge_calibration_is_stopped_by_a_recalibration_an_update_or_a_sensor_fault) {
  {
    FakePorts ports;
    oc::MouseState s;
    start_calibration(s, ports, 0);
    oc::mouse_tick(s, input(0.0f, 0.0f, 10, true), ports);
    CHECK(!s.nudge_calibration.running());
    CHECK(final_reports(ports).front().status == Status::CANCELLED);
    CHECK_EQ(ports.count("run_sweep"), 1);
  }
  {
    FakePorts ports;
    oc::MouseState s;
    start_calibration(s, ports, 0);
    oc::StepInput in = cal_input(0.0f, 0.0f, 10);
    in.output_hold = true;
    oc::mouse_tick(s, in, ports);
    CHECK(!s.nudge_calibration.running());
    CHECK(final_reports(ports).front().status == Status::FAILED);
  }
  {
    FakePorts ports;
    oc::MouseState s;
    start_calibration(s, ports, 0);
    for (int i = 1; i <= oc::MOUSE_SENSOR_FAULT_CONFIG.trip_after_failed_reads; ++i) {
      oc::mouse_tick(s, failed_read(0.0f, 0.0f, i * 10), ports);
    }
    CHECK(!s.nudge_calibration.running());
    CHECK_EQ(final_reports(ports).size(), size_t{1});
    CHECK(final_reports(ports).front().status == Status::FAILED);
  }
}

TEST(control_nudge_calibration_start_is_refused_without_a_usable_calibration) {
  FakePorts ports;
  oc::MouseState s;
  oc::StepInput in = cal_input(0.0f, 0.0f, 0);
  in.needs_calibration = true;
  in.nudge_calibration_start = true;
  oc::mouse_tick(s, in, ports);
  CHECK(!s.nudge_calibration.running());
  CHECK_EQ(ports.nudge_reports.size(), size_t{1});
  CHECK(ports.nudge_reports.front().status == Status::FAILED);
}

TEST(control_nudge_calibration_start_is_refused_in_xac_mode) {
  FakePorts ports;
  oc::XacState s;
  oc::StepInput in = cal_input(0.0f, 0.0f, 0);
  in.nudge_calibration_start = true;
  oc::xac_tick(s, in, ports);
  CHECK_EQ(ports.nudge_reports.size(), size_t{1});
  CHECK(ports.nudge_reports.front().status == Status::FAILED);
}

TEST(nudge_calibration_gate_refuses_a_second_start_until_finish) {
  oc::NudgeCalibrationGate gate;
  std::error_code ec;
  CHECK(!gate.busy());
  CHECK(!gate.request_cancel()); // nothing to cancel
  CHECK(gate.request_start(ec));
  CHECK(gate.busy());
  CHECK(!gate.request_start(ec));
  CHECK(ec == std::errc::device_or_resource_busy);
  CHECK(gate.take_start());
  CHECK(!gate.take_start()); // once
  CHECK(gate.request_cancel());
  CHECK(gate.take_cancel());
  CHECK(!gate.take_cancel()); // once
  gate.finish();
  CHECK(!gate.busy());
  ec.clear();
  CHECK(gate.request_start(ec));
}

TEST(nudge_calibration_gate_finish_drops_an_untaken_cancel) {
  oc::NudgeCalibrationGate gate;
  std::error_code ec;
  CHECK(gate.request_start(ec));
  CHECK(gate.take_start());
  CHECK(gate.request_cancel());
  gate.finish();
  CHECK(!gate.take_cancel());
}

// ---- left-click drag --------------------------------------------------------

TEST(control_left_click_does_not_move_the_cursor_before_it_settles) {
  oc::MouseState s = fresh_mouse();
  // Push forward and keep going a little further: the anchor follows.
  for (int t = 100; t < 200; t += 10) {
    const auto out = oc::mouse_step(s, input(0.0f, FORWARD_Y * (0.8f + 0.001f * (t - 100)), t));
    CHECK(s.nudge == NudgeDirection::FORWARD);
    CHECK_EQ(out.mouse.buttons, 0x01);
    CHECK_EQ(out.mouse.dx, 0);
    CHECK_EQ(out.mouse.dy, 0);
  }
}

TEST(control_held_left_click_drags_relative_to_where_it_settled) {
  oc::MouseState s = fresh_mouse();
  oc::mouse_step(s, input(0.0f, FORWARD_Y * 0.9f, 100)); // left click down
  oc::mouse_step(s, input(0.0f, FORWARD_Y * 0.9f, 150));
  // Settled at t = 200 with the stick still held: no creep while it stays put.
  auto out = oc::mouse_step(s, input(0.05f, FORWARD_Y * 0.9f, 200));
  CHECK_EQ(out.mouse.buttons, 0x01);
  CHECK_EQ(out.mouse.dx, 0);
  CHECK_EQ(out.mouse.dy, 0);
  // Steering right while still pushed drags right with the button held.
  out = oc::mouse_step(s, input(0.5f, FORWARD_Y * 0.9f, 210));
  CHECK_EQ(out.mouse.buttons, 0x01);
  CHECK(out.mouse.dx > 0);
  CHECK_EQ(out.mouse.dy, 0);
  // Easing back towards center (still above the 0.8 exit... here 0.85) drags back.
  out = oc::mouse_step(s, input(0.0f, FORWARD_Y * 0.85f, 220));
  CHECK_EQ(out.mouse.buttons, 0x01);
  // Further back, past the exit threshold, lets go.
  out = oc::mouse_step(s, input(0.0f, FORWARD_Y * 0.3f, 230));
  CHECK(s.nudge == NudgeDirection::NONE);
  CHECK_EQ(out.mouse.buttons, 0);
}

TEST(control_held_right_click_and_scroll_still_do_not_move_the_cursor) {
  for (const float x : {0.9f, -0.9f}) {
    oc::MouseState s = fresh_mouse();
    oc::mouse_step(s, input(x, 0.0f, 100));
    const auto out = oc::mouse_step(s, input(x, 0.5f, 300));
    CHECK(s.nudge != NudgeDirection::NONE);
    CHECK_EQ(out.mouse.dx, 0);
    CHECK_EQ(out.mouse.dy, 0);
  }
}
