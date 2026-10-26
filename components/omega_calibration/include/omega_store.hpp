#pragma once

// Thread-safe holder for one omega-stick's live calibration state
// (espp::OmegaCalibration::CenterResult / RangeResult / DeviceSettings).
// This plays the same role for CalibrationService that espp::CoreDump plays
// for CoreDumpService (see coredump.hpp / coredump_service.hpp): it owns the
// data, serializes access to it with its own internal mutex, and knows how
// to load/save it to flash -- so several callers (the stick's polling/
// control loop, and any number of CalibrationService transports) can safely
// share one instance without a live SET_* from the console racing the
// control loop's own reads.

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <system_error>

#include "base_component.hpp"
#include "omega_calibration.hpp"

namespace espp {

/**
 * @brief Thread-safe live store for an omega-stick's calibration + settings.
 *
 * Wraps espp::OmegaCalibration::CenterResult, RangeResult, and DeviceSettings
 * with an internal mutex, load-on-construction, and save-on-demand, so the
 * control loop and any number of espp::CalibrationService transports can
 * read/update the same live state safely.
 *
 * **Usage pattern**: the control loop should call snapshot() (or the
 * individual center()/range()/settings() accessors) once per iteration
 * rather than caching a copy across iterations, since a CalibrationService
 * transport can update any of these fields at any time from a different
 * task/context (e.g. a USB RX callback). After building/updating an
 * espp::Joystick from a snapshot, call
 * `OmegaCalibration::apply_calibration(js, snap.center, snap.range,
 * snap.settings)` -- or do that from the on_calibration_changed callback so
 * it happens immediately on a live edit rather than waiting for the next
 * polling iteration.
 *
 * @note Mirrors espp::CoreDump's design: one internal mutex serializes every
 *       access (see CoreDump's class-level threading notes), so this can be
 *       constructed once and shared between the control loop and however
 *       many CalibrationService instances (one per transport) you mount.
 */
class OmegaStore : public BaseComponent {
public:
  using CenterResult = OmegaCalibration::CenterResult;
  using RangeResult = OmegaCalibration::RangeResult;
  using DeviceSettings = OmegaCalibration::DeviceSettings;
  using InputMode = OmegaCalibration::InputMode;

  /// Called (OUTSIDE the internal lock) after a live update that changes the
  /// EFFECTIVE calibration -- a manual offset or a deadzone override -- so
  /// the caller can push it into a live espp::Joystick via
  /// `OmegaCalibration::apply_calibration(js, center, range, settings)`.
  /// Not fired for nudge-tuning or input-mode changes: the control loop
  /// simply reads the latest settings() each iteration for nudge tuning, and
  /// input-mode changes go through on_mode_changed instead (see below).
  using CalibrationChangedFn = std::function<void()>;

  /// Called (OUTSIDE the internal lock) when the input mode changes.
  /// Actually switching the running USB HID configuration is more than
  /// flipping a flag (typically needs re-enumeration or a full USB stack
  /// restart), so that decision is left to the application -- e.g. set a
  /// flag the main task checks at a safe point, or persist settings and call
  /// esp_restart().
  using ModeChangedFn = std::function<void(InputMode)>;

  struct Config {
    /// Where CenterResult/RangeResult are loaded from / saved to.
    std::filesystem::path calibration_path = OmegaCalibration::default_path();
    /// Where DeviceSettings is loaded from / saved to (a separate file --
    /// see DeviceSettings::default_path() for why).
    std::filesystem::path settings_path = OmegaCalibration::DeviceSettings::default_path();
    CalibrationChangedFn on_calibration_changed{nullptr};
    ModeChangedFn on_mode_changed{nullptr};
    espp::Logger::Verbosity log_level{espp::Logger::Verbosity::WARN};
  };

  /// Loads calibration + settings from flash if present. A missing or
  /// unparseable file is logged and left at that struct's default-
  /// constructed values; a calibration file that parses but fails
  /// OmegaCalibration::check_usable() keeps what the file says. Either way
  /// the application loads the calibration itself, decides whether it is
  /// usable, and pushes what it runs with in via set_calibration().
  explicit OmegaStore(const Config &config)
      : BaseComponent("OmegaStore", config.log_level)
      , calibration_path_(config.calibration_path)
      , settings_path_(config.settings_path)
      , on_calibration_changed_(config.on_calibration_changed)
      , on_mode_changed_(config.on_mode_changed) {
    std::error_code ec;
    if (!OmegaCalibration::load(center_, range_, calibration_path_, ec)) {
      logger_.info("no usable cached calibration ({}); center/range left at defaults",
                   ec.message());
    }
    if (!DeviceSettings::load(settings_, settings_path_, ec)) {
      logger_.info("no usable cached settings ({}); settings left at defaults", ec.message());
    }
    // A saved override outside the limits the service enforces (hand-edited
    // file, or saved by firmware that did not check) would suppress normal
    // travel on every boot, so fall back to the calibration-derived value.
    drop_out_of_range_override(settings_.manual_center_deadzone,
                               OmegaCalibration::MAX_CENTER_DEADZONE, "center");
    drop_out_of_range_override(settings_.manual_range_deadzone,
                               OmegaCalibration::MAX_RANGE_DEADZONE, "range");
  }

  /// A consistent, point-in-time copy of everything (center + range +
  /// settings). Prefer this over separate center()/range()/settings() calls
  /// when you need more than one field to agree with each other (e.g. a
  /// GET_STATE reply, or applying calibration to a Joystick).
  struct Snapshot {
    CenterResult center;
    RangeResult range;
    DeviceSettings settings;
  };
  Snapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Snapshot{center_, range_, settings_};
  }

  CenterResult center() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return center_;
  }
  RangeResult range() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return range_;
  }
  DeviceSettings settings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
  }

  /// Replaces center/range outright, manual offsets included -- e.g. at
  /// boot, after loading or running the first calibration. The control
  /// loop uses set_measured_calibration() instead. Fires
  /// on_calibration_changed().
  void set_calibration(const CenterResult &center, const RangeResult &range) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      center_ = center;
      range_ = range;
    }
    if (on_calibration_changed_) {
      on_calibration_changed_();
    }
  }

  /// Replaces the measured center/range but keeps the live manual offsets.
  /// For the control loop (drift correction, console-triggered
  /// recalibration): its local CenterResult can hold offsets older than a
  /// SET_MANUAL_OFFSET the console has since applied, and copying it
  /// wholesale would silently revert that trim. Read center() back
  /// afterwards to pick up the live offsets. Fires on_calibration_changed().
  void set_measured_calibration(const CenterResult &center, const RangeResult &range) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const float offset_x = center_.manual_offset_x;
      const float offset_y = center_.manual_offset_y;
      center_ = center;
      OmegaCalibration::set_manual_offset(center_, offset_x, offset_y);
      range_ = range;
    }
    if (on_calibration_changed_) {
      on_calibration_changed_();
    }
  }

  /// Sets the manual per-axis center trim live (see
  /// OmegaCalibration::set_manual_offset()). Fires on_calibration_changed().
  void set_manual_offset(float offset_x, float offset_y) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      OmegaCalibration::set_manual_offset(center_, offset_x, offset_y);
    }
    if (on_calibration_changed_) {
      on_calibration_changed_();
    }
  }

  /// Sets or clears the manual deadzone overrides live. std::nullopt clears
  /// that override back to the calibration-derived value. Fires
  /// on_calibration_changed().
  void set_deadzone_override(std::optional<float> manual_center_deadzone,
                             std::optional<float> manual_range_deadzone) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      settings_.manual_center_deadzone = manual_center_deadzone;
      settings_.manual_range_deadzone = manual_range_deadzone;
    }
    if (on_calibration_changed_) {
      on_calibration_changed_();
    }
  }

  /// Sets the nudge-click tuning live (enter-delta threshold, exit-absolute
  /// threshold, and the measurement time window -- see DeviceSettings'
  /// field docs). No change-notification is fired: the control loop simply
  /// reads the latest settings() each measurement window.
  void set_nudge_settings(float enter_threshold, float exit_threshold,
                          std::chrono::milliseconds time_window) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.nudge_distance_threshold = enter_threshold;
    settings_.nudge_exit_threshold = exit_threshold;
    settings_.nudge_hold_time = time_window;
  }

  /// Turns nudge clicks on or off live, keeping the tuning values. No
  /// change-notification is fired: the control loop reads settings() every
  /// iteration and releases a held nudge as soon as it sees false.
  void set_nudge_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.nudge_enabled = enabled;
  }

  /// Sets the input mode live and fires on_mode_changed() (outside the
  /// lock) so the application can act on it -- see ModeChangedFn. Setting
  /// the mode that is already active does nothing.
  void set_input_mode(InputMode mode) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (settings_.input_mode == mode)
        return;
      settings_.input_mode = mode;
    }
    if (on_mode_changed_) {
      on_mode_changed_(mode);
    }
  }

  /// Persists the CURRENT live center/range/settings to flash (both files --
  /// see Config::calibration_path / Config::settings_path). If the
  /// calibration write fails, the settings file is left untouched and this
  /// returns false with ec describing the calibration failure; if the
  /// calibration write succeeds but the settings write fails, the
  /// calibration file has still been updated (ec then describes the
  /// settings failure).
  bool save_to_flash(std::error_code &ec) {
    // Held across the snapshot and both file writes, so two callers (the
    // console worker and the control loop's drift save) cannot interleave on
    // the same temporary files. Separate from mutex_ so the live setters are
    // not blocked behind flash I/O.
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    const Snapshot snap = snapshot();
    if (!OmegaCalibration::save(snap.center, snap.range, calibration_path_, ec)) {
      logger_.error("save_to_flash: calibration save failed: {}", ec.message());
      return false;
    }
    if (!DeviceSettings::save(snap.settings, settings_path_, ec)) {
      logger_.error("save_to_flash: settings save failed: {}", ec.message());
      return false;
    }
    logger_.info("saved calibration + settings to flash");
    return true;
  }

private:
  mutable std::mutex mutex_;
  std::mutex save_mutex_;
  CenterResult center_{};
  RangeResult range_{};
  void drop_out_of_range_override(std::optional<float> &value, float max, const char *name) {
    if (value && !(std::isfinite(*value) && *value >= 0.0f && *value <= max)) {
      logger_.warn("ignoring saved {} deadzone override {} (allowed 0..{})", name, *value, max);
      value.reset();
    }
  }

  DeviceSettings settings_{};
  std::filesystem::path calibration_path_;
  std::filesystem::path settings_path_;
  CalibrationChangedFn on_calibration_changed_;
  ModeChangedFn on_mode_changed_;
};

} // namespace espp