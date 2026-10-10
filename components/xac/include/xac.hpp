#pragma once

#include <algorithm>
#include <cstdint>

#include "hid-rp-gamepad.hpp"
// TODO: add config option for chosing L or R port joystick-- default to R or something
//  untested code

namespace espp {

/**
 * @brief Xbox Adaptive Controller joystick input report.
 *
 * Uses the standard ESPP GamepadInputReport HID format.
 *
 * Physical controls exposed by this device:
 *
 *   Left joystick:
 *     X = joystick X
 *     Y = joystick Y
 *
 *   Right joystick:
 *     Z/RZ remain centered
 *
 *   Triggers:
 *     remain released
 *
 *   Hat:
 *     available
 *
 *   Buttons:
 *     12 buttons available
 *
 * The device therefore behaves as a single physical joystick while
 * retaining the standard GamepadInputReport HID descriptor.
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

  constexpr XacInputReport() { reset(); }

  /**
   * @brief Reset the joystick to center and all controls to neutral.
   */
  constexpr void reset() {
    Base::reset();

    // The XAC only uses our physical left joystick.
    //
    // Keep the unused right joystick centered.
    // Base::set_right_joystick(0.0f, 0.0f);
    // Base::set_left_joystick(0.0f, 0.0f);
    // Keep both triggers released.
    Base::set_left_trigger(0.0f);
    Base::set_right_trigger(0.0f);
  }

  /**
   * @brief Set the joystick position.
   *
   * @param x X position in range [-1, 1]
   * @param y Y position in range [-1, 1]
   */
  constexpr void set_joystick(float x, float y) {
    // Clamp before handing off to GamepadInputReport::set_joystick_axis().
    // That function casts (value * range + center) straight to uint8_t
    // WITHOUT clamping the float first -- a value even slightly outside
    // [-1, 1] (sensor overshoot, drift, calibration slop) casts a negative
    // float to an unsigned integer type, which is undefined behavior in
    // C++, not a safe wrap/clamp. Clamping here, before the call, avoids
    // ever handing it an out-of-range float.
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, -1.0f, 1.0f);
    // Base::set_right_joystick(x, y);
    // Base::set_left_joystick(x, y);
    Base::set_joystick_axis(0, x); // d-pad or nothing
    Base::set_joystick_axis(1, y); // right stick x, left dpad
    Base::set_joystick_axis(2, x); // right stick y
    Base::set_joystick_axis(3, y); // left x, right d-pad
  }
  constexpr void set_joystick(std::uint8_t x, std::uint8_t y) {
    Base::set_joystick_axis(0, x);
    Base::set_joystick_axis(1, y);
  }
  constexpr void set_x(float x) { Base::set_joystick_axis(0, std::clamp(x, -1.0f, 1.0f)); }
  constexpr void set_y(float y) { Base::set_joystick_axis(1, std::clamp(y, -1.0f, 1.0f)); }
  /**
   * @brief Set only the X axis using raw 8-bit data.
   */
  constexpr void set_x(std::uint8_t x) { Base::set_joystick_axis(2, x); }

  /**
   * @brief Set only the Y axis using raw 8-bit data.
   */
  constexpr void set_y(std::uint8_t y) { Base::set_joystick_axis(3, y); }

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
};

/**
 * @brief Convenience alias for the default XAC report.
 */
using XacGamepadInputReport = XacInputReport<1>;

} // namespace espp