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

  enum class Stick : uint8_t { LEFT = 0, RIGHT = 1 };

  constexpr XacInputReport() { reset(); }

  /**
   * @brief Reset the joystick to center and all controls to neutral.
   */
  constexpr void reset() {
    Base::reset();
    Base::set_left_trigger(0.0f);
    Base::set_right_trigger(0.0f);
  }

  /**
   * @brief Set the joystick position.
   *
   * @param x X position in range [-1, 1]
   * @param y Y position in range [-1, 1]
   */

  constexpr void set_stick(Stick s) { stick_ = s; }

  constexpr void set_joystick(float x, float y) {
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, -1.0f, 1.0f);
    if (stick_ == Stick::LEFT) {
      Base::set_left_joystick(x, y);
      Base::set_right_joystick(0.0f, 0.0f); // idle stick stays centered
    } else {
      Base::set_right_joystick(x, y);
      Base::set_left_joystick(0.0f, 0.0f);
    }
  }
  constexpr void set_joystick(std::uint8_t x, std::uint8_t y) {
    if (stick_ == Stick::LEFT) {
      Base::set_left_joystick(x, y);
      Base::set_right_joystick(0.0f, 0.0f); // idle stick stays centered
    } else {
      Base::set_right_joystick(x, y);
      Base::set_left_joystick(0.0f, 0.0f);
    }
  }

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

  /**
   * @brief Get the HID report descriptor.
   */
  constexpr auto get_report() const { return Base::get_report(); }
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
  Stick stick_{Stick::RIGHT};
};

/**
 * @brief Convenience alias for the default XAC report.
 */

using XacGamepadInputReport = XacInputReport<1>;
} // namespace espp