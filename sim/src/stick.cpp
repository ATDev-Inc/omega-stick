#include "stick.hpp"

#include <algorithm>
#include <cmath>

namespace sim {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kCircleMs = 8000.0;
// The range sweep samples for RANGE_SAMPLES * SAMPLE_DELAY = 2 s, so the calibrate
// pattern goes around once in a little under that.
constexpr double kCalibrateCircleMs = 1800.0;
constexpr double kNudgeStartMs = 50.0;
constexpr double kNudgeHoldMs = 400.0;
constexpr double kNudgeLengthMs = 600.0;

StickPos nudge(double dx, double dy, double t) {
  if (t >= kNudgeStartMs && t < kNudgeStartMs + kNudgeHoldMs) {
    return {dx, dy};
  }
  return {};
}

} // namespace

double pattern_length_ms(const std::string &name) {
  if (name.rfind("nudge_", 0) == 0) {
    return kNudgeLengthMs;
  }
  return -1.0;
}

StickPos pattern_position(const std::string &name, double t, double range_t) {
  if (name == "circle") {
    const double a = 2.0 * kPi * t / kCircleMs;
    return {std::cos(a), std::sin(a)};
  }
  if (name == "calibrate") {
    if (range_t < 0.0 || range_t >= kCalibrateCircleMs) {
      return {};
    }
    const double a = 2.0 * kPi * range_t / kCalibrateCircleMs;
    return {std::cos(a), std::sin(a)};
  }
  if (name == "nudge_right") {
    return nudge(1.0, 0.0, t);
  }
  if (name == "nudge_left") {
    return nudge(-1.0, 0.0, t);
  }
  if (name == "nudge_up") {
    return nudge(0.0, 1.0, t);
  }
  if (name == "nudge_down") {
    return nudge(0.0, -1.0, t);
  }
  if (name == "wander") {
    const double s = t / 1000.0;
    StickPos p{0.7 * std::sin(0.9 * s) + 0.25 * std::sin(2.3 * s + 1.0),
               0.7 * std::sin(0.6 * s + 2.0) + 0.25 * std::sin(1.7 * s)};
    const double m = std::hypot(p.x, p.y);
    if (m > 1.0) {
      p.x /= m;
      p.y /= m;
    }
    return p;
  }
  return {}; // rest
}

void StickSource::set_manual(double x, double y) {
  std::lock_guard<std::mutex> lock(mu_);
  kind_ = Kind::Manual;
  manual_ = {std::clamp(x, -1.0, 1.0), std::clamp(y, -1.0, 1.0)};
}

bool StickSource::play_pattern(const std::string &name, int64_t now_ms, int64_t duration_ms) {
  if (!is_known_pattern(name)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  kind_ = Kind::Pattern;
  pattern_ = name;
  start_ms_ = now_ms;
  duration_ms_ = duration_ms;
  return true;
}

void StickSource::play_script(const Script &script, int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  kind_ = Kind::Script;
  script_ = script;
  start_ms_ = now_ms;
  track_.clear();
  for (size_t i = 0; i < script_.steps.size(); i++) {
    if (script_.steps[i].kind == Step::Kind::Stick ||
        script_.steps[i].kind == Step::Kind::Pattern) {
      track_.push_back(i);
    }
  }
}

void StickSource::stop() {
  std::lock_guard<std::mutex> lock(mu_);
  kind_ = Kind::Manual;
  manual_ = {};
}

void StickSource::note_prompt(const std::string &text, int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  if (text.rfind("Release the joystick", 0) == 0) {
    range_start_ms_ = -1;
  } else if (text == "START!") {
    range_start_ms_ = static_cast<double>(now_ms);
  }
}

StickPos StickSource::script_position(int64_t t_ms, double range_start) const {
  const double rest_range = range_start;
  const Step *cur = nullptr;
  const Step *next = nullptr;
  for (size_t k = 0; k < track_.size(); k++) {
    const Step &s = script_.steps[track_[k]];
    if (s.at_ms > t_ms) {
      break;
    }
    cur = &s;
    next = k + 1 < track_.size() ? &script_.steps[track_[k + 1]] : nullptr;
  }
  if (!cur) {
    return {};
  }
  if (cur->kind == Step::Kind::Pattern) {
    const double lt = static_cast<double>(t_ms - cur->at_ms);
    const double len = cur->duration_ms >= 0 ? static_cast<double>(cur->duration_ms)
                                             : pattern_length_ms(cur->name);
    if (len >= 0 && lt >= len) {
      return {};
    }
    const double range_t = rest_range < 0 ? -1.0 : static_cast<double>(t_ms) - rest_range;
    return pattern_position(cur->name, lt, range_t);
  }
  if (next && next->kind == Step::Kind::Stick && next->at_ms > cur->at_ms) {
    const double f =
        static_cast<double>(t_ms - cur->at_ms) / static_cast<double>(next->at_ms - cur->at_ms);
    return {cur->x + (next->x - cur->x) * f, cur->y + (next->y - cur->y) * f};
  }
  return {cur->x, cur->y};
}

StickPos StickSource::position(int64_t now_ms) const {
  std::lock_guard<std::mutex> lock(mu_);
  const double range_t = range_start_ms_ < 0 ? -1.0 : static_cast<double>(now_ms) - range_start_ms_;
  switch (kind_) {
  case Kind::Manual:
    return manual_;
  case Kind::Pattern: {
    const double t = static_cast<double>(now_ms - start_ms_);
    const double len =
        duration_ms_ >= 0 ? static_cast<double>(duration_ms_) : pattern_length_ms(pattern_);
    if (len >= 0 && t >= len) {
      return {};
    }
    return pattern_position(pattern_, t, range_t);
  }
  case Kind::Script:
    // script_position works in script time; the range start is passed in script time too.
    return script_position(now_ms - start_ms_,
                           range_start_ms_ < 0 ? -1.0
                                               : range_start_ms_ - static_cast<double>(start_ms_));
  }
  return {};
}

StickSource::Info StickSource::info(int64_t now_ms) const {
  std::lock_guard<std::mutex> lock(mu_);
  Info i;
  i.kind = kind_;
  i.t_ms = kind_ == Kind::Manual ? 0 : now_ms - start_ms_;
  switch (kind_) {
  case Kind::Manual:
    break;
  case Kind::Pattern: {
    i.name = pattern_;
    const double len =
        duration_ms_ >= 0 ? static_cast<double>(duration_ms_) : pattern_length_ms(pattern_);
    i.done = len >= 0 && static_cast<double>(i.t_ms) >= len;
    break;
  }
  case Kind::Script:
    i.name = script_.name;
    i.done = !script_.steps.empty() ? i.t_ms > script_.steps.back().at_ms : true;
    break;
  }
  return i;
}

} // namespace sim
