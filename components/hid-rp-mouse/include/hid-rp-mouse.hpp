#pragma once

#include "hid-rp.hpp"

namespace espp {

/// @brief HID Mouse Input Report
/// @tparam REPORT_ID The report ID of the input report. Use 0 for the HID boot
///         protocol layout (3 buttons, no report ID, no wheel).
/// @tparam BUTTON_COUNT The number of buttons in the report. The boot protocol
///         layout uses 3.
/// @tparam WITH_WHEEL Whether to include an 8-bit relative wheel (scroll) axis
///         in the report, after X/Y. Defaults to false so the plain boot
///         protocol layout (buttons + x + y, 3 bytes total) is unaffected.
///         Set to true to get a 4th byte carrying relative vertical scroll,
///         e.g. for nudge-click scrolling.
///
/// This class implements a HID mouse input report with a configurable number of
/// buttons, 8-bit relative X/Y movement, and an optional 8-bit relative wheel
/// axis. It supports setting the buttons, movement, and wheel, serializing the
/// input report, and getting the report descriptor.
template <uint8_t REPORT_ID = 0, size_t BUTTON_COUNT = 3, bool WITH_WHEEL = false>
class MouseInputReport : public hid::report::base<hid::report::type::INPUT, REPORT_ID> {
  static_assert(BUTTON_COUNT >= 1, "a mouse needs at least one button");

  // the buttons occupy BUTTON_COUNT bits, padded out to a whole number of bytes
  static constexpr size_t button_bytes = (BUTTON_COUNT + 7) / 8;
  static constexpr size_t wheel_bytes = WITH_WHEEL ? 1 : 0;
  static constexpr size_t num_data_bytes = button_bytes + 2 + wheel_bytes; // + x + y (+ wheel)

  hid::report_bitset<hid::page::button, hid::page::button(1), hid::page::button(BUTTON_COUNT)>
      buttons;
  std::int8_t x_{};
  std::int8_t y_{};
  std::int8_t wheel_{}; // only serialized/reported when WITH_WHEEL is true

public:
  constexpr MouseInputReport() = default;

  /// Reset the buttons and the movement deltas (including wheel, if present)
  constexpr void reset() {
    buttons.reset();
    reset_movement();
  }

  /// Reset only the movement deltas (x, y, and wheel), leaving the button
  /// state alone.
  /// \note Call this after each report is sent, since the deltas are relative.
  constexpr void reset_movement() {
    x_ = 0;
    y_ = 0;
    wheel_ = 0;
  }

  /// Set a button's state
  /// \param button_index The 1-based button index (1 = left, 2 = right, 3 = middle).
  /// \param value The true/false value you want to set the button to.
  /// \return true if the button index was in range and was set.
  constexpr bool set_button(int button_index, bool value) {
    return buttons.set(hid::page::button(button_index), value);
  }

  /// Get a button's state
  /// \param button_index The 1-based button index.
  /// \return The true/false state of that button.
  bool get_button(int button_index) const { return buttons.test(hid::page::button(button_index)); }

  /// Set the relative movement for this report
  /// \param x The relative x movement, clamped to [-127, 127].
  /// \param y The relative y movement, clamped to [-127, 127].
  constexpr void set_movement(int x, int y) {
    set_x(x);
    set_y(y);
  }

  /// Set the relative x movement, clamped to [-127, 127]
  constexpr void set_x(int x) { x_ = clamp_delta(x); }

  /// Set the relative y movement, clamped to [-127, 127]
  constexpr void set_y(int y) { y_ = clamp_delta(y); }

  /// Get the relative x movement
  constexpr std::int8_t get_x() const { return x_; }

  /// Get the relative y movement
  constexpr std::int8_t get_y() const { return y_; }

  /// Set the relative wheel (scroll) movement, clamped to [-127, 127].
  /// Positive values scroll up, negative values scroll down (matches the
  /// HID generic desktop "Wheel" usage convention).
  /// \note Only meaningful when WITH_WHEEL is true; the value is simply
  ///       unused (and never serialized) otherwise.
  constexpr void set_wheel(int wheel) { wheel_ = clamp_delta(wheel); }

  /// Get the relative wheel movement
  constexpr std::int8_t get_wheel() const { return wheel_; }

  /// Get the input report as a vector of bytes
  /// \return The input report as a vector of bytes.
  /// \note The report id is not included in the returned vector.
  auto get_report() const {
    // if we have a report id, the first byte is the id, which we don't want
    size_t offset = (REPORT_ID != 0) ? 1 : 0;
    auto report_data = this->data() + offset;
    return std::vector<uint8_t>(report_data, report_data + num_data_bytes);
  }

  /// Get the report descriptor as a hid::rdf::descriptor
  /// \return The report descriptor as a hid::rdf::descriptor.
  /// \note This is an incomplete descriptor, you will need to add it to a
  ///      collection::application descriptor to create a complete report descriptor.
  ///      \code{.cpp}
  ///      using namespace hid::page;
  ///      using namespace hid::rdf;
  ///      auto mouse_descriptor = MouseInputReport<0, 3>::get_descriptor();
  ///      auto rdf_descriptor = descriptor(
  ///          usage_page<generic_desktop>(),
  ///          usage(generic_desktop::MOUSE),
  ///          collection::application(
  ///              usage(generic_desktop::POINTER),
  ///              collection::physical(
  ///                  mouse_descriptor
  ///              )
  ///          )
  ///      );
  ///      auto descriptor = std::vector<uint8_t>(rdf_descriptor.begin(), rdf_descriptor.end());
  ///      \endcode
  static constexpr auto get_descriptor() {
    using namespace hid::page;
    using namespace hid::rdf;

    // clang-format off
    if constexpr (WITH_WHEEL) {
      return descriptor(
          conditional_report_id<REPORT_ID>(),

          // buttons
          usage_page<button>(),
          usage_limits(button(1), button(BUTTON_COUNT)),
          logical_limits<1, 1>(0, 1),
          report_count(BUTTON_COUNT),
          report_size(1),
          input::absolute_variable(),
          input::byte_padding<BUTTON_COUNT>(),

          // relative movement
          usage_page<generic_desktop>(),
          usage(generic_desktop::X),
          usage(generic_desktop::Y),
          logical_limits<1, 1>(-127, 127),
          report_count(2),
          report_size(8),
          input::relative_variable(),

          // relative wheel (scroll)
          usage(generic_desktop::WHEEL),
          logical_limits<1, 1>(-127, 127),
          report_count(1),
          report_size(8),
          input::relative_variable()
      );
    } else {
      return descriptor(
          conditional_report_id<REPORT_ID>(),

          // buttons
          usage_page<button>(),
          usage_limits(button(1), button(BUTTON_COUNT)),
          logical_limits<1, 1>(0, 1),
          report_count(BUTTON_COUNT),
          report_size(1),
          input::absolute_variable(),
          input::byte_padding<BUTTON_COUNT>(),

          // relative movement
          usage_page<generic_desktop>(),
          usage(generic_desktop::X),
          usage(generic_desktop::Y),
          logical_limits<1, 1>(-127, 127),
          report_count(2),
          report_size(8),
          input::relative_variable()
      );
    }
    // clang-format on
  }

private:
  static constexpr std::int8_t clamp_delta(int v) {
    if (v > 127) {
      return 127;
    }
    if (v < -127) {
      return -127;
    }
    return static_cast<std::int8_t>(v);
  }
};

/// Convenience alias for the HID boot protocol mouse layout
using MouseBootInputReport = MouseInputReport<0, 3, false>;

// the boot protocol layout must be exactly 3 bytes: buttons, x, y
static_assert(sizeof(MouseBootInputReport) == 3 || sizeof(MouseBootInputReport) == 4,
              "boot protocol mouse report must be 3 bytes");

/// Convenience alias for a mouse report that additionally carries a relative
/// wheel (scroll) axis, e.g. for nudge-click scrolling. This is one byte
/// larger than the boot protocol layout (buttons, x, y, wheel) and is not a
/// strict HID boot protocol mouse.
using MouseWithWheelInputReport = MouseInputReport<0, 3, true>;

/// Build a complete mouse report descriptor around a MouseInputReport
/// \tparam REPORT_ID The report ID of the input report.
/// \tparam BUTTON_COUNT The number of buttons in the report.
/// \tparam WITH_WHEEL Whether to include a relative wheel axis (see
///         MouseInputReport).
/// \return The complete report descriptor.
template <uint8_t REPORT_ID = 0, size_t BUTTON_COUNT = 3, bool WITH_WHEEL = false>
static constexpr auto mouse_app_report_descriptor() {
  using namespace hid::page;
  using namespace hid::rdf;

  // clang-format off
  return descriptor(
      usage_page<generic_desktop>(),
      usage(generic_desktop::MOUSE),
      collection::application(
          usage(generic_desktop::POINTER),
          collection::physical(
              MouseInputReport<REPORT_ID, BUTTON_COUNT, WITH_WHEEL>::get_descriptor()
          )
      )
  );
  // clang-format on
}

/// @defgroup mouse_high_res_scroll Mouse High Resolution Scrolling
/// @brief    Utilities for using high-resolution scrolling in mouse applications
/// @details  High-resolution scrolling allows a host OS to activate this feature on a mouse.
///           The specification is written by Microsoft:
///           https://learn.microsoft.com/en-us/previous-versions/windows/hardware/design/dn613912(v=vs.85)
///           Windows (since Vista) and Linux (since 5.0) have support, not without issues though
/// @warning  This feature has the following problems on Linux:
///           - The mouse must use report IDs, otherwise the kernel fails to enable it,
///             see: https://bugzilla.kernel.org/show_bug.cgi?id=220144
///           - The resolution multiplier isn't set on the device after a
///             suspend/resume cycle, see: https://bugzilla.kernel.org/show_bug.cgi?id=203421
/// @warning  This feature has the following problems on Windows:
///           - The resolution multiplier isn't set on the device after a Fast startup boot,
///             or after a suspend/resume cycle.
/// @{

static constexpr uint8_t resolution_multiplier_bit_size() {
  // https://github.com/qmk/qmk_firmware/issues/17585#issuecomment-1238023671
  return 2;
}

/// @brief  Defines the descriptor items for a HID control to associate a resolution multiplier
///         value with it in a HID feature report.
/// @note   This descriptor block is assuming the current usage page is generic_desktop
/// @note   This item only takes two bits in the feature report, no byte padding
/// @tparam MULTIPLIER_MAX: the maximum value of the resolution multiplier (valid range is 1-120)
/// @return the HID report descriptor items
template <uint8_t MULTIPLIER_MAX> static constexpr auto resolution_multiplier() {
  using namespace hid::page;
  using namespace hid::rdf;

  // clang-format off
  return descriptor(
      push_globals(),
      usage(generic_desktop::RESOLUTION_MULTIPLIER),
      logical_limits<1, 1>(0, 1),
      physical_limits<1, 1>(1, MULTIPLIER_MAX),
      report_count(1),
      report_size(resolution_multiplier_bit_size()),
      feature::absolute_variable(),
      pop_globals()
  );
  // clang-format on
}

/// @brief  A HID feature report storing the resolution multiplier values for vertical and
///         horizontal scrolling.
/// @tparam MULTIPLIER_MAX: the maximum value of the resolution multiplier (valid range is 1-120)
/// @tparam REPORT_ID: the report ID of the feature report, don't use 0 for Linux compatibility:
///         https://bugzilla.kernel.org/show_bug.cgi?id=220144
template <uint8_t MULTIPLIER_MAX, uint8_t REPORT_ID>
struct ResolutionMultiplierReport
    : public hid::report::base<hid::report::type::FEATURE, REPORT_ID> {
  std::uint8_t resolutions{};

  constexpr void reset() { resolutions = 0; }
  bool high_resolution() const { return resolutions != 0; }

  constexpr uint8_t vertical_scroll_multiplier() const {
    return ((resolutions & 0x01) != 0) ? MULTIPLIER_MAX : 1;
  }
  constexpr uint8_t horizontal_scroll_multiplier() const {
    return ((resolutions & 0x04) != 0) ? MULTIPLIER_MAX : 1;
  }
};

/// @brief  Creates the descriptor block for high resolution scrolling, that is to be inserted
///         into a mouse pointer collection.
/// @note   This descriptor block is assuming the current usage page is generic_desktop
/// @tparam MAX_SCROLL: the maximum scroll value for Wheel and AC Pan usages
/// @tparam MULTIPLIER_MAX: the maximum value of the resolution multiplier (valid range is 1-120)
/// @return the descriptor block
template <int16_t MAX_SCROLL, uint8_t MULTIPLIER_MAX>
static constexpr auto high_resolution_scrolling() {
  using namespace hid::page;
  using namespace hid::rdf;
  constexpr uint8_t SCROLL_BYTES = MAX_SCROLL > std::numeric_limits<int8_t>::max() ? 2 : 1;

  // clang-format off
  return descriptor(
      collection::logical(
          usage(generic_desktop::WHEEL),
          logical_limits<SCROLL_BYTES>(-MAX_SCROLL, MAX_SCROLL),
          report_count(1),
          report_size(SCROLL_BYTES * 8),
          input::relative_variable(),
          resolution_multiplier<MULTIPLIER_MAX>()
      ),
      collection::logical(
          usage_extended(consumer::AC_PAN),
          // skip repeating the same global items as the Wheel usage
          input::relative_variable(),
          resolution_multiplier<MULTIPLIER_MAX>()
      ),
      feature::byte_padding<resolution_multiplier_bit_size() * 2>()
  );
  // clang-format on
}

/// @}

} // namespace espp