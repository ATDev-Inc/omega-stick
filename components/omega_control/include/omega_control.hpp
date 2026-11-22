#pragma once

// The pure control core of main.cpp: the mouse and XAC polling-loop bodies,
// nudge-click detection, drift compensation bookkeeping and the recalibration
// hand-off, with no USB, HID, FreeRTOS or ESP dependency. Everything that
// touches hardware sits behind the Ports interface, which main.cpp implements
// (EspPorts) and the host tests fake.
//
// Time is always injected (StepInput::now), so tests drive the nudge window
// and drift dwell without sleeping.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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
//
// Detection is checked on every poll: a nudge starts when the magnitude has
// risen by at least the enter threshold over the trailing time window (the
// current magnitude minus the lowest one in the last nudge_hold_time). A
// trailing window cannot split a push in two, so the whole rise counts
// however the push lines up with the polls. A nudge is held for at least
// nudge_hold_time and then released as soon as the magnitude is at or below
// the exit threshold.
//
// A held left click (forward) drags: once it has been held for
// nudge_hold_time, the stick moves the cursor relative to where it was then
// (the anchor), so pushing, holding and steering highlights text, and easing
// back towards center past the exit threshold lets go. Until then the anchor
// follows the stick, so a plain push and release clicks without moving the
// cursor. DRAG_DEADZONE keeps a hand holding the push still from creeping.
inline constexpr float DRAG_DEADZONE = 0.1f;

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

/// The values are also the NUDGE_CALIBRATION frame's direction bytes
/// (OmegaCalibration::NudgeCalibrationReport), so never renumber them.
enum class NudgeDirection : std::uint8_t { NONE = 0, LEFT = 1, RIGHT = 2, FORWARD = 3, BACK = 4 };

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
// Nudge calibration
// ============================================================================
//
// Works the three nudge settings (DeviceSettings::nudge_distance_threshold,
// nudge_exit_threshold and nudge_hold_time) out of the person's own nudges
// instead of assuming how far or how fast they push. The console asks for
// NUDGE_CAL_REPEATS nudges in each direction of NUDGE_CAL_ORDER. While it
// runs, mouse_tick() sends neutral reports and feeds every mapped sample to a
// NudgeCalibrator, so the nudges neither move the cursor nor click.
//
// An attempt is the stick leaving rest (magnitude above
// NUDGE_CAL_REST_MAGNITUDE, after resting for NUDGE_CAL_REST_HOLD) and coming
// back to rest. It counts when its peak reached NUDGE_CAL_MIN_PEAK in the
// direction asked for (classify_nudge() at the peak).
//
// Each counted nudge, with the rest just before it, is then measured the way
// mouse_step()'s trailing-window detector sees it (window_rise()):
//  - nudge_hold_time is the shortest window from NUDGE_CAL_MIN_WINDOW to
//    NUDGE_CAL_MAX_WINDOW whose median nudge rises within
//    NUDGE_CAL_WINDOW_FRACTION of what the longest window sees. A longer
//    window than that only delays the click.
//  - nudge_distance_threshold is NUDGE_CAL_ENTER_MARGIN of the median nudge's
//    rise at that window: the person's typical push clicks with room to
//    spare, and ordinary cursor movement smaller than it does not. It is
//    deliberately not capped by the weakest push, so one gentle push cannot
//    pull the threshold down for every direction; the price is that a push
//    much weaker than the typical one (say in a weak direction) does not
//    click. Below NUDGE_CAL_MIN_ENTER the calibration fails instead of setting
//    a threshold ordinary cursor movement could cross.
//  - nudge_exit_threshold is NUDGE_CAL_EXIT_FRACTION of the smallest peak, so a
//    nudge stays held while the stick is pushed and lets go on the way back.

inline constexpr int NUDGE_CAL_REPEATS = 3;
inline constexpr std::array<NudgeDirection, 4> NUDGE_CAL_ORDER{
    NudgeDirection::FORWARD, NudgeDirection::RIGHT, NudgeDirection::BACK, NudgeDirection::LEFT};
inline constexpr int NUDGE_CAL_TOTAL = NUDGE_CAL_REPEATS * static_cast<int>(NUDGE_CAL_ORDER.size());
inline constexpr float NUDGE_CAL_REST_MAGNITUDE = 0.1f;
inline constexpr std::chrono::milliseconds NUDGE_CAL_REST_HOLD{300};
inline constexpr float NUDGE_CAL_MIN_PEAK = 0.25f;
/// Samples kept per attempt: 2 s of the 10 ms loop, far more than the push
/// itself. A longer hold keeps its peak but records no more. The rest before
/// the push (up to NUDGE_CAL_MAX_WINDOW of it) comes on top.
inline constexpr std::size_t NUDGE_CAL_MAX_SAMPLES = 200;
/// No attempt finished for this long ends the calibration (the console may be
/// gone, and the output is neutral meanwhile).
inline constexpr std::chrono::seconds NUDGE_CAL_IDLE_TIMEOUT{30};
inline constexpr std::chrono::milliseconds NUDGE_CAL_MIN_WINDOW{50};
inline constexpr std::chrono::milliseconds NUDGE_CAL_MAX_WINDOW{500};
inline constexpr std::chrono::milliseconds NUDGE_CAL_WINDOW_STEP{10};
inline constexpr float NUDGE_CAL_WINDOW_FRACTION = 0.95f;
inline constexpr float NUDGE_CAL_ENTER_MARGIN = 0.8f;
inline constexpr float NUDGE_CAL_MIN_ENTER = 0.1f;
inline constexpr float NUDGE_CAL_EXIT_FRACTION = 0.5f;
inline constexpr float NUDGE_CAL_MAX_EXIT = 0.95f;
/// How often a running calibration reports even when nothing changed (for
/// the console's live push meter).
inline constexpr std::chrono::milliseconds NUDGE_CAL_REPORT_PERIOD{50};

/// One mapped magnitude in an attempt, `t_ms` after the push left rest
/// (negative for the rest recorded before it).
struct NudgeSample {
  std::int32_t t_ms{0};
  float magnitude{0.0f};
};

/// A counted nudge: its trace starts with the rest recorded before it.
struct RecordedNudge {
  NudgeDirection direction{NudgeDirection::NONE};
  float peak{0.0f};
  std::vector<NudgeSample> trace;
};

struct NudgeTuning {
  float enter_threshold{0.0f};
  float exit_threshold{0.0f};
  std::chrono::milliseconds hold_time{0};
};

/// The largest rise mouse_step()'s detector would see in `trace` with this
/// window: the most any sample exceeds the lowest one within `window` before
/// it (itself included). Samples before trace[0] count as trace[0], so a trace
/// should start with at least `window` of the rest before the push.
inline float window_rise(const std::vector<NudgeSample> &trace, std::chrono::milliseconds window) {
  if (trace.empty()) {
    return 0.0f;
  }
  const auto window_ms = static_cast<std::int32_t>(window.count());
  std::deque<std::size_t> lows; // indices, magnitudes increasing: lows.front() is the minimum
  float best = 0.0f;
  for (std::size_t i = 0; i < trace.size(); ++i) {
    while (!lows.empty() && trace[lows.back()].magnitude >= trace[i].magnitude) {
      lows.pop_back();
    }
    lows.push_back(i);
    while (trace[i].t_ms - trace[lows.front()].t_ms > window_ms) {
      lows.pop_front();
    }
    best = std::max(best, trace[i].magnitude - trace[lows.front()].magnitude);
  }
  return best;
}

/// The median of `values` (the mean of the middle two for an even count).
/// Reorders `values`.
inline float median_of(std::vector<float> &values) {
  if (values.empty()) {
    return 0.0f;
  }
  std::sort(values.begin(), values.end());
  const std::size_t mid = values.size() / 2;
  return values.size() % 2 ? values[mid] : 0.5f * (values[mid - 1] + values[mid]);
}

/// Works the nudge settings out of the counted nudges (see the section
/// comment). Fails with invalid_argument for no nudges, and with
/// result_out_of_range when the nudges are too small or too slow for an enter
/// threshold of at least NUDGE_CAL_MIN_ENTER.
inline bool compute_nudge_tuning(const std::vector<RecordedNudge> &nudges, NudgeTuning &out,
                                 std::error_code &ec) {
  ec.clear();
  if (nudges.empty()) {
    ec = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
  struct Candidate {
    std::chrono::milliseconds window;
    float median;
  };
  std::vector<Candidate> candidates;
  std::vector<float> rises(nudges.size());
  for (auto window = NUDGE_CAL_MIN_WINDOW; window <= NUDGE_CAL_MAX_WINDOW;
       window += NUDGE_CAL_WINDOW_STEP) {
    for (std::size_t i = 0; i < nudges.size(); ++i) {
      rises[i] = window_rise(nudges[i].trace, window);
    }
    candidates.push_back({window, median_of(rises)});
  }
  if (!(candidates.back().median > 0.0f)) {
    ec = std::make_error_code(std::errc::result_out_of_range);
    return false;
  }
  const Candidate *chosen = &candidates.back();
  for (const auto &c : candidates) {
    if (c.median >= NUDGE_CAL_WINDOW_FRACTION * candidates.back().median) {
      chosen = &c;
      break;
    }
  }
  const float enter = NUDGE_CAL_ENTER_MARGIN * chosen->median;
  if (!(enter >= NUDGE_CAL_MIN_ENTER)) {
    ec = std::make_error_code(std::errc::result_out_of_range);
    return false;
  }
  float min_peak = std::numeric_limits<float>::infinity();
  for (const auto &nudge : nudges) {
    min_peak = std::min(min_peak, nudge.peak);
  }
  out.enter_threshold = std::min(enter, 1.0f);
  out.exit_threshold =
      std::clamp(NUDGE_CAL_EXIT_FRACTION * min_peak, NUDGE_CAL_REST_MAGNITUDE, NUDGE_CAL_MAX_EXIT);
  out.hold_time = chosen->window;
  return true;
}

/// Records the attempts of one nudge calibration and works out the settings
/// at the end (see the section comment). Pure: the polling loop feeds it the
/// mapped stick and the time, and sends what it reports.
class NudgeCalibrator {
public:
  using Report = OmegaCalibration::NudgeCalibrationReport;
  using Status = Report::Status;
  using Attempt = Report::Attempt;

  bool running() const { return running_; }

  /// Starts a calibration at `now`, dropping anything recorded before.
  /// Returns the first (RUNNING) report.
  Report start(Clock::time_point now) {
    *this = NudgeCalibrator{};
    running_ = true;
    last_activity_ = now;
    last_report_ = now;
    nudges_.reserve(NUDGE_CAL_TOTAL);
    return report(Status::RUNNING);
  }

  /// Ends the calibration without applying anything and frees what it
  /// recorded. Returns the final report, with `message` for the person.
  Report stop(Status status, std::string message) {
    running_ = false;
    recording_ = false;
    ready_ = false;
    nudges_ = {};
    trace_ = {};
    Report r = report(status);
    r.message = std::move(message);
    return r;
  }

  /// The final report for a start that could not run at all.
  static Report refused(std::string message) {
    NudgeCalibrator none;
    return none.stop(Status::FAILED, std::move(message));
  }

  /// Feeds one mapped sample. Returns a report when something changed (and at
  /// least every NUDGE_CAL_REPORT_PERIOD while running), including the final
  /// one; nothing when the calibrator is not running.
  std::optional<Report> feed(float x, float y, Clock::time_point now) {
    if (!running_) {
      return std::nullopt;
    }
    magnitude_ = std::hypot(x, y);
    if (now - last_activity_ >= NUDGE_CAL_IDLE_TIMEOUT) {
      return stop(Status::FAILED, "No nudge was measured for 30 seconds, so nudge calibration "
                                  "stopped. Nothing was changed.");
    }
    bool changed = false;
    const bool at_rest = magnitude_ <= NUDGE_CAL_REST_MAGNITUDE;
    if (recording_) {
      record(x, y, now);
      if (at_rest) {
        recording_ = false;
        rest_since_ = now;
        rest_history_.clear();
        note_rest(now);
        last_activity_ = now;
        changed = true;
        if (auto final_report = finish_attempt()) {
          return final_report;
        }
      }
    } else if (at_rest) {
      if (!rest_since_) {
        rest_since_ = now;
      }
      note_rest(now);
      if (!ready_ && now - *rest_since_ >= NUDGE_CAL_REST_HOLD) {
        ready_ = true;
        changed = true;
      }
    } else {
      // Out of rest. Only a push that starts from a settled rest is measured;
      // one made too soon after the last has to come back and wait first.
      rest_since_.reset();
      if (ready_) {
        ready_ = false;
        recording_ = true;
        changed = true;
        gesture_start_ = now;
        trace_.clear();
        trace_.reserve(rest_history_.size() + NUDGE_CAL_MAX_SAMPLES);
        for (const auto &r : rest_history_) {
          trace_.push_back({ms_since_start(r.at), r.magnitude});
        }
        recorded_ = 0;
        peak_ = 0.0f;
        record(x, y, now);
      } else {
        rest_history_.clear();
      }
    }
    if (changed || now - last_report_ >= NUDGE_CAL_REPORT_PERIOD) {
      last_report_ = now;
      return report(Status::RUNNING);
    }
    return std::nullopt;
  }

  /// The direction asked for next (NONE once every nudge is in).
  NudgeDirection expected() const {
    return accepted_ < NUDGE_CAL_TOTAL ? NUDGE_CAL_ORDER[accepted_ / NUDGE_CAL_REPEATS]
                                       : NudgeDirection::NONE;
  }

private:
  struct RestSample {
    Clock::time_point at{};
    float magnitude{0.0f};
  };

  // Keeps the last NUDGE_CAL_MAX_WINDOW of rest, for the start of the next trace.
  void note_rest(Clock::time_point now) {
    rest_history_.push_back({now, magnitude_});
    while (now - rest_history_.front().at > NUDGE_CAL_MAX_WINDOW) {
      rest_history_.pop_front();
    }
  }

  std::int32_t ms_since_start(Clock::time_point at) const {
    return static_cast<std::int32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(at - gesture_start_).count());
  }

  void record(float x, float y, Clock::time_point now) {
    if (recorded_ < NUDGE_CAL_MAX_SAMPLES) {
      trace_.push_back({ms_since_start(now), magnitude_});
      ++recorded_;
    }
    if (magnitude_ > peak_) {
      peak_ = magnitude_;
      peak_x_ = x;
      peak_y_ = y;
    }
  }

  // Judges the attempt that just came back to rest. Returns the final
  // report once the last nudge is in.
  std::optional<Report> finish_attempt() {
    last_direction_ = classify_nudge(peak_x_, peak_y_);
    if (peak_ < NUDGE_CAL_MIN_PEAK) {
      last_attempt_ = Attempt::TOO_SMALL;
    } else if (last_direction_ != expected()) {
      last_attempt_ = Attempt::WRONG_DIRECTION;
    } else {
      last_attempt_ = Attempt::ACCEPTED;
      nudges_.push_back({last_direction_, peak_, std::move(trace_)});
      trace_ = {};
      ++accepted_;
    }
    if (accepted_ < NUDGE_CAL_TOTAL) {
      return std::nullopt;
    }
    NudgeTuning tuning;
    std::error_code ec;
    if (!compute_nudge_tuning(nudges_, tuning, ec)) {
      return stop(Status::FAILED,
                  "These nudges were too small or too slow to tell apart from moving the cursor, "
                  "so nothing was changed. Try pushing a little further or a little quicker, or "
                  "set the values by hand.");
    }
    Report r = stop(Status::SUCCEEDED, "");
    r.enter_threshold = tuning.enter_threshold;
    r.exit_threshold = tuning.exit_threshold;
    r.hold_time_ms = static_cast<std::uint32_t>(tuning.hold_time.count());
    return r;
  }

  Report report(Status status) const {
    Report r;
    r.status = status;
    r.accepted = static_cast<std::uint8_t>(accepted_);
    r.total = static_cast<std::uint8_t>(NUDGE_CAL_TOTAL);
    r.expected_direction =
        static_cast<std::uint8_t>(status == Status::RUNNING ? expected() : NudgeDirection::NONE);
    r.last_attempt = last_attempt_;
    r.last_direction = static_cast<std::uint8_t>(last_direction_);
    r.ready = running_ && ready_;
    r.magnitude = magnitude_;
    return r;
  }

  bool running_{false};
  bool recording_{false};
  bool ready_{false};
  int accepted_{0};
  float magnitude_{0.0f};
  float peak_{0.0f}, peak_x_{0.0f}, peak_y_{0.0f};
  Attempt last_attempt_{Attempt::NONE};
  NudgeDirection last_direction_{NudgeDirection::NONE};
  std::optional<Clock::time_point> rest_since_;
  std::deque<RestSample> rest_history_;
  std::size_t recorded_{0};
  Clock::time_point gesture_start_{};
  Clock::time_point last_activity_{};
  Clock::time_point last_report_{};
  std::vector<NudgeSample> trace_;
  std::vector<RecordedNudge> nudges_;
};

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
  /// START_NUDGE_CALIBRATION / CANCEL_NUDGE_CALIBRATION taken from the
  /// NudgeCalibrationGate for this tick. A start always ends in a final
  /// Ports::nudge_calibration_report(), even when it cannot run.
  bool nudge_calibration_start{false};
  bool nudge_calibration_cancel{false};
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
  /// The detector's trailing window: recent magnitudes, increasing, so
  /// recent.front() is the lowest one in the last nudge_hold_time.
  struct MagnitudeSample {
    Clock::time_point at{};
    float magnitude{0.0f};
  };
  std::deque<MagnitudeSample> recent;
  /// When the held nudge started, and (left click) the stick position the
  /// drag moves the cursor relative to.
  Clock::time_point nudge_since{};
  float anchor_x{0.0f}, anchor_y{0.0f};
  bool initialised{false};
  /// A running nudge calibration takes over the tick (neutral output).
  NudgeCalibrator nudge_calibration;
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

// Restarts the nudge detector's trailing window at this sample, so the next
// rise is measured from here (after a recalibration, a hold, a fault, or with
// nudge clicks off): a stick that is still pushed cannot click by itself.
inline void restart_nudge_history(MouseState &s, float x, float y, Clock::time_point now) {
  s.recent.clear();
  s.recent.push_back({now, std::hypot(x, y)});
  s.initialised = true;
}

// Cursor movement for a stick offset from the drag anchor.
inline void drag_delta(float ex, float ey, MouseReport &mouse) {
  const float m = std::hypot(ex, ey);
  if (m <= DRAG_DEADZONE) {
    return;
  }
  const float scale = (m - DRAG_DEADZONE) / m * SENSITIVITY;
  mouse.dx = to_delta(ex * scale);
  mouse.dy = to_delta(ey * scale);
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
    detail::restart_nudge_history(s, jx, jy, in.now);
  }
  const float cur_mag = std::hypot(jx, jy);

  // Nudge tuning is read live every poll, so a console edit
  // (SET_NUDGE_SETTINGS) takes effect at once without needing a restart.
  const auto &nudge_settings = in.settings;

  // With nudge clicks switched off (SET_NUDGE_ENABLED), a held nudge is
  // released on this very tick, so a held button or scroll never outlives the
  // switch. The trailing window follows the stick every tick while off, so
  // switching back on measures only movement made after the switch: a push
  // made while off can never turn into a click.
  if (!nudge_settings.nudge_enabled) {
    if (s.nudge != NudgeDirection::NONE) {
      s.nudge = NudgeDirection::NONE;
      s.scroll_poll_count = 0;
    }
    detail::restart_nudge_history(s, jx, jy, in.now);
  }

  // --- Nudge detection: the rise over the trailing window, every poll ---
  while (!s.recent.empty() && s.recent.back().magnitude >= cur_mag) {
    s.recent.pop_back();
  }
  s.recent.push_back({in.now, cur_mag});
  while (in.now - s.recent.front().at > nudge_settings.nudge_hold_time) {
    s.recent.pop_front();
  }
  const float rise = cur_mag - s.recent.front().magnitude;
  const bool settled = in.now - s.nudge_since >= nudge_settings.nudge_hold_time;

  if (s.nudge == NudgeDirection::NONE) {
    // Never on a failed read: the sensor fault monitor needs a few polls to
    // trip (D21), and a stale or bad sample must not click meanwhile.
    if (nudge_settings.nudge_enabled && in.stick.read_ok &&
        rise >= nudge_settings.nudge_distance_threshold) {
      s.nudge = classify_nudge(jx, jy);
      s.scroll_poll_count = 0;
      s.nudge_since = in.now;
      s.anchor_x = jx;
      s.anchor_y = jy;
    }
  } else if (settled && cur_mag <= nudge_settings.nudge_exit_threshold) {
    // Released: the next nudge has to rise from here.
    s.nudge = NudgeDirection::NONE;
    detail::restart_nudge_history(s, jx, jy, in.now);
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
    // Left click held: drag relative to the anchor once settled (see DRAG_DEADZONE).
    if (settled) {
      detail::drag_delta(jx - s.anchor_x, jy - s.anchor_y, mouse);
    } else {
      s.anchor_x = jx;
      s.anchor_y = jy;
    }
    break;
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
    mouse->nudge = NudgeDirection::NONE;
    mouse->scroll_poll_count = 0;
    detail::restart_nudge_history(*mouse, post_sweep.x, post_sweep.y, now);
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
  /// A nudge calibration report: every report of a running calibration, and
  /// exactly one final one (status other than RUNNING) for every start the
  /// tick was given. On SUCCEEDED the adapter applies the settings
  /// (OmegaStore::set_nudge_calibration()) and saves them; on any final report
  /// it frees the NudgeCalibrationGate.
  virtual void nudge_calibration_report(const OmegaCalibration::NudgeCalibrationReport &report) = 0;

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

using NudgeCalStatus = OmegaCalibration::NudgeCalibrationReport::Status;

// Ends a running nudge calibration with its final report; nothing otherwise.
inline void end_nudge_calibration(MouseState &s, Ports &ports, NudgeCalStatus status,
                                  std::string message) {
  if (s.nudge_calibration.running()) {
    ports.nudge_calibration_report(s.nudge_calibration.stop(status, std::move(message)));
  }
}

// This tick's START/CANCEL_NUDGE_CALIBRATION, applied before anything else. A
// start that cannot run now is answered with a final FAILED report, so the
// gate is always freed; a start and a cancel in the same tick start and then
// cancel.
inline void nudge_calibration_commands(MouseState &s, const StepInput &in, Ports &ports) {
  if (in.nudge_calibration_start) {
    if (in.needs_calibration) {
      ports.nudge_calibration_report(NudgeCalibrator::refused(
          "The stick has no usable calibration yet. Run the full calibration first, then "
          "calibrate nudge clicks."));
    } else if (in.output_hold) {
      ports.nudge_calibration_report(NudgeCalibrator::refused(
          "A firmware update or restart is in progress, so nudge calibration cannot start."));
    } else {
      ports.nudge_calibration_report(s.nudge_calibration.start(in.now));
    }
  }
  if (in.nudge_calibration_cancel) {
    end_nudge_calibration(s, ports, NudgeCalStatus::CANCELLED,
                          "Nudge calibration was cancelled. Nothing was changed.");
  }
}

} // namespace detail

inline void mouse_tick(MouseState &s, const StepInput &in, Ports &ports) {
  detail::nudge_calibration_commands(s, in, ports);
  if (holds_output(in)) {
    detail::end_nudge_calibration(
        s, ports, detail::NudgeCalStatus::FAILED,
        in.output_hold ? "A firmware update or restart started, so nudge calibration stopped. "
                         "Nothing was changed."
                       : "The stick has no usable calibration, so nudge calibration stopped. "
                         "Nothing was changed.");
    // Neutral report every tick while a firmware update runs or there is no
    // usable calibration, with the same bookkeeping reset as a sensor
    // recovery so releasing the hold does not see a delta against a reading
    // from before it. The sensor monitor keeps running; during an update a
    // pending recalibration waits (the caller only consumes it when not
    // holding).
    detail::sensor_fault_update(s.common, in, ports);
    s.nudge = NudgeDirection::NONE;
    s.scroll_poll_count = 0;
    detail::restart_nudge_history(s, in.stick.x, in.stick.y, in.now);
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
  if (fault.faulted) {
    detail::end_nudge_calibration(s, ports, detail::NudgeCalStatus::FAILED,
                                  "The sensor stopped answering, so nudge calibration stopped. "
                                  "Nothing was changed.");
  }
  if (in.recalibration_requested) {
    detail::end_nudge_calibration(s, ports, detail::NudgeCalStatus::CANCELLED,
                                  "A full calibration started, so nudge calibration stopped. "
                                  "Nothing was changed.");
  }
  if (fault.recovered) {
    // Same reset as after a recalibration: the next window must not see a
    // delta against a reading from before the fault.
    detail::restart_nudge_history(s, in.stick.x, in.stick.y, in.now);
  }
  if (fault.faulted && !in.recalibration_requested) {
    ports.send_mouse(MouseReport{});
    return;
  }
  if (s.nudge_calibration.running()) {
    // The calibrator gets the mapped sample; the host gets a neutral report,
    // so the nudges asked for neither move the cursor nor click. The detector
    // follows the stick meanwhile and starts fresh afterwards, with the drift
    // compensator, whose idle timer did not run.
    if (auto report = s.nudge_calibration.feed(in.stick.x, in.stick.y, in.now)) {
      ports.nudge_calibration_report(*report);
    }
    s.nudge = NudgeDirection::NONE;
    s.scroll_poll_count = 0;
    detail::restart_nudge_history(s, in.stick.x, in.stick.y, in.now);
    if (!s.nudge_calibration.running()) {
      s.common.drift.reset();
    }
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
  if (in.nudge_calibration_start) {
    // The service refuses it in XAC mode; this only keeps the gate honest.
    ports.nudge_calibration_report(NudgeCalibrator::refused(
        "Nudge clicks are only used in mouse mode, so there is nothing to calibrate in XAC mode."));
  }
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

/// Cross-task hand-off for START_NUDGE_CALIBRATION / CANCEL_NUDGE_CALIBRATION,
/// shaped like RecalibrationGate. The calibration service calls
/// request_start() / request_cancel() from the USB task; the polling loop
/// passes take_start() and take_cancel() to the tick as
/// StepInput::nudge_calibration_start / _cancel every iteration, and the Ports
/// adapter calls finish() on the final report. The gate stays busy from an
/// accepted start until then, so a second start is refused
/// (device_or_resource_busy) rather than restarting a calibration halfway.
class NudgeCalibrationGate {
public:
  /// Accepts a start if no calibration is pending or running.
  bool request_start(std::error_code &ec) {
    State expected = State::Idle;
    if (!state_.compare_exchange_strong(expected, State::Requested)) {
      ec = std::make_error_code(std::errc::device_or_resource_busy);
      return false;
    }
    return true;
  }

  /// Asks a pending or running calibration to stop. Returns whether there
  /// was one (its CANCELLED report follows from the polling loop).
  bool request_cancel() {
    if (state_.load() == State::Idle) {
      return false;
    }
    cancel_requested_.store(true);
    return true;
  }

  /// True exactly once per accepted start: moves it to running.
  bool take_start() {
    State expected = State::Requested;
    return state_.compare_exchange_strong(expected, State::Running);
  }

  /// True once per cancel request.
  bool take_cancel() { return cancel_requested_.exchange(false); }

  /// The calibration has sent its final report: accept a new start. A cancel
  /// not yet taken belonged to this calibration and is dropped.
  void finish() {
    cancel_requested_.store(false);
    state_.store(State::Idle);
  }

  bool busy() const { return state_.load() != State::Idle; }

private:
  enum class State : uint8_t { Idle, Requested, Running };
  std::atomic<State> state_{State::Idle};
  std::atomic<bool> cancel_requested_{false};
};

} // namespace espp::omega_control
