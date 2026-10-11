#pragma once

// What is moving the simulated stick: a hand (the console's `stick` message), a
// built-in pattern, or a script's keyframes. Positions are x right, y up, -1..1.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "script.hpp"
#include "sensor.hpp"

namespace sim {

/// Position of a built-in pattern `t_ms` after it started. `range_t_ms` is the time since
/// the firmware's range-calibration prompt ("START!"), or negative when none is running.
StickPos pattern_position(const std::string &name, double t, double range_t);

/// How long a pattern lasts by itself, or negative for ones that run until stopped.
double pattern_length_ms(const std::string &name);

class StickSource {
public:
  enum class Kind { Manual, Pattern, Script };
  struct Info {
    Kind kind{Kind::Manual};
    std::string name;
    int64_t t_ms{0};
    bool done{false};
  };

  /// Moves the stick by hand; values outside -1..1 are clamped.
  void set_manual(double x, double y);
  bool play_pattern(const std::string &name, int64_t now_ms, int64_t duration_ms = -1);
  void play_script(const Script &script, int64_t now_ms);
  /// Back to manual with the stick at rest.
  void stop();

  StickPos position(int64_t now_ms) const;
  Info info(int64_t now_ms) const;

  /// Called with each calibration prompt the firmware prints, so the `calibrate`
  /// pattern can follow the sweep.
  void note_prompt(const std::string &text, int64_t now_ms);

private:
  StickPos script_position(int64_t t_ms, double range_start) const;

  mutable std::mutex mu_;
  Kind kind_{Kind::Manual};
  StickPos manual_{};
  std::string pattern_;
  int64_t start_ms_{0};
  int64_t duration_ms_{-1};
  Script script_;
  std::vector<size_t> track_; ///< indexes of the script's stick and pattern steps
  double range_start_ms_{-1};
};

} // namespace sim
