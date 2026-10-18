// espp::TMAG5273 driver, exercised through a fake I2C bus: a 256-byte register
// file behind the write / read_register callbacks BasePeripheral expects
// (espp base_peripheral 1.3.3, UseAddress = true, so every callback gets the
// 7-bit device address first).
//
// Expected register values are derived from the register and bit constants in
// tmag5273.hpp. Bit positions are the driver's own; they are not cross-checked
// against the TI datasheet here.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <system_error>
#include <vector>

#include "base_component.hpp"
#include "base_peripheral.hpp"
#include "test_harness.hpp"

#include "tmag5273.hpp"

using Tmag = espp::TMAG5273;

namespace {

/// A TMAG5273 on a fake bus. Not movable: the callbacks capture `this`.
struct FakeBus {
  struct Write {
    std::uint8_t address;
    std::uint8_t reg;
    std::uint8_t value;
    std::size_t length;
  };

  std::array<std::uint8_t, 256> regs{};
  std::vector<Write> writes;
  std::vector<std::uint8_t> read_addresses;
  bool fail_reads{false};
  bool fail_writes{false};

  FakeBus() { regs[Tmag::DEVICE_ID] = 0x02; } // x2 version
  FakeBus(const FakeBus &) = delete;
  FakeBus &operator=(const FakeBus &) = delete;

  /// Driver config wired to this bus. auto_init is off so each test decides
  /// when initialize() runs.
  Tmag::Config config(std::uint8_t address = Tmag::DEFAULT_ADDRESS) {
    Tmag::Config cfg;
    cfg.device_address = address;
    cfg.verbosity = espp::Logger::Verbosity::NONE;
    cfg.auto_init = false;
    cfg.write = [this](std::uint8_t addr, const std::uint8_t *data, std::size_t len) {
      if (fail_writes || len < 2) {
        return false;
      }
      // [register, value...] with auto-increment, like the real part.
      for (std::size_t i = 1; i < len; ++i) {
        regs[static_cast<std::size_t>(data[0] + i - 1)] = data[i];
      }
      writes.push_back({addr, data[0], data[1], len});
      return true;
    };
    cfg.read_register = [this](std::uint8_t addr, std::uint8_t reg, std::uint8_t *data,
                               std::size_t len) {
      read_addresses.push_back(addr);
      if (fail_reads || static_cast<std::size_t>(reg) + len > regs.size()) {
        return false;
      }
      for (std::size_t i = 0; i < len; ++i) {
        data[i] = regs[static_cast<std::size_t>(reg) + i];
      }
      return true;
    };
    return cfg;
  }

  void set16(std::uint8_t msb_reg, std::uint16_t value) {
    regs[msb_reg] = static_cast<std::uint8_t>(value >> 8);
    regs[static_cast<std::size_t>(msb_reg) + 1] = static_cast<std::uint8_t>(value & 0xFF);
  }
};

int reg(const FakeBus &bus, std::uint8_t r) { return bus.regs[r]; }

} // namespace

// ---- initialize ------------------------------------------------------------

TEST(tmag_initialize_writes_the_configured_registers_in_order) {
  FakeBus bus;
  Tmag::Config cfg = bus.config();
  cfg.channels = Tmag::MagneticChannels::XYZ;              // 0x7 -> SENSOR_CONFIG_1 bits 7:4
  cfg.xy_range = Tmag::MagneticRange::HIGH;                // SENSOR_CONFIG_2 bit 0
  cfg.z_range = Tmag::MagneticRange::HIGH;                 // SENSOR_CONFIG_2 bit 1
  cfg.conversion_average = Tmag::ConversionAverage::AVG_8; // 3 -> DEVICE_CONFIG_1 bits 6:4
  cfg.low_noise = true;                                    // DEVICE_CONFIG_2 bit 4
  cfg.operating_mode = Tmag::OperatingMode::CONTINUOUS;    // 2 -> DEVICE_CONFIG_2 bits 1:0
  Tmag dev(cfg);

  std::error_code ec;
  CHECK(dev.initialize(ec));
  CHECK(!ec);

  struct Expect {
    std::uint8_t reg;
    std::uint8_t value;
  };
  const Expect expected[] = {
      {Tmag::SENSOR_CONFIG_1, 0x70}, {Tmag::SENSOR_CONFIG_2, 0x01}, {Tmag::SENSOR_CONFIG_2, 0x03},
      {Tmag::DEVICE_CONFIG_1, 0x30}, {Tmag::DEVICE_CONFIG_2, 0x10}, {Tmag::DEVICE_CONFIG_2, 0x12},
  };
  CHECK_EQ(bus.writes.size(), std::size_t{6});
  for (std::size_t i = 0; i < bus.writes.size(); ++i) {
    CHECK_EQ(int(bus.writes[i].reg), int(expected[i].reg));
    CHECK_EQ(int(bus.writes[i].value), int(expected[i].value));
    CHECK_EQ(bus.writes[i].length, std::size_t{2});
    CHECK_EQ(int(bus.writes[i].address), int(Tmag::DEFAULT_ADDRESS));
  }
}

TEST(tmag_initialize_preserves_unrelated_bits_in_the_config_registers) {
  FakeBus bus;
  bus.regs[Tmag::SENSOR_CONFIG_1] = 0x0A;
  bus.regs[Tmag::SENSOR_CONFIG_2] = 0xFC;
  bus.regs[Tmag::DEVICE_CONFIG_1] = 0x8F;
  bus.regs[Tmag::DEVICE_CONFIG_2] = 0xE0;
  Tmag::Config cfg = bus.config();
  cfg.channels = Tmag::MagneticChannels::XY; // 3
  cfg.xy_range = Tmag::MagneticRange::LOW;
  cfg.z_range = Tmag::MagneticRange::HIGH;
  cfg.conversion_average = Tmag::ConversionAverage::AVG_2; // 1
  cfg.low_noise = false;
  cfg.operating_mode = Tmag::OperatingMode::WAKE_SLEEP; // 3
  Tmag dev(cfg);
  std::error_code ec;
  CHECK(dev.initialize(ec));

  CHECK_EQ(reg(bus, Tmag::SENSOR_CONFIG_1), 0x3A); // low nibble kept, channels 3 in the high one
  CHECK_EQ(reg(bus, Tmag::SENSOR_CONFIG_2), 0xFE); // bit 0 cleared (LOW), bit 1 set
  CHECK_EQ(reg(bus, Tmag::DEVICE_CONFIG_1), 0x9F); // bits 6:4 = 1, bit 7 and 3:0 kept
  CHECK_EQ(reg(bus, Tmag::DEVICE_CONFIG_2), 0xE3); // bit 4 cleared, mode 3, top bits kept
}

TEST(tmag_initialize_uses_the_configured_device_address) {
  FakeBus bus;
  Tmag dev(bus.config(0x22));
  std::error_code ec;
  CHECK(dev.initialize(ec));
  CHECK(!bus.writes.empty());
  for (const auto &w : bus.writes) {
    CHECK_EQ(int(w.address), 0x22);
  }
  for (std::uint8_t a : bus.read_addresses) {
    CHECK_EQ(int(a), 0x22);
  }
}

TEST(tmag_initialize_fails_without_writing_when_the_device_id_is_missing) {
  for (std::uint8_t id : {std::uint8_t{0x00}, std::uint8_t{0xFF}}) {
    FakeBus bus;
    bus.regs[Tmag::DEVICE_ID] = id;
    Tmag dev(bus.config());
    std::error_code ec;
    CHECK(!dev.initialize(ec));
    CHECK(ec == std::errc::not_connected);
    CHECK(bus.writes.empty());
  }
}

TEST(tmag_initialize_fails_when_the_bus_reads_fail) {
  FakeBus bus;
  bus.fail_reads = true;
  Tmag dev(bus.config());
  std::error_code ec;
  CHECK(!dev.initialize(ec));
  CHECK(bool(ec));
  CHECK(bus.writes.empty());
}

TEST(tmag_initialize_fails_when_a_config_write_fails) {
  FakeBus bus;
  bus.fail_writes = true;
  Tmag dev(bus.config());
  std::error_code ec;
  CHECK(!dev.initialize(ec));
  CHECK(ec == std::errc::io_error);
}

TEST(tmag_auto_init_configures_the_device_in_the_constructor) {
  FakeBus bus;
  Tmag::Config cfg = bus.config();
  cfg.auto_init = true;
  Tmag dev(cfg);
  CHECK_EQ(bus.writes.size(), std::size_t{6});
  std::error_code ec;
  CHECK(dev.operating_mode(ec) == Tmag::OperatingMode::CONTINUOUS);
}

// ---- identification --------------------------------------------------------

TEST(tmag_manufacturer_id_is_assembled_lsb_first) {
  FakeBus bus;
  bus.regs[Tmag::MANUFACTURER_ID_LSB] = 0x49;
  bus.regs[Tmag::MANUFACTURER_ID_MSB] = 0x54;
  Tmag dev(bus.config());
  std::error_code ec;
  CHECK_EQ(int(dev.manufacturer_id(ec)), 0x5449);
  CHECK(!ec);
  CHECK_EQ(int(dev.device_id(ec)), 0x02);
}

TEST(tmag_is_connected_rejects_all_zero_and_all_one_ids) {
  FakeBus bus;
  Tmag dev(bus.config());
  std::error_code ec;
  CHECK(dev.is_connected(ec));
  bus.regs[Tmag::DEVICE_ID] = 0x00;
  CHECK(!dev.is_connected(ec));
  // cppcheck-suppress unreadVariable ; read back through the fake bus callbacks
  bus.regs[Tmag::DEVICE_ID] = 0xFF;
  CHECK(!dev.is_connected(ec));
}

// ---- setters: read-modify-write -------------------------------------------

TEST(tmag_set_operating_mode_replaces_only_bits_1_0) {
  FakeBus bus;
  Tmag dev(bus.config());
  std::error_code ec;
  bus.regs[Tmag::DEVICE_CONFIG_2] = 0xFF;
  CHECK(dev.set_operating_mode(Tmag::OperatingMode::STANDBY, ec));
  CHECK_EQ(reg(bus, Tmag::DEVICE_CONFIG_2), 0xFC);
  bus.regs[Tmag::DEVICE_CONFIG_2] = 0x00;
  CHECK(dev.set_operating_mode(Tmag::OperatingMode::WAKE_SLEEP, ec));
  CHECK_EQ(reg(bus, Tmag::DEVICE_CONFIG_2), 0x03);
  CHECK(dev.operating_mode(ec) == Tmag::OperatingMode::WAKE_SLEEP);
  CHECK(dev.set_operating_mode(Tmag::OperatingMode::CONTINUOUS, ec));
  CHECK(dev.operating_mode(ec) == Tmag::OperatingMode::CONTINUOUS);
}

TEST(tmag_set_magnetic_channels_replaces_only_the_high_nibble) {
  FakeBus bus;
  Tmag dev(bus.config());
  std::error_code ec;
  bus.regs[Tmag::SENSOR_CONFIG_1] = 0xFF;
  CHECK(dev.set_magnetic_channels(Tmag::MagneticChannels::XY, ec));
  CHECK_EQ(reg(bus, Tmag::SENSOR_CONFIG_1), 0x3F);
  CHECK(dev.set_magnetic_channels(Tmag::MagneticChannels::XYX, ec)); // 0x8, the top of the range
  CHECK_EQ(reg(bus, Tmag::SENSOR_CONFIG_1), 0x8F);
  bus.regs[Tmag::SENSOR_CONFIG_1] = 0x0A;
  CHECK(dev.set_magnetic_channels(Tmag::MagneticChannels::Z, ec));
  CHECK_EQ(reg(bus, Tmag::SENSOR_CONFIG_1), 0x4A);
  CHECK(dev.magnetic_channels(ec) == Tmag::MagneticChannels::Z);
}

TEST(tmag_set_xy_and_z_range_each_touch_their_own_bit) {
  FakeBus bus;
  Tmag dev(bus.config());
  std::error_code ec;
  bus.regs[Tmag::SENSOR_CONFIG_2] = 0xFF;
  CHECK(dev.set_xy_range(Tmag::MagneticRange::LOW, ec));
  CHECK_EQ(reg(bus, Tmag::SENSOR_CONFIG_2), 0xFE);
  CHECK(dev.set_z_range(Tmag::MagneticRange::LOW, ec));
  CHECK_EQ(reg(bus, Tmag::SENSOR_CONFIG_2), 0xFC);
  CHECK(dev.xy_range(ec) == Tmag::MagneticRange::LOW);
  CHECK(dev.z_range(ec) == Tmag::MagneticRange::LOW);

  bus.regs[Tmag::SENSOR_CONFIG_2] = 0x00;
  CHECK(dev.set_z_range(Tmag::MagneticRange::HIGH, ec));
  CHECK_EQ(reg(bus, Tmag::SENSOR_CONFIG_2), 0x02);
  CHECK(dev.set_xy_range(Tmag::MagneticRange::HIGH, ec));
  CHECK_EQ(reg(bus, Tmag::SENSOR_CONFIG_2), 0x03);
  CHECK(dev.xy_range(ec) == Tmag::MagneticRange::HIGH);
  CHECK(dev.z_range(ec) == Tmag::MagneticRange::HIGH);
}

TEST(tmag_set_conversion_average_replaces_only_bits_6_4) {
  FakeBus bus;
  Tmag dev(bus.config());
  std::error_code ec;
  bus.regs[Tmag::DEVICE_CONFIG_1] = 0xFF;
  CHECK(dev.set_conversion_average(Tmag::ConversionAverage::AVG_1, ec));
  CHECK_EQ(reg(bus, Tmag::DEVICE_CONFIG_1), 0x8F);
  bus.regs[Tmag::DEVICE_CONFIG_1] = 0x00;
  CHECK(dev.set_conversion_average(Tmag::ConversionAverage::AVG_32, ec)); // 5
  CHECK_EQ(reg(bus, Tmag::DEVICE_CONFIG_1), 0x50);
  CHECK(dev.conversion_average(ec) == Tmag::ConversionAverage::AVG_32);
}

TEST(tmag_set_low_noise_toggles_only_bit_4) {
  FakeBus bus;
  Tmag dev(bus.config());
  std::error_code ec;
  bus.regs[Tmag::DEVICE_CONFIG_2] = 0x00;
  CHECK(dev.set_low_noise(true, ec));
  CHECK_EQ(reg(bus, Tmag::DEVICE_CONFIG_2), 0x10);
  CHECK(dev.low_noise(ec));
  bus.regs[Tmag::DEVICE_CONFIG_2] = 0xFF;
  CHECK(dev.set_low_noise(false, ec));
  CHECK_EQ(reg(bus, Tmag::DEVICE_CONFIG_2), 0xEF);
  CHECK(!dev.low_noise(ec));
}

TEST(tmag_set_angle_conversion_replaces_only_bits_3_2) {
  FakeBus bus;
  Tmag dev(bus.config());
  std::error_code ec;
  bus.regs[Tmag::SENSOR_CONFIG_2] = 0xFF;
  CHECK(dev.set_angle_conversion(Tmag::AngleConversion::ANGLE_XY, ec));
  CHECK_EQ(reg(bus, Tmag::SENSOR_CONFIG_2), 0xF7);
  bus.regs[Tmag::SENSOR_CONFIG_2] = 0x00;
  CHECK(dev.set_angle_conversion(Tmag::AngleConversion::ANGLE_XZ, ec)); // 3
  CHECK_EQ(reg(bus, Tmag::SENSOR_CONFIG_2), 0x0C);
}

XFAIL_TEST(tmag_angle_conversion_getter_reads_back_what_the_setter_wrote,
           "set_angle_conversion writes SENSOR_CONFIG_2 bits 3:2 but angle_conversion() reads "
           "bits 7:6, so the value never round-trips") {
  FakeBus bus;
  Tmag dev(bus.config());
  std::error_code ec;
  CHECK(dev.set_angle_conversion(Tmag::AngleConversion::ANGLE_YZ, ec));
  CHECK(dev.angle_conversion(ec) == Tmag::AngleConversion::ANGLE_YZ);
}

TEST(tmag_setters_do_not_write_when_the_read_fails) {
  FakeBus bus;
  Tmag dev(bus.config());
  // cppcheck-suppress unreadVariable ; read back through the fake bus callbacks
  bus.fail_reads = true;
  std::error_code ec;
  CHECK(!dev.set_operating_mode(Tmag::OperatingMode::SLEEP, ec));
  CHECK(ec == std::errc::io_error);
  CHECK(!dev.set_magnetic_channels(Tmag::MagneticChannels::XY, ec));
  CHECK(!dev.set_xy_range(Tmag::MagneticRange::LOW, ec));
  CHECK(!dev.set_z_range(Tmag::MagneticRange::LOW, ec));
  CHECK(!dev.set_conversion_average(Tmag::ConversionAverage::AVG_4, ec));
  CHECK(!dev.set_low_noise(true, ec));
  CHECK(!dev.set_angle_conversion(Tmag::AngleConversion::ANGLE_XY, ec));
  CHECK(bus.writes.empty());
}

TEST(tmag_failed_write_leaves_the_cached_range_unchanged) {
  FakeBus bus;
  Tmag::Config cfg = bus.config();
  cfg.xy_range = Tmag::MagneticRange::HIGH;
  Tmag dev(cfg);
  // cppcheck-suppress unreadVariable ; read back through the fake bus callbacks
  bus.fail_writes = true;
  std::error_code ec;
  CHECK(!dev.set_xy_range(Tmag::MagneticRange::LOW, ec));
  CHECK(ec == std::errc::io_error);
  CHECK_NEAR(dev.range_xy_mT(), 266.0f, 0.0); // still the scale of the real device state
}

// ---- raw and scaled readings ----------------------------------------------

TEST(tmag_read_raw_is_big_endian_and_signed) {
  FakeBus bus;
  bus.set16(Tmag::X_MSB_RESULT, 0x1234);
  bus.set16(Tmag::Y_MSB_RESULT, 0xFFFF);
  bus.set16(Tmag::Z_MSB_RESULT, 0x8000);
  Tmag dev(bus.config());
  std::error_code ec;
  const auto raw = dev.read_raw(ec);
  CHECK(!ec);
  CHECK_EQ(int(raw.x), 0x1234);
  CHECK_EQ(int(raw.y), -1);
  CHECK_EQ(int(raw.z), -32768);

  bus.set16(Tmag::X_MSB_RESULT, 0x7FFF);
  bus.set16(Tmag::Y_MSB_RESULT, 0x0100);
  bus.set16(Tmag::Z_MSB_RESULT, 0xFF00);
  const auto raw2 = dev.read_raw(ec);
  CHECK_EQ(int(raw2.x), 32767);
  CHECK_EQ(int(raw2.y), 256); // MSB first: 0x01 0x00, not 0x0001
  CHECK_EQ(int(raw2.z), -256);
}

TEST(tmag_convert_raw_to_mt_scales_by_the_range) {
  FakeBus bus;
  // Full scale is +-133 mT (LOW) or +-266 mT (HIGH) over a signed 16-bit count.
  // Convert through the public single-axis readers: X uses the XY range.
  auto convert = [&bus](std::int16_t raw, Tmag::MagneticRange range) {
    bus.set16(Tmag::X_MSB_RESULT, static_cast<std::uint16_t>(raw));
    Tmag::Config cfg = bus.config();
    cfg.xy_range = range;
    Tmag dev(cfg);
    std::error_code ec;
    const float mt = dev.read_x(ec);
    CHECK(!ec);
    return static_cast<double>(mt);
  };
  CHECK_NEAR(convert(0, Tmag::MagneticRange::LOW), 0.0, 1e-6);
  CHECK_NEAR(convert(16384, Tmag::MagneticRange::LOW), 66.5, 1e-4);
  CHECK_NEAR(convert(-16384, Tmag::MagneticRange::LOW), -66.5, 1e-4);
  CHECK_NEAR(convert(-32768, Tmag::MagneticRange::LOW), -133.0, 1e-4);
  CHECK_NEAR(convert(16384, Tmag::MagneticRange::HIGH), 133.0, 1e-4);
  CHECK_NEAR(convert(-32768, Tmag::MagneticRange::HIGH), -266.0, 1e-4);
  CHECK_NEAR(convert(32767, Tmag::MagneticRange::HIGH), 266.0 * 32767.0 / 32768.0, 1e-3);
}

TEST(tmag_read_magnetic_uses_the_xy_range_for_x_y_and_the_z_range_for_z) {
  FakeBus bus;
  bus.set16(Tmag::X_MSB_RESULT, 16384);
  bus.set16(Tmag::Y_MSB_RESULT, static_cast<std::uint16_t>(-16384));
  bus.set16(Tmag::Z_MSB_RESULT, 16384);
  Tmag::Config cfg = bus.config();
  cfg.xy_range = Tmag::MagneticRange::LOW;
  cfg.z_range = Tmag::MagneticRange::HIGH;
  Tmag dev(cfg);
  std::error_code ec;
  const auto m = dev.read_magnetic(ec);
  CHECK(!ec);
  CHECK_NEAR(m.x, 66.5, 1e-4);
  CHECK_NEAR(m.y, -66.5, 1e-4);
  CHECK_NEAR(m.z, 133.0, 1e-4);

  // Changing the range through the setter changes the scale of later reads.
  CHECK(dev.set_xy_range(Tmag::MagneticRange::HIGH, ec));
  const auto m2 = dev.read_magnetic(ec);
  CHECK_NEAR(m2.x, 133.0, 1e-4);
  CHECK_NEAR(m2.y, -133.0, 1e-4);
  CHECK_NEAR(m2.z, 133.0, 1e-4);
}

TEST(tmag_single_axis_reads_match_the_combined_read) {
  FakeBus bus;
  bus.set16(Tmag::X_MSB_RESULT, 1000);
  bus.set16(Tmag::Y_MSB_RESULT, static_cast<std::uint16_t>(-2000));
  bus.set16(Tmag::Z_MSB_RESULT, 3000);
  Tmag::Config cfg = bus.config();
  cfg.xy_range = Tmag::MagneticRange::HIGH;
  cfg.z_range = Tmag::MagneticRange::LOW;
  Tmag dev(cfg);
  std::error_code ec;
  const auto m = dev.read_magnetic(ec);
  CHECK_NEAR(dev.read_x(ec), m.x, 1e-6);
  CHECK_NEAR(dev.read_y(ec), m.y, 1e-6);
  CHECK_NEAR(dev.read_z(ec), m.z, 1e-6);
  CHECK_NEAR(m.z, 3000.0 / 32768.0 * 133.0, 1e-4);
}

TEST(tmag_range_helpers_follow_the_configured_range) {
  FakeBus bus;
  Tmag::Config cfg = bus.config();
  cfg.xy_range = Tmag::MagneticRange::LOW;
  cfg.z_range = Tmag::MagneticRange::HIGH;
  Tmag dev(cfg);
  CHECK_NEAR(dev.range_xy_mT(), 133.0f, 0.0);
  CHECK_NEAR(dev.range_z_mT(), 266.0f, 0.0);

  std::error_code ec;
  CHECK(dev.set_xy_range(Tmag::MagneticRange::HIGH, ec));
  CHECK(dev.set_z_range(Tmag::MagneticRange::LOW, ec));
  CHECK_NEAR(dev.range_xy_mT(), 266.0f, 0.0);
  CHECK_NEAR(dev.range_z_mT(), 133.0f, 0.0);
}

TEST(tmag_range_helpers_agree_with_the_raw_conversion_full_scale) {
  FakeBus bus;
  for (auto range : {Tmag::MagneticRange::LOW, Tmag::MagneticRange::HIGH}) {
    Tmag::Config cfg = bus.config();
    cfg.xy_range = range;
    cfg.z_range = range;
    Tmag dev(cfg);
    std::error_code ec;
    // One count below +full scale converts to just under the advertised range.
    bus.set16(Tmag::X_MSB_RESULT, 32767);
    bus.set16(Tmag::Z_MSB_RESULT, 0x8000);
    CHECK_NEAR(dev.read_x(ec), dev.range_xy_mT(), 0.01);
    CHECK_NEAR(dev.read_z(ec), -dev.range_z_mT(), 1e-3);
  }
}

TEST(tmag_a1_variant_uses_the_a1_full_scale_values) {
  FakeBus bus;
  struct Case {
    Tmag::MagneticRange range;
    double full_scale;
  };
  for (const Case c :
       {Case{Tmag::MagneticRange::LOW, 40.0}, Case{Tmag::MagneticRange::HIGH, 80.0}}) {
    Tmag::Config cfg = bus.config();
    cfg.variant = Tmag::Variant::A1;
    cfg.xy_range = c.range;
    cfg.z_range = c.range;
    Tmag dev(cfg);
    std::error_code ec;
    CHECK_NEAR(dev.range_xy_mT(), c.full_scale, 0.0);
    CHECK_NEAR(dev.range_z_mT(), c.full_scale, 0.0);
    bus.set16(Tmag::X_MSB_RESULT, 16384);
    bus.set16(Tmag::Z_MSB_RESULT, 0x8000);
    CHECK_NEAR(dev.read_x(ec), c.full_scale / 2.0, 1e-4);
    CHECK_NEAR(dev.read_z(ec), -c.full_scale, 1e-4);
    const auto m = dev.read_magnetic(ec);
    CHECK_NEAR(m.x, c.full_scale / 2.0, 1e-4);
    CHECK_NEAR(m.z, -c.full_scale, 1e-4);
  }
}

TEST(tmag_variant_defaults_to_auto_and_does_not_change_raw_counts) {
  FakeBus bus;
  Tmag::Config cfg = bus.config();
  CHECK(cfg.variant == Tmag::Variant::AUTO);
  bus.set16(Tmag::X_MSB_RESULT, 1234);
  bus.set16(Tmag::Y_MSB_RESULT, 0xFB2E);
  bus.set16(Tmag::Z_MSB_RESULT, 777);
  std::error_code ec;
  Tmag a2(cfg);
  cfg.variant = Tmag::Variant::A1;
  Tmag a1(cfg);
  const auto r2 = a2.read_raw(ec);
  const auto r1 = a1.read_raw(ec);
  CHECK_EQ(int(r1.x), int(r2.x));
  CHECK_EQ(int(r1.y), int(r2.y));
  CHECK_EQ(int(r1.z), int(r2.z));
  CHECK_NEAR(a2.range_xy_mT(), 266.0f, 0.0);
}

TEST(tmag_auto_variant_is_detected_from_the_device_id_version_field) {
  struct Case {
    std::uint8_t id;
    Tmag::Variant variant;
    double low;
    double high;
  };
  // Bits 1:0 are the version; the upper bits do not matter.
  for (const Case c :
       {Case{0x01, Tmag::Variant::A1, 40.0, 80.0}, Case{0x02, Tmag::Variant::A2, 133.0, 266.0},
        Case{0x81, Tmag::Variant::A1, 40.0, 80.0}, Case{0x82, Tmag::Variant::A2, 133.0, 266.0}}) {
    FakeBus bus;
    bus.regs[Tmag::DEVICE_ID] = c.id;
    Tmag::Config cfg = bus.config();
    cfg.xy_range = Tmag::MagneticRange::LOW;
    cfg.z_range = Tmag::MagneticRange::HIGH;
    Tmag dev(cfg);
    std::error_code ec;
    CHECK(dev.initialize(ec));
    CHECK(dev.variant() == c.variant);
    CHECK_NEAR(dev.range_xy_mT(), c.low, 0.0);
    CHECK_NEAR(dev.range_z_mT(), c.high, 0.0);
  }
}

TEST(tmag_auto_variant_falls_back_to_a2_for_an_unknown_version) {
  for (std::uint8_t id : {std::uint8_t{0x04}, std::uint8_t{0x03}}) {
    FakeBus bus;
    bus.regs[Tmag::DEVICE_ID] = id;
    Tmag dev(bus.config());
    std::error_code ec;
    CHECK(dev.initialize(ec));
    CHECK(dev.variant() == Tmag::Variant::A2);
  }
  FakeBus bus;
  bus.regs[Tmag::DEVICE_ID] = 0x00; // version 0 (reserved)
  Tmag before_init(bus.config());
  CHECK(before_init.variant() == Tmag::Variant::A2); // before any detection
}

TEST(tmag_explicit_variant_overrides_the_device_id) {
  FakeBus bus;
  bus.regs[Tmag::DEVICE_ID] = 0x02; // an x2 part
  Tmag::Config cfg = bus.config();
  cfg.variant = Tmag::Variant::A1;
  Tmag dev(cfg);
  std::error_code ec;
  CHECK(dev.initialize(ec));
  CHECK(dev.variant() == Tmag::Variant::A1);
  CHECK_NEAR(dev.range_xy_mT(), 80.0f, 0.0);

  // cppcheck-suppress unreadVariable ; read back through the fake bus callbacks
  bus.regs[Tmag::DEVICE_ID] = 0x01; // an x1 part, forced to A2
  cfg.variant = Tmag::Variant::A2;
  Tmag forced(cfg);
  CHECK(forced.initialize(ec));
  CHECK(forced.variant() == Tmag::Variant::A2);
  CHECK_NEAR(forced.range_xy_mT(), 266.0f, 0.0);
}

// ---- temperature, angle, status -------------------------------------------

TEST(tmag_read_temperature_follows_the_header_formula) {
  // Driver formula: raw / 128 + 25 degrees C, raw a signed big-endian count.
  FakeBus bus;
  Tmag dev(bus.config());
  std::error_code ec;
  bus.set16(Tmag::T_MSB_RESULT, 0x0000);
  CHECK_NEAR(dev.read_temperature(ec), 25.0, 1e-6);
  bus.set16(Tmag::T_MSB_RESULT, 0x0A00); // 2560
  CHECK_NEAR(dev.read_temperature(ec), 45.0, 1e-6);
  bus.set16(Tmag::T_MSB_RESULT, 0xFB00); // -1280
  CHECK_NEAR(dev.read_temperature(ec), 15.0, 1e-6);
  CHECK_EQ(int(dev.read_raw_temperature(ec)), -1280);
}

TEST(tmag_read_angle_is_a_sixteenth_of_a_degree_per_count) {
  FakeBus bus;
  Tmag dev(bus.config());
  std::error_code ec;
  bus.set16(Tmag::ANGLE_RESULT_MSB, 0x0B40); // 2880
  CHECK_NEAR(dev.read_angle(ec), 180.0, 1e-6);
  bus.set16(Tmag::ANGLE_RESULT_MSB, 0x0001);
  CHECK_NEAR(dev.read_angle(ec), 0.0625, 1e-6);
  bus.set16(Tmag::ANGLE_RESULT_MSB, 0x167F); // 5759
  CHECK_NEAR(dev.read_angle(ec), 359.9375, 1e-4);
}

TEST(tmag_conversion_ready_tests_only_the_result_ready_bit) {
  FakeBus bus;
  Tmag dev(bus.config());
  std::error_code ec;
  bus.regs[Tmag::CONV_STATUS] = Tmag::STATUS_RESULT_READY;
  CHECK(dev.conversion_ready(ec));
  bus.regs[Tmag::CONV_STATUS] = Tmag::STATUS_DIAG_FAIL | Tmag::STATUS_POR;
  CHECK(!dev.conversion_ready(ec));
  // cppcheck-suppress unreadVariable ; read back through the fake bus callbacks
  bus.regs[Tmag::CONV_STATUS] = 0xFF;
  CHECK(dev.conversion_ready(ec));
}

TEST(tmag_direct_register_access_round_trips) {
  FakeBus bus;
  Tmag dev(bus.config());
  std::error_code ec;
  CHECK(dev.write_register(Tmag::X_THR_CONFIG, 0xA5, ec));
  CHECK_EQ(int(dev.read_register(Tmag::X_THR_CONFIG, ec)), 0xA5);
  bus.regs[Tmag::MAGNITUDE_RESULT] = 0x42;
  CHECK_EQ(int(dev.read_magnitude(ec)), 0x42);
  std::uint8_t buf[3]{};
  bus.regs[Tmag::Y_THR_CONFIG] = 1;
  bus.regs[Tmag::Z_THR_CONFIG] = 2;
  // cppcheck-suppress unreadVariable ; read back through the fake bus callbacks
  bus.regs[Tmag::T_CONFIG] = 3;
  CHECK(dev.read_registers(Tmag::Y_THR_CONFIG, buf, sizeof(buf), ec));
  CHECK_EQ(int(buf[0]), 1);
  CHECK_EQ(int(buf[2]), 3);
}

// ---- bus failures ----------------------------------------------------------

TEST(tmag_reads_report_io_error_and_return_zero_when_the_bus_fails) {
  FakeBus bus;
  bus.set16(Tmag::X_MSB_RESULT, 1234);
  Tmag dev(bus.config());
  // cppcheck-suppress unreadVariable ; read back through the fake bus callbacks
  bus.fail_reads = true;

  std::error_code ec;
  const auto raw = dev.read_raw(ec);
  CHECK(ec == std::errc::io_error);
  CHECK_EQ(int(raw.x), 0);

  ec.clear();
  const auto m = dev.read_magnetic(ec);
  CHECK(ec == std::errc::io_error);
  CHECK_NEAR(m.x, 0.0, 0.0);

  ec.clear();
  CHECK_NEAR(dev.read_temperature(ec), 0.0, 0.0);
  CHECK(bool(ec));
  ec.clear();
  CHECK_NEAR(dev.read_angle(ec), 0.0, 0.0);
  CHECK(bool(ec));
  ec.clear();
  CHECK_NEAR(dev.read_x(ec), 0.0, 0.0);
  CHECK(bool(ec));
  ec.clear();
  CHECK(!dev.conversion_ready(ec));
  CHECK(bool(ec));
  ec.clear();
  CHECK_EQ(int(dev.manufacturer_id(ec)), 0);
  CHECK(bool(ec));
}

TEST(tmag_recovers_once_the_bus_works_again) {
  FakeBus bus;
  bus.set16(Tmag::X_MSB_RESULT, 16384);
  Tmag::Config cfg = bus.config();
  cfg.xy_range = Tmag::MagneticRange::HIGH;
  Tmag dev(cfg);
  std::error_code ec;
  bus.fail_reads = true;
  dev.read_magnetic(ec);
  CHECK(bool(ec));
  // cppcheck-suppress [unreadVariable, redundantAssignment] ; read back through the fake bus
  // callbacks
  bus.fail_reads = false;
  const auto m = dev.read_magnetic(ec);
  CHECK(!ec);
  CHECK_NEAR(m.x, 133.0, 1e-4);
}

TEST(tmag_reports_not_supported_without_a_read_register_function) {
  FakeBus bus;
  Tmag::Config cfg = bus.config();
  cfg.read_register = nullptr;
  Tmag dev(cfg);
  std::error_code ec;
  dev.read_raw(ec);
  CHECK(ec == std::errc::operation_not_supported);
}
