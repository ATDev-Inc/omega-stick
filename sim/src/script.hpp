#pragma once

// Simulator scripts: parsing and validation of the JSON timeline described in
// docs/simulator.md. Executing a script is the host's job (sim_host.cpp).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace sim {

inline constexpr int64_t kMaxScriptMs = 86'400'000;
inline constexpr size_t kMaxScriptSteps = 5000;
inline constexpr size_t kMaxScriptBytes = 256 * 1024;
inline constexpr double kMaxDriftRate = 10.0; ///< stick units per minute
inline constexpr double kMaxNoiseSigma = 1.0; ///< stick units
inline constexpr double kMaxCenterDeadzone = 0.5;
inline constexpr double kMaxRangeDeadzone = 0.1;

struct Expect {
  std::optional<bool> calibrated;
  std::optional<double> center_max;
  std::optional<std::string> mouse_moved; ///< right, left, up, down, none
  std::optional<std::string> clicked;     ///< left, right, none
  std::optional<std::string> mode;        ///< mouse, xac
  std::optional<bool> sensor_fault;
  std::optional<bool> sensor_observed;
  std::optional<bool> neutral;
  std::optional<bool> needs_calibration; ///< GET_DIAGNOSTICS calibration_state (D26)
  std::optional<std::string> fw_slot;
  std::optional<bool> fw_pending;
  std::optional<std::string> fw_version;
  std::optional<double> xac_x; ///< latest gamepad x, within 0.1
  std::optional<double> xac_y;
};

struct Step {
  enum class Kind { Stick, Pattern, Fault, Drift, Noise, NextImage, Action, Expect };
  int64_t at_ms{0};
  Kind kind{Kind::Stick};
  double x{0}, y{0};            ///< Stick
  std::string name;             ///< Pattern, Fault, NextImage: the value; Action: the action
  int64_t duration_ms{-1};      ///< Pattern: optional
  double number{0};             ///< Drift, Noise
  std::string text;             ///< Action value as text (set_mode, flash version)
  std::optional<double> center; ///< Action set_deadzone
  std::optional<double> range;
  std::optional<std::string> expect_reply;
  Expect expect;
};

struct Script {
  std::string name;
  std::vector<Step> steps;
};

bool is_known_pattern(const std::string &name);

/// Validates and converts a script document. On failure returns false with a
/// message that names the offending step.
bool parse_script(const json::Value &doc, Script &out, std::string &error);

/// Parses script text (checks the size limit first).
bool parse_script_text(const std::string &text, Script &out, std::string &error);

} // namespace sim
