// HID report descriptors versus the reports they describe. A small short-item
// walker (hid_descriptor_check.hpp) reads each descriptor and totals its Input
// bits per Report ID; the total must equal the number of data bytes the struct
// serializes, so a descriptor edit that drifts from get_report() is caught
// without a host or a device.
//
// The walker itself is checked first against hand-assembled descriptors,
// including the USB HID boot-protocol mouse from HID 1.11 appendix B.2.

#include <cstdint>
#include <vector>

#include "hid-rp-mouse.hpp"
#include "hid_descriptor_check.hpp"
#include "test_harness.hpp"
#include "xac.hpp"

using hid_check::DescriptorInfo;
using hid_check::parse_descriptor;

namespace {

DescriptorInfo walk(const std::vector<std::uint8_t> &bytes) {
  return parse_descriptor(bytes.data(), bytes.size());
}

} // namespace

// ---- the walker ------------------------------------------------------------

TEST(hid_walker_reads_the_spec_boot_mouse_descriptor) {
  const std::vector<std::uint8_t> boot_mouse = {
      0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19,
      0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
      0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15,
      0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0};
  const auto info = walk(boot_mouse);
  CHECK(info.parsed);
  CHECK_EQ(info.consumed, boot_mouse.size());
  CHECK(info.collections_balanced);
  CHECK_EQ(info.max_collection_depth, 2);
  CHECK(info.logical_ranges_ok);
  CHECK_EQ(info.input_bits.size(), std::size_t{1});
  CHECK_EQ(info.bits_for(0), std::size_t{3 + 5 + 16}); // buttons, padding, x and y
}

TEST(hid_walker_totals_input_bits_per_report_id) {
  const std::vector<std::uint8_t> d = {
      0x85, 0x01, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, // id 1: 2 x 8 bits
      0x85, 0x02, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02, // id 2: 4 x 1 bit
      0x85, 0x01, 0x75, 0x04, 0x95, 0x01, 0x81, 0x02, // id 1 again: + 4 bits
  };
  const auto info = walk(d);
  CHECK(info.parsed);
  CHECK_EQ(info.bits_for(1), std::size_t{20});
  CHECK_EQ(info.bits_for(2), std::size_t{4});
  CHECK_EQ(info.bits_for(0), std::size_t{0});
}

TEST(hid_walker_ignores_output_and_feature_items_for_input_bits) {
  const std::vector<std::uint8_t> d = {0x75, 0x08, 0x95, 0x02, 0x91, 0x02, 0xB1, 0x02};
  const auto info = walk(d);
  CHECK(info.parsed);
  CHECK(info.input_bits.empty());
}

TEST(hid_walker_follows_push_and_pop) {
  const std::vector<std::uint8_t> d = {
      0x75, 0x08, 0x95, 0x01, // size 8, count 1
      0xA4,                   // Push
      0x75, 0x01, 0x95, 0x02, // size 1, count 2
      0x81, 0x02,             // 2 bits
      0xB4,                   // Pop: back to 8 x 1
      0x81, 0x02,             // 8 bits
  };
  const auto info = walk(d);
  CHECK(info.parsed);
  CHECK_EQ(info.bits_for(0), std::size_t{10});
}

TEST(hid_walker_decodes_multi_byte_report_fields) {
  // Report Count with a two-byte value (0x0102 = 258) and a four-byte prefix code.
  const std::vector<std::uint8_t> d = {0x75, 0x01, 0x96, 0x02, 0x01, 0x81, 0x02};
  const auto info = walk(d);
  CHECK(info.parsed);
  CHECK_EQ(info.bits_for(0), std::size_t{258});

  const std::vector<std::uint8_t> four = {0x75, 0x01, 0x97, 0x10, 0x00, 0x00, 0x00, 0x81, 0x02};
  CHECK_EQ(walk(four).bits_for(0), std::size_t{16}); // bSize code 3 means 4 data bytes
}

TEST(hid_walker_treats_a_one_byte_logical_max_as_unsigned_when_min_is_non_negative) {
  // Logical 0..255 written with one-byte items: 0xFF would be -1 if signed.
  const std::vector<std::uint8_t> d = {0x15, 0x00, 0x25, 0xFF, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02};
  CHECK(walk(d).logical_ranges_ok);
}

TEST(hid_walker_flags_an_inverted_logical_range) {
  const std::vector<std::uint8_t> d = {0x15, 0x05, 0x25, 0x01, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02};
  const auto info = walk(d);
  CHECK(info.parsed);
  CHECK(!info.logical_ranges_ok);
}

TEST(hid_walker_does_not_check_the_range_of_constant_padding) {
  const std::vector<std::uint8_t> d = {0x15, 0x05, 0x25, 0x01, 0x75, 0x03, 0x95, 0x01, 0x81, 0x03};
  CHECK(walk(d).logical_ranges_ok);
}

TEST(hid_walker_rejects_long_items) {
  const std::vector<std::uint8_t> d = {0x75, 0x08, 0xFE, 0x02, 0x10, 0xAA, 0xBB};
  const auto info = walk(d);
  CHECK(!info.parsed);
  CHECK_EQ(info.consumed, std::size_t{2}); // stopped at the 0xFE
}

TEST(hid_walker_rejects_a_truncated_item) {
  const std::vector<std::uint8_t> d = {0x75, 0x08, 0x95}; // Report Count with no value
  const auto info = walk(d);
  CHECK(!info.parsed);
  CHECK_EQ(info.consumed, std::size_t{2});
}

TEST(hid_walker_reports_an_unclosed_collection) {
  const std::vector<std::uint8_t> d = {0xA1, 0x01, 0xA1, 0x00, 0xC0};
  const auto info = walk(d);
  CHECK(info.parsed);
  CHECK(!info.collections_balanced);
}

TEST(hid_walker_rejects_an_extra_end_collection) {
  const std::vector<std::uint8_t> d = {0xA1, 0x01, 0xC0, 0xC0};
  const auto info = walk(d);
  CHECK(!info.parsed);
  CHECK(!info.collections_balanced);
}

TEST(hid_walker_rejects_pop_without_push) {
  const std::vector<std::uint8_t> d = {0xB4};
  CHECK(!walk(d).parsed);
}

// ---- Xbox Adaptive Controller ----------------------------------------------

TEST(xac_descriptor_input_bits_match_the_report_payload) {
  using Report = espp::XacInputReport<1>;
  const auto desc = Report::get_descriptor();
  const auto info = parse_descriptor(desc);

  CHECK(info.parsed);
  CHECK_EQ(info.consumed, std::size_t{desc.size()});
  CHECK(info.collections_balanced);
  CHECK(info.logical_ranges_ok);
  CHECK_EQ(info.input_bits.size(), std::size_t{1}); // only report ID 1
  // num_data_bytes and get_report() exclude the leading Report ID byte, and the
  // Report ID item is not an Input field, so both sides count payload only.
  CHECK_EQ(info.bits_for(1), 8 * Report::num_data_bytes);
  CHECK_EQ(info.bits_for(1), 8 * Report().get_report().size());
}

TEST(xac_descriptor_nests_the_gamepad_physical_collections_in_one_application) {
  // XacInputReport wraps the espp gamepad descriptor (which opens physical
  // collections for the sticks/triggers and for the buttons) in an application
  // collection, so the deepest nesting is 2.
  const auto info = parse_descriptor(espp::XacInputReport<1>::get_descriptor());
  CHECK_EQ(info.max_collection_depth, 2);
}

TEST(xac_descriptor_uses_the_requested_report_id) {
  const auto info = parse_descriptor(espp::XacInputReport<7>::get_descriptor());
  CHECK(info.parsed);
  CHECK_EQ(info.input_bits.size(), std::size_t{1});
  CHECK_EQ(info.bits_for(7), 8 * espp::XacInputReport<7>::num_data_bytes);
}

// ---- Mouse -----------------------------------------------------------------

namespace {

/// Checks one MouseInputReport instantiation: the fragment descriptor and the
/// complete application descriptor both walk cleanly and describe exactly the
/// bytes get_report() serializes.
template <std::uint8_t ID, std::size_t BUTTONS, bool WHEEL> void check_mouse() {
  using Report = espp::MouseInputReport<ID, BUTTONS, WHEEL>;
  // get_report() excludes the report ID byte (see its doc comment), and
  // num_data_bytes is private, so get_report().size() is the payload size.
  const std::size_t payload_bits = 8 * Report().get_report().size();

  const auto fragment = Report::get_descriptor();
  const auto f = parse_descriptor(fragment);
  CHECK(f.parsed);
  CHECK_EQ(f.consumed, std::size_t{fragment.size()});
  CHECK(f.collections_balanced);
  CHECK(f.logical_ranges_ok);
  CHECK_EQ(f.input_bits.size(), std::size_t{1});
  CHECK_EQ(f.bits_for(ID), payload_bits);

  const auto app = espp::mouse_app_report_descriptor<ID, BUTTONS, WHEEL>();
  const auto a = parse_descriptor(app);
  CHECK(a.parsed);
  CHECK_EQ(a.consumed, std::size_t{app.size()});
  CHECK(a.collections_balanced);
  CHECK_EQ(a.max_collection_depth, 2); // application { physical { ... } }
  CHECK(a.logical_ranges_ok);
  CHECK_EQ(a.bits_for(ID), payload_bits);
}

} // namespace

TEST(mouse_boot_descriptor_matches_its_report) { check_mouse<0, 3, false>(); }
TEST(mouse_wheel_descriptor_matches_its_report) { check_mouse<0, 3, true>(); }
TEST(mouse_report_id_descriptor_matches_its_report) { check_mouse<1, 3, false>(); }
TEST(mouse_report_id_wheel_descriptor_matches_its_report) { check_mouse<1, 3, true>(); }
TEST(mouse_eight_button_descriptor_matches_its_report) { check_mouse<0, 8, false>(); }
TEST(mouse_nine_button_descriptor_matches_its_report) { check_mouse<0, 9, true>(); }
TEST(mouse_five_button_report_id_descriptor_matches_its_report) { check_mouse<2, 5, true>(); }

TEST(mouse_boot_descriptor_has_the_boot_protocol_input_layout) {
  // 3 button bits + 5 padding bits + 2 x 8 bits, no report ID.
  const auto info = parse_descriptor(espp::mouse_app_report_descriptor<0, 3, false>());
  CHECK_EQ(info.bits_for(0), std::size_t{24});
  CHECK_EQ(info.input_bits.size(), std::size_t{1});
}
