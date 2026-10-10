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
  /// constructed values -- exactly the "no usable cached calibration"
  /// fallback the interactive calibration flow already handles; the
  /// application is responsible for then running an interactive
  /// calibration (as before OmegaStore existed) when center()/range() come
  /// back at their defaults, and can push the result in via
  /// set_calibration().
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

  /// Replaces center/range outright -- e.g. right after an interactive
  /// recalibration (center_calibration() + range_calibration() +
  /// finalize_center_deadzone(), run from the main task or a console
  /// command). Fires on_calibration_changed().
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

  /// Sets the input mode live and fires on_mode_changed() (outside the
  /// lock) so the application can act on it -- see ModeChangedFn.
  void set_input_mode(InputMode mode) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
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
  CenterResult center_{};
  RangeResult range_{};
  DeviceSettings settings_{};
  std::filesystem::path calibration_path_;
  std::filesystem::path settings_path_;
  CalibrationChangedFn on_calibration_changed_;
  ModeChangedFn on_mode_changed_;
};

} // namespace espp