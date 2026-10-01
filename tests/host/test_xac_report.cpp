// XacInputReport: the bytes get_report() hands to write_hid_report() must be
// laid out in the order the report descriptor declares them.
//
// Built against the real espp hid-rp headers at the version pinned in
// dependencies.lock, never managed_components/, so a local patch there cannot
// make these pass.

#include <cstdint>
#include <vector>

#include "test_harness.hpp"
#include "xac.hpp"

using Report = espp::XacGamepadInputReport;

namespace {

// Descriptor order (hid-rp-gamepad.hpp get_descriptor): X, Y, Z, RZ, brake,
// accelerator, hat (low nibble), 12 buttons (2 bytes), consumer record (bit 0).
constexpr std::size_t X = 0, Y = 1, Z = 2, RZ = 3, BRAKE = 4, ACCEL = 5, HAT = 6, BUTTONS_LO = 7,
                      BUTTONS_HI = 8, CONSUMER = 9;

} // namespace

TEST(xac_report_size_matches_descriptor) {
  Report r;
  CHECK_EQ(Report::num_data_bytes, std::size_t{10});
  CHECK_EQ(r.get_report().size(), Report::num_data_bytes);
}

TEST(xac_report_bytes_follow_descriptor_order) {
  Report r;
  r.set_joystick(std::uint8_t{0xA1}, std::uint8_t{0xA2});
  r.set_joystick_axis(2, std::uint8_t{0xA3});
  r.set_joystick_axis(3, std::uint8_t{0xA4});
  r.set_trigger_axis(0, std::uint8_t{0xB1});
  r.set_trigger_axis(1, std::uint8_t{0xB2});
  r.set_hat(Report::Hat::UP_RIGHT);
  r.set_button(1, true);
  r.set_button(12, true);
  r.set_consumer_record(true);

  const auto bytes = r.get_report();
  CHECK_EQ(bytes.size(), Report::num_data_bytes);
  CHECK_EQ(int(bytes[X]), 0xA1);
  CHECK_EQ(int(bytes[Y]), 0xA2);
  CHECK_EQ(int(bytes[Z]), 0xA3);
  CHECK_EQ(int(bytes[RZ]), 0xA4);
  CHECK_EQ(int(bytes[BRAKE]), 0xB1);
  CHECK_EQ(int(bytes[ACCEL]), 0xB2);
  CHECK_EQ(int(bytes[HAT] & 0x0f), int(Report::Hat::UP_RIGHT));
  CHECK_EQ(int(bytes[BUTTONS_LO]), 0x01);
  CHECK_EQ(int(bytes[BUTTONS_HI]), 0x08);
  CHECK_EQ(int(bytes[CONSUMER] & 0x01), 1);
}

TEST(xac_centered_report_is_neutral) {
  Report r;
  r.set_joystick(0.0f, 0.0f);

  const auto bytes = r.get_report();
  CHECK_EQ(bytes.size(), Report::num_data_bytes);
  const int center = Report::joystick_center;
  CHECK_EQ(int(bytes[X]), center);
  CHECK_EQ(int(bytes[Y]), center);
  CHECK_EQ(int(bytes[Z]), center);
  CHECK_EQ(int(bytes[RZ]), center);
  CHECK_EQ(int(bytes[BRAKE]), 0);
  CHECK_EQ(int(bytes[ACCEL]), 0);
  // 0x0f is outside the hat's 1..8 logical range: the null state, no D-pad.
  CHECK_EQ(int(bytes[HAT] & 0x0f), int(Report::Hat::CENTERED));
  CHECK_EQ(int(bytes[BUTTONS_LO]), 0);
  CHECK_EQ(int(bytes[BUTTONS_HI] & 0x0f), 0);
}

TEST(xac_stick_x_moves_report_x) {
  // Full deflection lands at center +/- range, not at joystick_max/min: the base
  // report computes center = 255 / 2 = 127 and range = 255 / 2 = 127, so +1.0f
  // maps to 254 and -1.0f to 0.

  Report centered;
  centered.set_joystick(0.0f, 0.0f);
  Report right;
  right.set_joystick(1.0f, 0.0f);
  Report up;
  up.set_joystick(0.0f, -1.0f);

  const auto c = centered.get_report();
  const auto rx = right.get_report();
  const auto uy = up.get_report();
  CHECK_EQ(int(rx[X]), int(Report::joystick_center + Report::joystick_range));
  CHECK_EQ(int(rx[Y]), int(c[Y]));
  CHECK_EQ(int(uy[Y]), int(Report::joystick_center - Report::joystick_range));
  CHECK_EQ(int(uy[X]), int(c[X]));
}
