#pragma once

// Host-test stand-in for espp/joystick. Keeps the public surface
// omega_calibration touches (Type, Config, set_calibration, update, x, y) and
// records what was pushed through set_calibration so tests can inspect it.
// The mapping in update() is a plain linear normalisation, not espp's
// deadband-aware one; tests should assert on the recorded calibration, not
// on mapped values.

#include <algorithm>
#include <functional>

#include "base_component.hpp"
#include "range_mapper.hpp"

namespace espp {

class Joystick : public BaseComponent {
public:
  enum class Type { RECTANGULAR, CIRCULAR };
  using get_values_fn = std::function<bool(float *, float *)>;

  struct Config {
    FloatRangeMapper::Config x_calibration;
    FloatRangeMapper::Config y_calibration;
    Type type{Type::RECTANGULAR};
    float center_deadzone_radius{0};
    float range_deadzone{0};
    get_values_fn get_values{nullptr};
    Logger::Verbosity log_level{Logger::Verbosity::WARN};
  };

  explicit Joystick(const Config &config)
      : BaseComponent("Joystick", config.log_level)
      , x_cal_(config.x_calibration)
      , y_cal_(config.y_calibration)
      , type_(config.type)
      , center_deadzone_radius_(config.center_deadzone_radius)
      , range_deadzone_(config.range_deadzone)
      , get_values_(config.get_values) {}

  void set_calibration(const FloatRangeMapper::Config &x_calibration,
                       const FloatRangeMapper::Config &y_calibration,
                       float center_deadzone_radius = 0, float range_deadzone = 0) {
    x_cal_ = x_calibration;
    y_cal_ = y_calibration;
    center_deadzone_radius_ = center_deadzone_radius;
    range_deadzone_ = range_deadzone;
    ++calibration_updates_;
  }

  void update() {
    float raw_x = 0, raw_y = 0;
    if (get_values_ && get_values_(&raw_x, &raw_y)) {
      update(raw_x, raw_y);
    }
  }

  void update(float raw_x, float raw_y) {
    x_ = map(x_cal_, raw_x);
    y_ = map(y_cal_, raw_y);
  }

  float x() const { return x_; }
  float y() const { return y_; }
  Type type() const { return type_; }

  // ---- test-only inspection (not part of the real espp API) ----
  const FloatRangeMapper::Config &x_calibration() const { return x_cal_; }
  const FloatRangeMapper::Config &y_calibration() const { return y_cal_; }
  float center_deadzone_radius() const { return center_deadzone_radius_; }
  float range_deadzone() const { return range_deadzone_; }
  int calibration_updates() const { return calibration_updates_; }

private:
  static float map(const FloatRangeMapper::Config &cal, float raw) {
    const float span = raw >= cal.center ? (cal.maximum - cal.center) : (cal.center - cal.minimum);
    if (span <= 0.0f) {
      return 0.0f;
    }
    return std::clamp((raw - cal.center) / span, -1.0f, 1.0f);
  }

  FloatRangeMapper::Config x_cal_;
  FloatRangeMapper::Config y_cal_;
  Type type_;
  float center_deadzone_radius_;
  float range_deadzone_;
  get_values_fn get_values_;
  float x_{0};
  float y_{0};
  int calibration_updates_{0};
};

} // namespace espp
