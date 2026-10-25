#pragma once

// The pure control core of main.cpp: the mouse and XAC polling-loop bodies,
// nudge-click detection, drift compensation bookkeeping and the recalibration
// hand-off, with no USB, HID, FreeRTOS or ESP dependency. Everything that
// touches hardware sits behind the Ports interface, which main.cpp implements
// (EspPorts) and the host tests fake.
//
// Time is always injected (StepInput::now), so tests drive the nudge window
// and drift dwell without sleeping.

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

// The enter threshold (delta), exit threshold (absolute), and measurement
// time window live in espp::OmegaCalibration::DeviceSettings
// (nudge_distance_threshold / nudge_exit_threshold / nudge_hold_time) so the
// calibration console can tune them live. They arrive via StepInput::settings.

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
  // held nudge stays held.
  NudgeDirection nudge{NudgeDirection::NONE};
  int scroll_poll_count{0};
  Clock::time_point window_start{};
  float prev_x{0.0f}, prev_y{0.0f};
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

} // namespace detail

inline StepOutput mouse_step(MouseState &s, const StepInput &in) {
  if (in.recalibration_requested) {
    // Deliberate behaviour change: a neutral report goes out before every
    // recalibration, so a nudge-held button is released instead of staying
    // down for the whole sweep.
    s.nudge = NudgeDirection::NONE;
    s.scroll_poll_count = 0;
    return detail::recalibration_output();
  }

  const float jx = in.stick.x;
  const float jy = in.stick.y;
  if (!s.initialised) {
    s.window_start = in.now;
    s.prev_x = jx;
    s.prev_y = jy;
    s.initialised = true;
  }

  const auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(in.now - s.window_start);
  const float cur_mag = std::hypot(jx, jy);

  // Nudge tuning is read live each measurement window, so a console edit
  // (SET_NUDGE_SETTINGS) takes effect on the very next window without needing
  // a restart.
  const auto &nudge_settings = in.settings;

  // --- Nudge direction detection only (runs every nudge_hold_time window) ---
  if (duration >= nudge_settings.nudge_hold_time) {
    const float prev_mag = std::hypot(s.prev_x, s.prev_y);
    const float mapped_magnitude = cur_mag - prev_mag;

    if (s.nudge == NudgeDirection::NONE) {
      if (mapped_magnitude >= nudge_settings.nudge_distance_threshold) {
        s.nudge = classify_nudge(jx, jy);
        s.scroll_poll_count = 0;
      }
    } else if (cur_mag <= nudge_settings.nudge_exit_threshold) {
      s.nudge = NudgeDirection::NONE;
    }

    s.prev_x = jx;
    s.prev_y = jy;
    s.window_start = in.now;
  }

  // --- Per-report mouse update: runs every 10ms iteration ---
  StepOutput out;
  out.send_report = true;
  MouseReport &mouse = out.mouse;
  mouse.buttons = static_cast<std::uint8_t>((s.nudge == NudgeDirection::FORWARD ? 0x01 : 0) |
                                            (s.nudge == NudgeDirection::RIGHT ? 0x02 : 0));

  switch (s.nudge) {
  case NudgeDirection::NONE:
    mouse.dx = detail::to_delta(jx * SENSITIVITY);
    mouse.dy = detail::to_delta(jy * SENSITIVITY);
    break;
  case NudgeDirection::LEFT:
    s.scroll_poll_count++;
    if (s.scroll_poll_count % SCROLL_POLL_DIVIDER == 0)
      mouse.wheel = SCROLL_STEP;
    break;
  case NudgeDirection::BACK:
    s.scroll_poll_count++;
    if (s.scroll_poll_count % SCROLL_POLL_DIVIDER == 0)
      mouse.wheel = -SCROLL_STEP;
    break;
  case NudgeDirection::FORWARD:
  case NudgeDirection::RIGHT:
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
    mouse->nudge = NudgeDirection::NONE;
    mouse->scroll_poll_count = 0;
    mouse->window_start = now;
    mouse->prev_x = post_sweep.x;
    mouse->prev_y = post_sweep.y;
    mouse->initialised = true;
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
    s.nudge = NudgeDirection::NONE;
    s.scroll_poll_count = 0;
    s.window_start = in.now;
    s.prev_x = in.stick.x;
    s.prev_y = in.stick.y;
    s.initialised = true;
    ports.send_mouse(MouseReport{});
    return;
  }
  // While the sensor is faulted the stick is reported centered: neutral
  // report, no nudge detection, no drift correction. A pending recalibration
  // still runs (it reads the sensor itself and reports its own failure).
  const auto fault = detail::sensor_fault_update(s.common, in, ports);
  if (fault.tripped) {
    s.nudge = NudgeDirection::NONE;
    s.scroll_poll_count = 0;
  }
  if (fault.recovered) {
    // Same reset as after a recalibration: the next window must not see a
    // delta against a reading from before the fault.
    s.window_start = in.now;
    s.prev_x = in.stick.x;
    s.prev_y = in.stick.y;
    s.initialised = true;
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
  void finish() { state_.store(State::Idle); }

private:
  enum class State : uint8_t { Idle, Requested, Running };
  std::atomic<State> state_{State::Idle};
};

} // namespace espp::omega_control
