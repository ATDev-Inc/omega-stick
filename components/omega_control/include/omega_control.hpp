#pragma once

// The pure control core of main.cpp: the mouse and XAC polling-loop bodies,
// nudge-click detection, drift compensation bookkeeping and the recalibration
// hand-off, with no USB, HID, FreeRTOS or ESP dependency. Everything that
// touches hardware sits behind the Ports interface, which main.cpp implements
// (EspPorts) and the host tests fake.
//
// Time is always injected (StepInput::now), so tests drive the nudge timing
// and drift dwell without sleeping.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <system_error>

#include "omega_calibration.hpp"

namespace espp::omega_control {

using Clock = std::chrono::steady_clock;
using CenterResult = espp::OmegaCalibration::CenterResult;
using RangeResult = espp::OmegaCalibration::RangeResult;
using DeviceSettings = espp::OmegaCalibration::DeviceSettings;
using DriftCompensator = espp::OmegaCalibration::DriftCompensator;
using SensorFaultMonitor = espp::OmegaCalibration::SensorFaultMonitor;

// Sensor-fault thresholds (D21 in docs/decisions.md) are counted in ticks, and
// the two firmware loops tick at different rates: the mouse loop sleeps 10 ms
// per iteration, the XAC loop 5 ms. Each loop gets a config that keeps the
// same real-time thresholds: trip after 30 ms of failed reads, call a frozen
// reading stuck after 5 s, and recover after 100 ms of good reads. (The real
// period is the sleep plus the I2C read and HID write, so these are floors.)
inline constexpr SensorFaultMonitor::Config MOUSE_SENSOR_FAULT_CONFIG{
    .trip_after_failed_reads = 3,
    .stuck_after_samples = 500,
    .recover_after_good_reads = 10,
};
inline constexpr SensorFaultMonitor::Config XAC_SENSOR_FAULT_CONFIG{
    .trip_after_failed_reads = 6,
    .stuck_after_samples = 1000,
    .stuck_after_stale_conversions = 100,
    .recover_after_good_reads = 20,
};

// What GET_DIAGNOSTICS reports in its sensor_fault byte before the polling loop
// has seen the sensor answer: the SensorFaultMonitor::Fault codes are 0 none,
// 1 read failed, 2 stuck, and this one means "not observed yet", so the console
// can tell an untested sensor from a healthy one (D22). It is published at boot
// and replaced by 0 once recover_after_good_reads consecutive reads succeed
// (about 100 ms of polling in either loop), or by a fault code if the sensor
// trips first.
inline constexpr std::uint8_t SENSOR_NOT_OBSERVED = 3;

// Scales the normalized joystick output (-1..1) into mouse movement counts
// per polling interval. Lower this to slow the cursor down, raise it to
// speed the cursor up.
inline constexpr float SENSITIVITY = 15.0f;

// ============================================================================
// Nudge click
// ============================================================================
//
//     left    -> scroll up   (held: repeated wheel-up ticks)
//     right   -> right click (held: right button down)
//     forward -> left click  (held: left button down)
//     back    -> scroll down (held: repeated wheel-down ticks)
//
// "Forward" is taken to mean pushing the stick away from the user, which is
// the same direction that already drives the cursor upward on screen, i.e.
// negative js.y(); "back" (pulling the stick toward the user) is positive
// js.y(). If a given build's mechanical/sensor orientation has the Y axis
// flipped, set INVERT_NUDGE_Y to true rather than touching the logic below.
inline constexpr bool INVERT_NUDGE_Y = false;

// The enter threshold (delta), exit threshold (absolute), and nudge time live
// in espp::OmegaCalibration::DeviceSettings (nudge_distance_threshold /
// nudge_exit_threshold / nudge_hold_time) so the calibration console can tune
// them live. They arrive via StepInput::settings.
//
// A nudge is a fast push: the stick's magnitude rises by the enter threshold
// within the nudge time. It is checked on every poll, and the cursor is kept
// still around it in three ways (NudgePhase below):
//
//   PENDING   a fast rise has started and may still become a nudge. Cursor
//             movement is withheld, not sent. If the rise turns out too slow
//             or stops short, the withheld movement is sent after all, so a
//             push that is not a nudge loses nothing, it only starts late (by
//             at most the nudge time).
//   HELD      the nudge is active: button down or wheel ticks, no movement.
//             It lasts at least the nudge time, then ends once the stick is
//             at or below the exit threshold.
//   SETTLING  the nudge has ended but the stick is still on its way back.
//             The cursor stays still until the stick is at rest, or is pushed
//             out again by NUDGE_SETTLE_BREAKOUT (the way out if it never
//             comes to rest).

// A rise slower than this fraction of the slowest possible nudge (enter
// threshold / nudge time) is ordinary cursor movement: it neither starts a
// PENDING gesture nor keeps one alive. Below 1 so that a nudge which starts
// gently, or slows near its peak, is not given up on early.
inline constexpr float NUDGE_PACE_FRACTION = 0.5f;

// How far the stick must be pushed out again, above the lowest magnitude seen
// since the nudge ended, for SETTLING to give the cursor back before the stick
// has come to rest. Large enough that an unsteady hold does not trip it.
inline constexpr float NUDGE_SETTLE_BREAKOUT = 0.2f;

// The nudge time is clamped to this range inside the control loop. The lower
// bound keeps the arithmetic finite. The upper bound limits how long cursor
// movement can be withheld, whatever value was saved (SET_NUDGE_SETTINGS does
// not bound it).
inline constexpr std::chrono::milliseconds NUDGE_MIN_TIME{1};
inline constexpr std::chrono::milliseconds NUDGE_MAX_TIME{1000};

// The polling loop runs every 10ms. Emitting a wheel tick on every single
// poll while a scroll nudge is held would scroll far too fast, so a tick is
// only sent every Nth poll instead.
inline constexpr int SCROLL_POLL_DIVIDER = 5;
inline constexpr std::int8_t SCROLL_STEP = 1;

// Corrects slow center drift while the device runs (see DriftCompensator's
// docs in omega_calibration.hpp). Persisting the drift-corrected calibration
// on every single nudge would wear the flash for no real benefit, so it's
// only saved every SAVE_EVERY_N_NUDGES.
inline constexpr int SAVE_EVERY_N_NUDGES = 20;

enum class NudgeDirection { NONE, LEFT, RIGHT, FORWARD, BACK };

/// Where the nudge gesture is, see the overview above. IDLE is ordinary cursor
/// movement.
enum class NudgePhase { IDLE, PENDING, HELD, SETTLING };

// Classifies a normalized (x, y) stick position into one of the four cardinal
// nudge directions. Only called once the stick's magnitude has already
// crossed the enter threshold, so this only needs to pick a direction, not
// decide whether one is active.
inline NudgeDirection classify_nudge(float x, float y) {
  const float effective_y = INVERT_NUDGE_Y ? -y : y;
  if (std::abs(x) >= std::abs(effective_y)) {
    return (x >= 0.0f) ? NudgeDirection::RIGHT : NudgeDirection::LEFT;
  } else {
    return (effective_y < 0.0f) ? NudgeDirection::FORWARD : NudgeDirection::BACK;
  }
}

// ============================================================================
// Data
// ============================================================================

/// One joystick reading: mapped (calibrated, -1..1) and raw sensor values.
struct StickSample {
  float x{0.0f}, y{0.0f};
  float raw_x{0.0f}, raw_y{0.0f};
  /// False when the sensor read failed. x/y/raw then hold the previous values
  /// (espp::Joystick::update() keeps its old output), so they are not to be
  /// trusted; the SensorFaultMonitor decides when to stop acting on them.
  bool read_ok{true};
  /// The sensor's conversion counter at this read (TMAG5273 CONV_STATUS
  /// SET_COUNT); the fault monitor trips when it stops advancing.
  /// Unset when the producer has no counter (host fakes, tests), in which
  /// case the monitor applies only its other checks.
  std::optional<std::uint8_t> conversion_count{};
};

struct StepInput {
  StickSample stick;
  Clock::time_point now{};
  bool recalibration_requested{false};
  /// D22 rule 1: a firmware update is in flight (or a reboot is pending), so
  /// the output stays neutral. Takes priority over a recalibration request.
  bool output_hold{false};
  /// D26: there is no usable calibration, so the output stays neutral as for
  /// output_hold. Unlike output_hold it lets a recalibration request through:
  /// a successful sweep is what ends it (the caller clears the flag).
  bool needs_calibration{false};
  DeviceSettings settings;
};

/// True when the tick sends a neutral report and does nothing else: a
/// firmware update holds the output (output_hold), or there is no usable
/// calibration and no recalibration is starting on this tick.
inline bool holds_output(const StepInput &in) {
  return in.output_hold || (in.needs_calibration && !in.recalibration_requested);
}

/// Bit 0 = button 1 (left click), bit 1 = button 2 (right click).
struct MouseReport {
  std::uint8_t buttons{0};
  std::int8_t dx{0}, dy{0}, wheel{0};
};

/// Already-normalized gamepad axes, [-1, +1].
struct XacReport {
  float x{0.0f}, y{0.0f};
};

struct StepOutput {
  bool send_report{false};
  MouseReport mouse;
  XacReport xac;
  bool run_recalibration{false}; ///< Caller must run the sweep, see *_tick().
  bool drift_applied{false};     ///< center was nudged; push it into the joystick.
  bool save_calibration{false};  ///< Persist the drift-corrected calibration.
};

struct CommonState {
  CenterResult center;
  RangeResult range;
  DriftCompensator drift;
  int drift_nudge_count{0};
  SensorFaultMonitor sensor;
  bool sensor_was_faulted{false};
  /// Consecutive good reads since the last failed one, and whether
  /// Ports::sensor_observed_healthy() has been called (once per boot).
  int good_reads{0};
  int observe_after_good_reads{10};
  bool sensor_observed{false};

  CommonState() = default;
  explicit CommonState(const SensorFaultMonitor::Config &sensor_config)
      : sensor(sensor_config)
      , observe_after_good_reads(sensor_config.recover_after_good_reads) {}
};

struct MouseState {
  CommonState common{MOUSE_SENSOR_FAULT_CONFIG};
  // Current nudge-click state. This persists across polling iterations so a
  // held nudge stays held. `nudge` is a direction exactly while phase is HELD.
  NudgeDirection nudge{NudgeDirection::NONE};
  NudgePhase phase{NudgePhase::IDLE};
  int scroll_poll_count{0};
  // The previous poll's magnitude and time: a rise is measured against it.
  float last_mag{0.0f};
  Clock::time_point last_time{};
  // PENDING: the magnitude and time the rise started from, and whether the
  // previous poll already saw the full enter threshold (a nudge needs two
  // polls in a row, so one bad sample cannot click).
  float pending_base{0.0f};
  Clock::time_point pending_start{};
  bool pending_reached{false};
  // Cursor movement withheld while PENDING, and what is left to send of it
  // once the gesture turned out not to be a nudge.
  std::int32_t owed_dx{0}, owed_dy{0};
  // HELD: when the nudge started.
  Clock::time_point nudge_start{};
  // SETTLING: the lowest magnitude seen since the nudge ended.
  float settle_min{0.0f};
  bool initialised{false};
};

struct XacState {
  CommonState common{XAC_SENSOR_FAULT_CONFIG};
};

// ============================================================================
// Pure steps
// ============================================================================

namespace detail {

inline std::int8_t to_delta(float v) {
  // Truncate toward zero, then clamp to [-127, 127] (what
  // MouseInputReport::set_movement does with the same value).
  const int i = static_cast<int>(v);
  if (i > 127) {
    return 127;
  }
  if (i < -127) {
    return -127;
  }
  return static_cast<std::int8_t>(i);
}

// Shared by both engines: feed the PRE-drift sample to the compensator,
// count nudges, and ask for a flash save every SAVE_EVERY_N_NUDGES.
inline void drift_step(CommonState &common, const StepInput &in, float mapped_magnitude,
                       StepOutput &out) {
  if (common.drift.update(in.stick.raw_x, in.stick.raw_y, mapped_magnitude, common.center,
                          in.now)) {
    out.drift_applied = true;
    common.drift_nudge_count++;
    if (common.drift_nudge_count % SAVE_EVERY_N_NUDGES == 0) {
      out.save_calibration = true;
    }
  }
}

// A recalibration request pre-empts the tick: neutral report, no nudge or
// drift processing.
inline StepOutput recalibration_output() {
  StepOutput out;
  out.send_report = true;
  out.run_recalibration = true;
  return out;
}

inline std::int8_t clamp_delta(std::int32_t v) {
  return static_cast<std::int8_t>(v > 127 ? 127 : (v < -127 ? -127 : v));
}

// Drops any gesture in progress (a held nudge, withheld movement) and restarts
// the rise measurement from this sample, so the next poll cannot see a rise
// against a reading from before a sweep, a fault or an output hold.
inline void reset_gesture(MouseState &s, float x, float y, Clock::time_point now) {
  s.nudge = NudgeDirection::NONE;
  s.phase = NudgePhase::IDLE;
  s.scroll_poll_count = 0;
  s.pending_reached = false;
  s.owed_dx = 0;
  s.owed_dy = 0;
  s.last_mag = std::hypot(x, y);
  s.last_time = now;
  s.initialised = true;
}

// Advances the nudge gesture by one poll. `at_rest` is true when the stick
// would not move the cursor at all on this poll.
inline void nudge_update(MouseState &s, const StepInput &in, float cur_mag, bool at_rest) {
  using FloatMs = std::chrono::duration<float, std::milli>;
  const auto &settings = in.settings;
  const auto nudge_time = std::clamp(settings.nudge_hold_time, NUDGE_MIN_TIME, NUDGE_MAX_TIME);
  const float nudge_time_ms = FloatMs(nudge_time).count();
  const float threshold = settings.nudge_distance_threshold;
  // Magnitude per millisecond: slower than this is ordinary movement.
  const float pace = NUDGE_PACE_FRACTION * threshold / nudge_time_ms;

  if (s.phase == NudgePhase::HELD) {
    // Not checked before the nudge time has passed: the stick is usually still
    // rising when the nudge starts, and a click must not be shorter than this.
    if (in.now - s.nudge_start >= nudge_time && cur_mag <= settings.nudge_exit_threshold) {
      s.nudge = NudgeDirection::NONE;
      s.phase = at_rest ? NudgePhase::IDLE : NudgePhase::SETTLING;
      s.settle_min = cur_mag;
    }
    return;
  }

  if (s.phase == NudgePhase::SETTLING) {
    s.settle_min = std::min(s.settle_min, cur_mag);
    if (!at_rest && cur_mag - s.settle_min < NUDGE_SETTLE_BREAKOUT) {
      return;
    }
    // At rest, or pushed out again: this poll is handled as IDLE, so a second
    // quick nudge goes straight to PENDING without moving the cursor first.
    s.phase = NudgePhase::IDLE;
  }

  if (s.phase == NudgePhase::IDLE) {
    const float step = cur_mag - s.last_mag;
    const float dt_ms = FloatMs(in.now - s.last_time).count();
    if (!(step > 0.0f) || !(dt_ms > 0.0f) || step < pace * dt_ms) {
      return;
    }
    s.phase = NudgePhase::PENDING;
    s.pending_base = s.last_mag;
    s.pending_start = s.last_time;
    s.pending_reached = false;
  }

  // PENDING.
  const float rise = cur_mag - s.pending_base;
  const float elapsed_ms = FloatMs(in.now - s.pending_start).count();
  if (rise >= threshold) {
    if (s.pending_reached) {
      s.nudge = classify_nudge(in.stick.x, in.stick.y);
      s.phase = NudgePhase::HELD;
      s.scroll_poll_count = 0;
      s.nudge_start = in.now;
      s.pending_reached = false;
      // The withheld movement was the nudge itself: it is never sent.
      s.owed_dx = 0;
      s.owed_dy = 0;
      return;
    }
    if (elapsed_ms <= nudge_time_ms) {
      s.pending_reached = true;
      return;
    }
  } else {
    s.pending_reached = false;
    if (elapsed_ms < nudge_time_ms && rise >= pace * elapsed_ms) {
      return; // still rising fast enough to become a nudge
    }
  }
  // Too slow, too late, or it stopped short: ordinary movement after all. The
  // withheld movement stays in owed_dx/owed_dy and goes out from this poll on.
  s.phase = NudgePhase::IDLE;
  s.pending_reached = false;
}

} // namespace detail

inline StepOutput mouse_step(MouseState &s, const StepInput &in) {
  const float jx = in.stick.x;
  const float jy = in.stick.y;
  if (in.recalibration_requested) {
    // Deliberate behaviour change: a neutral report goes out before every
    // recalibration, so a nudge-held button is released instead of staying
    // down for the whole sweep.
    detail::reset_gesture(s, jx, jy, in.now);
    return detail::recalibration_output();
  }

  if (!s.initialised) {
    detail::reset_gesture(s, jx, jy, in.now);
  }

  const float cur_mag = std::hypot(jx, jy);
  const std::int8_t stick_dx = detail::to_delta(jx * SENSITIVITY);
  const std::int8_t stick_dy = detail::to_delta(jy * SENSITIVITY);

  // Nudge tuning is read live on every poll, so a console edit
  // (SET_NUDGE_SETTINGS) takes effect at once without needing a restart.
  if (in.settings.nudge_enabled) {
    // A failed read repeats the previous sample (see StickSample::read_ok), so
    // it says nothing new about the gesture: it must not start a nudge, nor be
    // the second poll that confirms one. The sensor monitor deals with it.
    if (in.stick.read_ok) {
      detail::nudge_update(s, in, cur_mag, stick_dx == 0 && stick_dy == 0);
      s.last_mag = cur_mag;
      s.last_time = in.now;
    }
  } else {
    // With nudge clicks switched off (SET_NUDGE_ENABLED), a held nudge is
    // released on this very tick, so a held button or scroll never outlives
    // the switch, and the cursor follows the stick again at once (movement a
    // PENDING gesture was withholding is sent, not dropped). The rise
    // baseline follows the stick every tick while off, so switching back on
    // measures only movement made after the switch: a push made while off can
    // never turn into a click.
    s.nudge = NudgeDirection::NONE;
    s.phase = NudgePhase::IDLE;
    s.scroll_poll_count = 0;
    s.pending_reached = false;
    s.last_mag = cur_mag;
    s.last_time = in.now;
  }

  // --- Per-report mouse update: runs every 10ms iteration ---
  StepOutput out;
  out.send_report = true;
  MouseReport &mouse = out.mouse;

  switch (s.phase) {
  case NudgePhase::IDLE: {
    // The stick's movement, plus whatever a gesture that was not a nudge
    // withheld (as much as one report holds; the rest follows).
    const std::int32_t want_dx = s.owed_dx + stick_dx;
    const std::int32_t want_dy = s.owed_dy + stick_dy;
    mouse.dx = detail::clamp_delta(want_dx);
    mouse.dy = detail::clamp_delta(want_dy);
    s.owed_dx = want_dx - mouse.dx;
    s.owed_dy = want_dy - mouse.dy;
    break;
  }
  case NudgePhase::PENDING:
    s.owed_dx += stick_dx;
    s.owed_dy += stick_dy;
    break;
  case NudgePhase::HELD:
    mouse.buttons = static_cast<std::uint8_t>((s.nudge == NudgeDirection::FORWARD ? 0x01 : 0) |
                                              (s.nudge == NudgeDirection::RIGHT ? 0x02 : 0));
    if (s.nudge == NudgeDirection::LEFT || s.nudge == NudgeDirection::BACK) {
      s.scroll_poll_count++;
      if (s.scroll_poll_count % SCROLL_POLL_DIVIDER == 0) {
        mouse.wheel = s.nudge == NudgeDirection::LEFT ? SCROLL_STEP : -SCROLL_STEP;
      }
    }
    break;
  case NudgePhase::SETTLING:
    break;
  }

  detail::drift_step(s.common, in, cur_mag, out);
  return out;
}

inline StepOutput xac_step(XacState &s, const StepInput &in) {
  if (in.recalibration_requested) {
    // Same neutral-report-first behaviour as mouse_step.
    return detail::recalibration_output();
  }

  // XAC mode has no nudge clicks, so DeviceSettings::nudge_enabled (and the
  // nudge tuning) does not affect it: the axes always follow the stick.
  StepOutput out;
  out.send_report = true;
  // IMPORTANT:
  // Gamepad/XAC axes are already normalized [-1, +1].
  out.xac.x = in.stick.x;
  out.xac.y = in.stick.y;

  detail::drift_step(s.common, in, std::hypot(in.stick.x, in.stick.y), out);
  return out;
}

/// Folds a finished recalibration sweep back into the state. `new_center` and
/// `new_range` must already be finalised (manual offset re-applied,
/// finalize_center_deadzone run). Always resets the nudge/timing bookkeeping
/// so the next tick does not see a bogus giant delta, and (deliberate
/// behaviour change) resets the drift compensator, whose idle timer used to
/// span the whole sweep. `mouse` may be null for the XAC engine.
inline void on_recalibration_done(CommonState &common, MouseState *mouse,
                                  const StickSample &post_sweep, Clock::time_point now, bool ok,
                                  const CenterResult &new_center, const RangeResult &new_range) {
  if (ok) {
    common.center = new_center;
    common.range = new_range;
  }
  common.drift.reset();
  if (mouse) {
    detail::reset_gesture(*mouse, post_sweep.x, post_sweep.y, now);
  }
}

// ============================================================================
// Ports and ticks
// ============================================================================

/// Everything the control loops need from the outside world.
struct Ports {
  virtual void send_mouse(const MouseReport &report) = 0;
  virtual void send_xac(const XacReport &report) = 0;
  /// Runs the interactive sweep. Called with the current calibration in
  /// new_center/new_range; on success they hold the finalised new results
  /// (the adapter takes the live manual offset from the store).
  virtual bool run_sweep(CenterResult &new_center, RangeResult &new_range) = 0;
  /// Persists the store to flash (drift-corrected calibration).
  virtual void save_to_flash() = 0;
  /// Pushes a drift-nudged calibration into the store and the live joystick.
  /// The store keeps its own live manual offset (the console may have changed
  /// it since this copy was taken), so the adapter writes the store's center
  /// back into `center`.
  virtual void apply_drift(CenterResult &center, const RangeResult &range) = 0;
  /// Fresh joystick reading (used after a sweep).
  virtual StickSample read_stick() = 0;
  virtual Clock::time_point now() = 0;
  /// Called once when the sensor monitor trips (`faulted`, with the fault
  /// kind) and once when it recovers. `fault_count` is the trips since boot.
  virtual void sensor_fault_changed(bool faulted, SensorFaultMonitor::Fault fault,
                                    std::uint32_t fault_count) = 0;
  /// Called once, the first time the sensor has answered
  /// `recover_after_good_reads` polls in a row without a fault in effect. Until
  /// then the published sensor status is SENSOR_NOT_OBSERVED, not "healthy".
  virtual void sensor_observed_healthy() = 0;

protected:
  ~Ports() = default;
};

namespace detail {

struct FaultEdge {
  bool faulted{false};
  bool tripped{false};   ///< entered the fault on this tick
  bool recovered{false}; ///< left the fault on this tick
};

// Feeds the sensor monitor and reports the transition through the ports.
// Called first on every tick, whatever else the tick does.
inline FaultEdge sensor_fault_update(CommonState &common, const StepInput &in, Ports &ports) {
  FaultEdge edge;
  edge.faulted = common.sensor.update(in.stick.read_ok, in.stick.raw_x, in.stick.raw_y,
                                      in.stick.conversion_count);
  if (edge.faulted != common.sensor_was_faulted) {
    common.sensor_was_faulted = edge.faulted;
    edge.tripped = edge.faulted;
    edge.recovered = !edge.faulted;
    ports.sensor_fault_changed(edge.faulted, common.sensor.fault(), common.sensor.fault_count());
  }
  common.good_reads = in.stick.read_ok ? common.good_reads + 1 : 0;
  if (!common.sensor_observed && !edge.faulted &&
      common.good_reads >= common.observe_after_good_reads) {
    common.sensor_observed = true;
    ports.sensor_observed_healthy();
  }
  return edge;
}

} // namespace detail

inline void mouse_tick(MouseState &s, const StepInput &in, Ports &ports) {
  if (holds_output(in)) {
    // Neutral report every tick while a firmware update runs or there is no
    // usable calibration, with the same bookkeeping reset as a sensor
    // recovery so releasing the hold does not see a delta against a reading
    // from before it. The sensor monitor keeps running; during an update a
    // pending recalibration waits (the caller only consumes it when not
    // holding).
    detail::sensor_fault_update(s.common, in, ports);
    detail::reset_gesture(s, in.stick.x, in.stick.y, in.now);
    ports.send_mouse(MouseReport{});
    return;
  }
  // While the sensor is faulted the stick is reported centered: neutral
  // report, no nudge detection, no drift correction. A pending recalibration
  // still runs (it reads the sensor itself and reports its own failure).
  const auto fault = detail::sensor_fault_update(s.common, in, ports);
  if (fault.tripped || fault.recovered) {
    // Tripped: a held nudge is released and withheld movement dropped.
    // Recovered: same reset as after a recalibration, so the next poll does
    // not see a rise against a reading from before the fault.
    detail::reset_gesture(s, in.stick.x, in.stick.y, in.now);
  }
  if (fault.faulted && !in.recalibration_requested) {
    ports.send_mouse(MouseReport{});
    return;
  }

  const StepOutput out = mouse_step(s, in);
  if (out.send_report) {
    ports.send_mouse(out.mouse);
  }
  if (out.run_recalibration) {
    CenterResult new_center = s.common.center;
    RangeResult new_range = s.common.range;
    const bool ok = ports.run_sweep(new_center, new_range);
    const StickSample post = ports.read_stick();
    on_recalibration_done(s.common, &s, post, ports.now(), ok, new_center, new_range);
    ports.send_mouse(MouseReport{});
    return;
  }
  if (out.drift_applied) {
    ports.apply_drift(s.common.center, s.common.range);
  }
  if (out.save_calibration) {
    ports.save_to_flash();
  }
}

inline void xac_tick(XacState &s, const StepInput &in, Ports &ports) {
  if (holds_output(in)) {
    // See mouse_tick: neutral report every tick while a firmware update runs
    // or there is no usable calibration.
    detail::sensor_fault_update(s.common, in, ports);
    ports.send_xac(XacReport{});
    return;
  }
  // See mouse_tick: a faulted sensor reports a centered stick.
  const auto fault = detail::sensor_fault_update(s.common, in, ports);
  if (fault.faulted && !in.recalibration_requested) {
    ports.send_xac(XacReport{});
    return;
  }

  const StepOutput out = xac_step(s, in);
  if (out.send_report) {
    ports.send_xac(out.xac);
  }
  if (out.run_recalibration) {
    CenterResult new_center = s.common.center;
    RangeResult new_range = s.common.range;
    const bool ok = ports.run_sweep(new_center, new_range);
    const StickSample post = ports.read_stick();
    on_recalibration_done(s.common, nullptr, post, ports.now(), ok, new_center, new_range);
    ports.send_xac(XacReport{});
    return;
  }
  if (out.drift_applied) {
    ports.apply_drift(s.common.center, s.common.range);
  }
  if (out.save_calibration) {
    ports.save_to_flash();
  }
}

// ============================================================================
// Recalibration hand-off
// ============================================================================

/// Cross-task hand-off for "recalibrate now". The calibration service calls
/// request() from the USB task; the polling loop calls take() once per
/// iteration, runs the sweep inside the tick if it returned true, and calls
/// finish() after the tick returns.
///
/// The gate stays busy from the moment a request is accepted until finish(),
/// so a second request that arrives while the sweep is running is refused
/// (device_or_resource_busy) instead of queueing another full sweep behind it.
///
/// It also carries FINISH_CALIBRATION ("end the range sweep now"): the
/// service calls request_finish(), which only succeeds while the sweep's
/// finish window is open (sweep_stage() opens it for the range stage), and
/// the sweep polls take_finish(). All of it is lock-free atomics.
class RecalibrationGate {
public:
  /// Accepts a request if none is pending or running. Otherwise sets ec to
  /// device_or_resource_busy and returns false.
  bool request(std::error_code &ec) {
    State expected = State::Idle;
    if (!state_.compare_exchange_strong(expected, State::Requested)) {
      ec = std::make_error_code(std::errc::device_or_resource_busy);
      return false;
    }
    return true;
  }

  /// True exactly once per accepted request: moves it to running.
  bool take() {
    State expected = State::Requested;
    return state_.compare_exchange_strong(expected, State::Running);
  }

  /// Marks the sweep done (or abandoned) so a new request is accepted.
  /// Also closes the finish window and drops an unconsumed finish request.
  void finish() {
    set_finish_window(false);
    state_.store(State::Idle);
  }

  /// Opened by the sweep (its progress callback) while the range stage is
  /// sampling, the only time a finish request can take effect, and closed
  /// when sampling stops. Either edge drops an unconsumed finish request, so
  /// one can never leak into a later sweep.
  void set_finish_window(bool open) {
    finish_requested_.store(false);
    finish_window_.store(open);
  }

  /// Called by the sweep's progress callback with each report's stage: opens
  /// the finish window when the range sweep starts sampling and closes it
  /// when sampling ends. Acts on changes only, so a finish request that lands
  /// between two reports of the same stage is kept for take_finish().
  void sweep_stage(espp::OmegaCalibration::Stage stage) {
    const bool sampling = stage == espp::OmegaCalibration::Stage::RANGE_SWEEP;
    if (sampling != finish_window_.load()) {
      set_finish_window(sampling);
    }
  }

  /// FINISH_CALIBRATION from the calibration service (USB task): accepted
  /// only while a sweep is running and its finish window is open.
  bool request_finish() {
    if (state_.load() != State::Running || !finish_window_.load()) {
      return false;
    }
    finish_requested_.store(true);
    return true;
  }

  /// Polled by the sweep (OmegaCalibration::Config::finish_requested): true
  /// once per accepted finish request.
  bool take_finish() { return finish_requested_.exchange(false); }

private:
  enum class State : uint8_t { Idle, Requested, Running };
  std::atomic<State> state_{State::Idle};
  std::atomic<bool> finish_window_{false};
  std::atomic<bool> finish_requested_{false};
};

} // namespace espp::omega_control
