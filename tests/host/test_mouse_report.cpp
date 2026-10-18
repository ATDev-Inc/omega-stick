// espp::MouseInputReport (components/hid-rp-mouse): delta clamping, button
// bounds, reset semantics, and that get_report() bytes follow the order the
// report descriptor declares (buttons, X, Y, wheel).
//
// num_data_bytes is private in the class, so sizes are checked through
// get_report().size().

#include <cstdint>
#include <limits>
#include <vector>

#include "hid-rp-mouse.hpp"
#include "test_harness.hpp"

using Boot = espp::MouseInputReport<0, 3, false>;
using Wheel = espp::MouseInputReport<0, 3, true>;
using Id1 = espp::MouseInputReport<1, 3, false>;
using Id1Wheel = espp::MouseInputReport<1, 3, true>;
using Nine = espp::MouseInputReport<0, 9, true>;

namespace {

std::vector<std::uint8_t> bytes(std::initializer_list<int> v) {
  std::vector<std::uint8_t> out;
  for (int b : v) {
    out.push_back(static_cast<std::uint8_t>(b));
  }
  return out;
}

} // namespace

// ---- clamp_delta -----------------------------------------------------------

TEST(mouse_deltas_pass_through_inside_the_limits) {
  Wheel r;
  r.set_movement(0, 0);
  r.set_wheel(0);
  CHECK_EQ(int(r.get_x()), 0);
  r.set_movement(127, -127);
  r.set_wheel(-127);
  CHECK_EQ(int(r.get_x()), 127);
  CHECK_EQ(int(r.get_y()), -127);
  CHECK_EQ(int(r.get_wheel()), -127);
  r.set_movement(-5, 42);
  CHECK_EQ(int(r.get_x()), -5);
  CHECK_EQ(int(r.get_y()), 42);
}

TEST(mouse_deltas_saturate_at_plus_minus_127) {
  Wheel r;
  r.set_x(128);
  r.set_y(-128); // the symmetric limit is -127, not int8_t's -128
  r.set_wheel(5000);
  CHECK_EQ(int(r.get_x()), 127);
  CHECK_EQ(int(r.get_y()), -127);
  CHECK_EQ(int(r.get_wheel()), 127);
  r.set_wheel(-5000);
  CHECK_EQ(int(r.get_wheel()), -127);
}

TEST(mouse_deltas_saturate_at_the_int_extremes) {
  Wheel r;
  r.set_movement(std::numeric_limits<int>::max(), std::numeric_limits<int>::min());
  r.set_wheel(std::numeric_limits<int>::min());
  CHECK_EQ(int(r.get_x()), 127);
  CHECK_EQ(int(r.get_y()), -127);
  CHECK_EQ(int(r.get_wheel()), -127);
}

// ---- buttons ---------------------------------------------------------------

TEST(mouse_set_button_accepts_only_indices_1_through_button_count) {
  Boot r;
  CHECK(r.set_button(1, true));
  CHECK(r.set_button(2, true));
  CHECK(r.set_button(3, true));
  CHECK(!r.set_button(0, true));
  CHECK(!r.set_button(4, true));
  CHECK(!r.set_button(-1, true));
  CHECK(!r.set_button(1000, true));
}

TEST(mouse_set_button_rejects_indices_that_wrap_into_range) {
  Boot r;
  CHECK(!r.set_button(65536 + 1, true));
  CHECK(!r.get_button(1));
}

TEST(mouse_out_of_range_set_button_does_not_change_the_report) {
  Boot r;
  r.set_button(2, true);
  const auto before = r.get_report();
  r.set_button(0, true);
  r.set_button(4, true);
  r.set_button(-3, false);
  CHECK(r.get_report() == before);
}

TEST(mouse_buttons_set_and_clear_independently) {
  Boot r;
  r.set_button(1, true);
  r.set_button(3, true);
  CHECK(r.get_button(1));
  CHECK(!r.get_button(2));
  CHECK(r.get_button(3));
  r.set_button(1, false);
  CHECK(!r.get_button(1));
  CHECK(r.get_button(3));
}

// ---- reset -----------------------------------------------------------------

TEST(mouse_reset_movement_keeps_the_buttons) {
  Wheel r;
  r.set_button(1, true);
  r.set_button(2, true);
  r.set_movement(10, -20);
  r.set_wheel(3);
  r.reset_movement();
  CHECK_EQ(int(r.get_x()), 0);
  CHECK_EQ(int(r.get_y()), 0);
  CHECK_EQ(int(r.get_wheel()), 0);
  CHECK(r.get_button(1));
  CHECK(r.get_button(2));
  CHECK(!r.get_button(3));
}

TEST(mouse_reset_clears_buttons_and_movement) {
  Wheel r;
  r.set_button(3, true);
  r.set_movement(1, 2);
  r.set_wheel(3);
  r.reset();
  CHECK(r.get_report() == bytes({0, 0, 0, 0}));
}

// ---- report size -----------------------------------------------------------

TEST(mouse_report_size_depends_on_wheel_and_button_count) {
  CHECK_EQ(Boot().get_report().size(), std::size_t{3});  // buttons, x, y
  CHECK_EQ(Wheel().get_report().size(), std::size_t{4}); // + wheel
  CHECK_EQ(Nine().get_report().size(), std::size_t{5});  // 9 buttons need 2 bytes
  CHECK_EQ((espp::MouseInputReport<0, 8, false>().get_report().size()), std::size_t{3});
  CHECK_EQ((espp::MouseInputReport<0, 1, false>().get_report().size()), std::size_t{3});
}

TEST(mouse_report_size_excludes_the_report_id_byte) {
  CHECK_EQ(Id1().get_report().size(), std::size_t{3});
  CHECK_EQ(Id1Wheel().get_report().size(), std::size_t{4});
  // The object itself does carry the id as its first byte.
  Id1 r;
  CHECK_EQ(int(r.data()[0]), 1);
  CHECK_EQ(int(Id1Wheel().data()[0]), 1);
}

// ---- byte order ------------------------------------------------------------

TEST(mouse_boot_report_bytes_are_buttons_x_y) {
  Boot r;
  r.set_button(1, true);
  r.set_button(3, true);
  r.set_movement(10, -10);
  CHECK(r.get_report() == bytes({0x05, 10, 0xF6}));
}

TEST(mouse_wheel_report_bytes_are_buttons_x_y_wheel) {
  Wheel r;
  r.set_button(2, true);
  r.set_movement(-1, 7);
  r.set_wheel(-5);
  CHECK(r.get_report() == bytes({0x02, 0xFF, 7, 0xFB}));
}

TEST(mouse_report_with_an_id_has_the_same_payload_bytes) {
  Id1Wheel r;
  r.set_button(1, true);
  r.set_movement(3, 4);
  r.set_wheel(5);
  CHECK(r.get_report() == bytes({0x01, 3, 4, 5}));
}

TEST(mouse_wheel_is_not_serialized_when_the_report_has_no_wheel) {
  Boot r;
  r.set_movement(1, 2);
  r.set_wheel(99);
  CHECK(r.get_report() == bytes({0, 1, 2}));
}

TEST(mouse_nine_buttons_spill_into_a_second_byte_before_x) {
  Nine r;
  r.set_button(1, true);
  r.set_button(9, true);
  r.set_movement(5, 6);
  r.set_wheel(7);
  CHECK(r.get_report() == bytes({0x01, 0x01, 5, 6, 7}));
}
