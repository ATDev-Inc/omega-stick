#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "hid-rp-gamepad.hpp"

namespace espp {

/**
 * @brief Xbox Adaptive Controller joystick input report.
 *
 * Uses the standard ESPP GamepadInputReport HID format.
 *
 * The omega-stick is a single physical joystick. It can be presented to the
 * XAC as either joystick port, selected with Side:
 *
 *   Side::RIGHT: stick drives axes 0, 1 (X, Y); axes 2, 3 (Z, RZ) stay centered
 *   Side::LEFT:  stick drives axes 2, 3 (Z, RZ); axes 0, 1 (X, Y) stay centered
 *
 *   Triggers:
 *     remain released
 *
 *   Hat:
 *     available
 *
 *   Buttons:
 *     12 buttons available
 */
template <uint8_t REPORT_ID = 1>
class XacInputReport : public GamepadInputReport<12,           // BUTTON_COUNT
                                                 std::uint8_t, // JOYSTICK_TYPE
                                                 std::uint8_t, // TRIGGER_TYPE
                                                 0,            // JOYSTICK_MIN
                                                 255,          // JOYSTICK_MAX
                                                 0,            // TRIGGER_MIN
                                                 255,          // TRIGGER_MAX
                                                 REPORT_ID> {

public:
  using Base = GamepadInputReport<12, std::uint8_t, std::uint8_t, 0, 255, 0, 255, REPORT_ID>;

  using Hat = typename Base::Hat;

  /// Which XAC joystick port the stick is reported as.
  enum class Side : std::uint8_t {
    RIGHT = 0, ///< axes 0, 1 (X, Y)
    LEFT = 1,  ///< axes 2, 3 (Z, RZ)
  };

  constexpr explicit XacInputReport(Side side = Side::RIGHT)
      : side_(side) {
    reset();
  }

  /**
   * @brief Select which joystick the stick is reported as.
   *
   * Recenters both joysticks so the previously selected axes do not keep a
   * stale deflection.
   */
  constexpr void set_side(Side side) {
    side_ = side;
    Base::set_joystick_axis(0, Base::joystick_center);
    Base::set_joystick_axis(1, Base::joystick_center);
    Base::set_joystick_axis(2, Base::joystick_center);
    Base::set_joystick_axis(3, Base::joystick_center);
  }

  constexpr Side side() const { return side_; }

  /**
   * @brief Reset the joystick to center and all controls to neutral.
   *
   * Keeps the selected side.
   */
  constexpr void reset() {
    Base::reset();
    // Both joysticks centered; both triggers released.
    Base::set_left_trigger(0.0f);
    Base::set_right_trigger(0.0f);
  }

  /**
   * @brief Set the joystick position on the selected side.
   *
   * @param x X position in range [-1, 1]
   * @param y Y position in range [-1, 1]
   */
  constexpr void set_joystick(float x, float y) {
    set_x(x);
    set_y(y);
  }

  /**
   * @brief Set the joystick position on the selected side using raw 8-bit data.
   */
  constexpr void set_joystick(std::uint8_t x, std::uint8_t y) {
    set_x(x);
    set_y(y);
  }

  /**
   * @brief Set only the X axis of the selected side, in range [-1, 1].
   */
  constexpr void set_x(float x) {
    // Clamp before handing off to GamepadInputReport::set_joystick_axis().
    // That function casts (value * range + center) straight to uint8_t
    // WITHOUT clamping the float first -- a value even slightly outside
    // [-1, 1] (sensor overshoot, drift, calibration slop) casts a negative
    // float to an unsigned integer type, which is undefined behavior in
    // C++, not a safe wrap/clamp. Clamping here, before the call, avoids
    // ever handing it an out-of-range float.
    Base::set_joystick_axis(x_axis(), std::clamp(x, -1.0f, 1.0f));
  }

  /**
   * @brief Set only the Y axis of the selected side, in range [-1, 1].
   */
  constexpr void set_y(float y) { Base::set_joystick_axis(y_axis(), std::clamp(y, -1.0f, 1.0f)); }

  /**
   * @brief Set only the X axis of the selected side using raw 8-bit data.
   */
  constexpr void set_x(std::uint8_t x) { Base::set_joystick_axis(x_axis(), x); }

  /**
   * @brief Set only the Y axis of the selected side using raw 8-bit data.
   */
  constexpr void set_y(std::uint8_t y) { Base::set_joystick_axis(y_axis(), y); }

  /**
   * @brief Set a button.
   *
   * Button numbers are 1 through 12.
   */
  constexpr void set_button(int button_index, bool pressed) {
    Base::set_button(button_index, pressed);
  }

  /**
   * @brief Set the hat switch.
   */
  constexpr void set_hat(Hat hat) { Base::set_hat(hat); }

  /**
   * @brief Set the hat switch using directional inputs.
   */
  constexpr void set_hat(bool up, bool down, bool left, bool right) {
    Base::set_hat(up, down, left, right);
  }

  /**
   * @brief Get the serialized HID report.
   *
   * The report ID is not included.
   */
  constexpr auto get_report() const {
    const std::uint8_t *p = this->data() + 1; // only the report ID precedes the payload
    return std::vector<std::uint8_t>(p, p + Base::num_data_bytes);
  }

  /**
   * @brief Get the HID report descriptor.
   */
  static constexpr auto get_descriptor() {
    using namespace hid::page;
    using namespace hid::rdf;

    return descriptor(usage_page<generic_desktop>(),
                      usage(generic_desktop::GAMEPAD), // top-level collection usage
                      collection::application(
                          Base::get_descriptor() // report ID + axes/hat/buttons go inside here
                          ));
  }

private:
  constexpr std::size_t x_axis() const { return side_ == Side::LEFT ? 2 : 0; }
  constexpr std::size_t y_axis() const { return side_ == Side::LEFT ? 3 : 1; }

  Side side_{Side::RIGHT};
};

/**
 * @brief Convenience alias for the default XAC report.
 */
using XacGamepadInputReport = XacInputReport<1>;

} // namespace espp