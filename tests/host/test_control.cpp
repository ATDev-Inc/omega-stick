// components/omega_control: the pure core of main.cpp's mouse and XAC polling
// loops. Time is injected through StepInput::now (a base time_point plus
// offsets) and every hardware effect goes through a fake Ports that records
// the calls in order, so nothing sleeps and nothing touches USB.
//
// Expected values come from the constants in omega_control.hpp and the default
// DeviceSettings / DriftCompensator::Config, not from running the code.

#include <chrono>
#include <cmath>
#include <string>
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
  return in; // default DeviceSettings: 100 ms hold, 0.5 enter, 0.8 exit
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
  bool sweep_ok{true};
  Cal::CenterResult sweep_center;
  Cal::RangeResult sweep_range;
  oc::StickSample post_sweep;
  Clock::time_point clock{};
  Cal::CenterResult last_applied;

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
  oc::StickSample read_stick() override {
    calls.push_back("read_stick");
    return post_sweep;
  }
  oc::Clock::time_point now() override { return clock; }

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

TEST(control_nudge_does_not_enter_before_the_hold_window_elapses) {
  oc::MouseState s = fresh_mouse();
  const auto out = oc::mouse_step(s, input(0.0f, FORWARD_Y * 0.9f, 99));
  CHECK(s.nudge == NudgeDirection::NONE);
  CHECK_EQ(out.mouse.buttons, 0);
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

TEST(control_nudge_exits_only_below_the_exit_threshold_at_a_window_boundary) {
  oc::MouseState s = fresh_mouse();
  enter_nudge(s, 0.0f, FORWARD_Y * 0.9f);
  CHECK(s.nudge == NudgeDirection::FORWARD);

  // Mid-window: even a released stick does not drop the nudge yet.
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
  const auto out = oc::mouse_step(s, input(0.5f, -0.25f, 10));
  CHECK_EQ(out.mouse.dx, expected_delta(0.5f));
  CHECK_EQ(out.mouse.dy, expected_delta(-0.25f));
  CHECK_EQ(out.mouse.wheel, 0);
  CHECK_EQ(out.mouse.buttons, 0);
  CHECK(out.send_report);
}

TEST(control_none_state_motion_clamps_to_127) {
  oc::MouseState s = fresh_mouse();
  const auto out = oc::mouse_step(s, input(10.0f, -10.0f, 10)); // 150 counts before clamping
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

TEST(control_recalibration_resets_the_nudge_window_and_prev_sample) {
  FakePorts ports;
  ports.clock = at(1000);
  ports.post_sweep = {0.0f, FORWARD_Y * 0.9f, 0.0f, 0.0f}; // stick still held after the sweep
  oc::MouseState s;
  oc::mouse_tick(s, input(0.0f, 0.0f, 0), ports);
  oc::mouse_tick(s, input(0.0f, 0.0f, 500, true), ports);
  CHECK(s.window_start == at(1000));
  CHECK_NEAR(s.prev_y, FORWARD_Y * 0.9f, 1e-6);

  // Window not yet elapsed since the post-sweep reference: no entry.
  oc::mouse_step(s, input(0.0f, FORWARD_Y * 0.9f, 1099));
  CHECK(s.nudge == NudgeDirection::NONE);
  // Window elapsed, but the stick has not moved since the post-sweep sample, so
  // the delta is 0. (With a stale prev of 0 this would wrongly enter.)
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
