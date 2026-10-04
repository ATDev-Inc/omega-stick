#pragma once

#include <algorithm>
#include <cstdint>

#include "hid-rp-gamepad.hpp"

// Not yet verified on Xbox Adaptive Controller hardware.
//
// The shape here follows the external USB joysticks that are known to work on
// the XAC (controllercustom/dinput_pluggable and dinput_tinyusb): a Generic
// Desktop / Joystick application collection, unsigned 8-bit axes (0..255), a
// 4-bit hat and 12 buttons. Those references also settle two questions that
// come up a lot: a report ID is fine, and a CDC interface next to the HID
// interface is fine. The Leonardo build of dinput_pluggable has both.

namespace espp {

/**
 * @brief Xbox Adaptive Controller joystick input report.
 *
 * Uses the standard ESPP GamepadInputReport HID format with 8-bit axes.
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
 * How the XAC (firmware June 2024 or newer) maps the report:
 *
 *   X/Y     -> left thumbstick
 *   Z/RZ    -> right thumbstick
 *   hat     -> d-pad
 *   buttons -> XAC buttons (the default assignment is odd; remap in the Xbox
 *              Accessories app)
 *
 * The triggers and the consumer Record usage are part of the espp layout but
 * the XAC ignores them.
 *
 * The device therefore behaves as a single physical joystick while
 * retaining the standard GamepadInputReport HID descriptor.
 *
 * GamepadInputReport::get_descriptor() is only the field list; use
 * xac_app_report_descriptor() for the descriptor handed to USB.
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
    Base::set_right_joystick(0.0f, 0.0f);

    // Keep both triggers released.
    Base::set_left_trigger(0.0f);
    Base::set_right_trigger(0.0f);
  }

  /**
   * @brief Set the joystick position.
   *
   * Values outside [-1, 1] are clamped. GamepadInputReport's float setter
   * casts to the 8-bit axis type before it clamps, so without this a value
   * even slightly past full scale would wrap to the far side of the range.
   *
   * @param x X position in range [-1, 1]
   * @param y Y position in range [-1, 1]
   */
  constexpr void set_joystick(float x, float y) {
    Base::set_left_joystick(clamp_unit(x), clamp_unit(y));
  }

  /**
   * @brief Set the joystick position using raw 8-bit values.
   *
   * @param x X position in range [0, 255]
   * @param y Y position in range [0, 255]
   */
  constexpr void set_joystick(std::uint8_t x, std::uint8_t y) {
    Base::set_joystick_axis(0, x);
    Base::set_joystick_axis(1, y);
  }

  /**
   * @brief Set only the X axis.
   *
   * @param x X position in range [-1, 1] (clamped)
   */
  constexpr void set_x(float x) { Base::set_joystick_axis(0, clamp_unit(x)); }

  /**
   * @brief Set only the Y axis.
   *
   * @param y Y position in range [-1, 1] (clamped)
   */
  constexpr void set_y(float y) { Base::set_joystick_axis(1, clamp_unit(y)); }

  /**
   * @brief Set only the X axis using raw 8-bit data.
   */
  constexpr void set_x(std::uint8_t x) { Base::set_joystick_axis(0, x); }

  /**
   * @brief Set only the Y axis using raw 8-bit data.
   */
  constexpr void set_y(std::uint8_t y) { Base::set_joystick_axis(1, y); }

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
  constexpr auto get_report() const { return Base::get_report(); }

  /**
   * @brief Get the HID report descriptor fragment.
   *
   * This is the field list only. See xac_app_report_descriptor() for the
   * complete descriptor.
   */
  static constexpr auto get_descriptor() { return Base::get_descriptor(); }

private:
  static constexpr float clamp_unit(float v) { return std::clamp(v, -1.0f, 1.0f); }
};

/**
 * @brief Convenience alias for the default XAC report.
 */
using XacGamepadInputReport = XacInputReport<1>;

/// Build a complete report descriptor around an XacInputReport.
///
/// GamepadInputReport::get_descriptor() is only the field list. A host needs
/// it inside an application collection with a usage page and usage, or it
/// rejects the HID interface before it ever looks at a report. This is the
/// same wrapping hid-rp-mouse does in mouse_app_report_descriptor().
///
/// Usage is Joystick (0x04) rather than Gamepad (0x05) because that is what
/// the XAC references that are confirmed working declare.
///
/// \tparam REPORT_ID The report ID of the input report; must match the
///         XacInputReport the reports are built from.
/// \return The complete report descriptor.
template <uint8_t REPORT_ID = 1> static constexpr auto xac_app_report_descriptor() {
  using namespace hid::page;
  using namespace hid::rdf;

  // clang-format off
  return descriptor(
      usage_page<generic_desktop>(),
      usage(generic_desktop::JOYSTICK),
      collection::application(
          XacInputReport<REPORT_ID>::get_descriptor()
      )
  );
  // clang-format on
}

} // namespace espp
