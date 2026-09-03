#pragma once

#include <cstdint>
#include <system_error>

#include "base_component.hpp"
#include "base_peripheral.hpp"

namespace espp {

/**
 * @brief Driver for the Texas Instruments TMAG5273 3-axis Hall-effect sensor.
 *
 * @details
 * Header-only espp driver for the TMAG5273 using the ESP-IDF I2C master API.
 *
 * The API is intentionally modeled after the Adafruit TMAG5273 library while
 * using espp's BasePeripheral abstraction for register access and I2C locking.
 */
class TMAG5273 : public BasePeripheral<> {
public:
  // ==========================================================================
  // Device constants
  // ==========================================================================

  static constexpr uint8_t DEFAULT_ADDRESS = 0x35;

  // ==========================================================================
  // Register addresses
  // ==========================================================================

  static constexpr uint8_t DEVICE_CONFIG_1 = 0x00;
  static constexpr uint8_t DEVICE_CONFIG_2 = 0x01;
  static constexpr uint8_t SENSOR_CONFIG_1 = 0x02;
  static constexpr uint8_t SENSOR_CONFIG_2 = 0x03;

  static constexpr uint8_t X_THR_CONFIG = 0x04;
  static constexpr uint8_t Y_THR_CONFIG = 0x05;
  static constexpr uint8_t Z_THR_CONFIG = 0x06;
  static constexpr uint8_t T_CONFIG = 0x07;
  static constexpr uint8_t INT_CONFIG_1 = 0x08;

  static constexpr uint8_t MAG_GAIN_CONFIG = 0x09;
  static constexpr uint8_t MAG_OFFSET_CONFIG_1 = 0x0A;
  static constexpr uint8_t MAG_OFFSET_CONFIG_2 = 0x0B;

  static constexpr uint8_t I2C_ADDRESS = 0x0C;
  static constexpr uint8_t DEVICE_ID = 0x0D;
  static constexpr uint8_t MANUFACTURER_ID_LSB = 0x0E;
  static constexpr uint8_t MANUFACTURER_ID_MSB = 0x0F;

  static constexpr uint8_t T_MSB_RESULT = 0x10;
  static constexpr uint8_t T_LSB_RESULT = 0x11;

  static constexpr uint8_t X_MSB_RESULT = 0x12;
  static constexpr uint8_t X_LSB_RESULT = 0x13;

  static constexpr uint8_t Y_MSB_RESULT = 0x14;
  static constexpr uint8_t Y_LSB_RESULT = 0x15;

  static constexpr uint8_t Z_MSB_RESULT = 0x16;
  static constexpr uint8_t Z_LSB_RESULT = 0x17;

  static constexpr uint8_t CONV_STATUS = 0x18;

  static constexpr uint8_t ANGLE_RESULT_MSB = 0x19;
  static constexpr uint8_t ANGLE_RESULT_LSB = 0x1A;

  static constexpr uint8_t MAGNITUDE_RESULT = 0x1B;
  static constexpr uint8_t DEVICE_STATUS = 0x1C;

  // ==========================================================================
  // Conversion status bits
  // ==========================================================================

  static constexpr uint8_t STATUS_RESULT_READY = 0x01;
  static constexpr uint8_t STATUS_DIAG_FAIL = 0x02;
  static constexpr uint8_t STATUS_POR = 0x10;

  // ==========================================================================
  // Operating modes
  // ==========================================================================

  enum class OperatingMode : uint8_t {
    STANDBY = 0x00,
    SLEEP = 0x01,
    CONTINUOUS = 0x02,
    WAKE_SLEEP = 0x03
  };

  // ==========================================================================
  // Magnetic channels
  // ==========================================================================

  enum class MagneticChannels : uint8_t {
    OFF = 0x00,
    X = 0x01,
    Y = 0x02,
    XY = 0x03,
    Z = 0x04,
    ZX = 0x05,
    YZ = 0x06,
    XYZ = 0x07,

    // Angle measurement combinations
    XYX = 0x08,
    YXY = 0x09,
    YZY = 0x0A,
    XZX = 0x0B
  };

  // ==========================================================================
  // Magnetic range
  //
  // TMAG5273A1:
  //   LOW  = ±40 mT
  //   HIGH = ±80 mT
  //
  // TMAG5273A2:
  //   LOW  = ±133 mT
  //   HIGH = ±266 mT
  // ==========================================================================

  enum class MagneticRange : uint8_t { LOW = 0, HIGH = 1 };

  // ==========================================================================
  // Angle conversion
  // ==========================================================================

  enum class AngleConversion : uint8_t {
    OFF = 0x00,
    ANGLE_XY = 0x01,
    ANGLE_YZ = 0x02,
    ANGLE_XZ = 0x03
  };

  // ==========================================================================
  // Conversion averaging
  // ==========================================================================

  enum class ConversionAverage : uint8_t {
    AVG_1 = 0x00,
    AVG_2 = 0x01,
    AVG_4 = 0x02,
    AVG_8 = 0x03,
    AVG_16 = 0x04,
    AVG_32 = 0x05
  };

  // ==========================================================================
  // Data structures
  // ==========================================================================

  struct MagneticData {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
  };

  struct RawMagneticData {
    int16_t x{0};
    int16_t y{0};
    int16_t z{0};
  };

  // ==========================================================================
  // Configuration
  // ==========================================================================

  struct Config {

    uint8_t device_address = DEFAULT_ADDRESS;
    BasePeripheral::write_fn write;
    // BasePeripheral::read_fn read;
    BasePeripheral::read_register_fn read_register;
    espp::Logger::Verbosity verbosity = espp::Logger::Verbosity::INFO;

    MagneticChannels channels{MagneticChannels::XYZ};

    MagneticRange xy_range{MagneticRange::LOW};

    MagneticRange z_range{MagneticRange::LOW};

    OperatingMode operating_mode{OperatingMode::CONTINUOUS};

    ConversionAverage conversion_average{ConversionAverage::AVG_1};

    bool low_noise{false};

    bool auto_init{true};
  };

  // ==========================================================================
  // Constructor
  // ==========================================================================

  explicit TMAG5273(const Config &config)
      : BasePeripheral({.address = config.device_address,
                        .write = config.write,
                        //.read = config.read,
                        .read_register = config.read_register},
                       "TMAG5273", config.verbosity)
      , channels_(config.channels)
      , xy_range_(config.xy_range)
      , z_range_(config.z_range)
      , operating_mode_(config.operating_mode)
      , conversion_average_(config.conversion_average)
      , low_noise_(config.low_noise) {

    if (config.auto_init) {
      std::error_code ec;
      initialize(ec);
    }
  }

  ~TMAG5273() = default;

  // ==========================================================================
  // Initialization
  // ==========================================================================

  bool initialize(std::error_code &ec) {
    if (!is_connected(ec)) {
      ec = std::make_error_code(std::errc::not_connected);
      logger_.error("TMAG5273 not connected or no response from device.");
      return false;
    }

    if (!set_magnetic_channels(channels_, ec)) {
      return false;
    }

    if (!set_xy_range(xy_range_, ec)) {
      return false;
    }

    if (!set_z_range(z_range_, ec)) {
      return false;
    }

    if (!set_conversion_average(conversion_average_, ec)) {
      return false;
    }

    if (!set_low_noise(low_noise_, ec)) {
      return false;
    }

    if (!set_operating_mode(operating_mode_, ec)) {
      return false;
    }

    ec.clear();
    return true;
  }

  // ==========================================================================
  // Device identification
  // ==========================================================================

  uint8_t device_id(std::error_code &ec) const {
    uint8_t device_id = read_u8_from_register(DEVICE_ID, ec);
    return device_id;
  }

  uint16_t manufacturer_id(std::error_code &ec) const {

    uint8_t data[2]{};

    read_many_from_register(MANUFACTURER_ID_LSB, data, sizeof(data), ec);

    if (ec) {
      return 0;
    }

    return static_cast<uint16_t>((static_cast<uint16_t>(data[1]) << 8) | data[0]);
  }

  bool is_connected(std::error_code &ec) const {

    const uint8_t id = device_id(ec);
    printf("TMAG5273 device ID: 0x%02X\n", id);
    if (ec) {
      return false;
    }

    return id != 0x00 && id != 0xFF;
  }

  // ==========================================================================
  // Operating mode
  // ==========================================================================

  bool set_operating_mode(OperatingMode mode, std::error_code &ec) {

    uint8_t reg = read_u8_from_register(DEVICE_CONFIG_1, ec);

    if (ec) {
      return false;
    }

    reg &= 0xFC;
    reg |= static_cast<uint8_t>(mode) & 0x03;

    write_u8_to_register(DEVICE_CONFIG_1, reg, ec);

    if (!ec) {
      operating_mode_ = mode;
    }

    return !ec;
  }

  OperatingMode operating_mode(std::error_code &ec) const {

    const uint8_t reg = read_u8_from_register(DEVICE_CONFIG_1, ec);

    if (ec) {
      return OperatingMode::STANDBY;
    }

    return static_cast<OperatingMode>(reg & 0x03);
  }

  // ==========================================================================
  // Magnetic channel configuration
  // ==========================================================================

  bool set_magnetic_channels(MagneticChannels channels, std::error_code &ec) {

    uint8_t reg = read_u8_from_register(SENSOR_CONFIG_1, ec);

    if (ec) {
      return false;
    }

    // MAG_CH bits.
    reg &= 0xF0;
    reg |= static_cast<uint8_t>(channels) & 0x0F;

    write_u8_to_register(SENSOR_CONFIG_1, reg, ec);

    if (!ec) {
      channels_ = channels;
    }

    return !ec;
  }

  MagneticChannels magnetic_channels(std::error_code &ec) const {

    const uint8_t reg = read_u8_from_register(SENSOR_CONFIG_1, ec);

    if (ec) {
      return MagneticChannels::OFF;
    }

    return static_cast<MagneticChannels>(reg & 0x0F);
  }

  // ==========================================================================
  // Magnetic range
  // ==========================================================================

  bool set_xy_range(MagneticRange range, std::error_code &ec) {

    uint8_t reg = read_u8_from_register(SENSOR_CONFIG_2, ec);

    if (ec) {
      return false;
    }

    /*
     * XY_RANGE occupies the appropriate range-selection bits.
     * Preserve all unrelated bits.
     */
    reg &= ~(0x01 << 3);
    reg |= (static_cast<uint8_t>(range) & 0x01) << 3;

    write_u8_to_register(SENSOR_CONFIG_2, reg, ec);

    if (!ec) {
      xy_range_ = range;
    }

    return !ec;
  }

  MagneticRange xy_range(std::error_code &ec) const {

    const uint8_t reg = read_u8_from_register(SENSOR_CONFIG_2, ec);

    if (ec) {
      return MagneticRange::LOW;
    }

    return static_cast<MagneticRange>((reg >> 3) & 0x01);
  }

  bool set_z_range(MagneticRange range, std::error_code &ec) {

    uint8_t reg = read_u8_from_register(SENSOR_CONFIG_2, ec);

    if (ec) {
      return false;
    }

    reg &= ~(0x01 << 4);
    reg |= (static_cast<uint8_t>(range) & 0x01) << 4;

    write_u8_to_register(SENSOR_CONFIG_2, reg, ec);

    if (!ec) {
      z_range_ = range;
    }

    return !ec;
  }

  MagneticRange z_range(std::error_code &ec) const {

    const uint8_t reg = read_u8_from_register(SENSOR_CONFIG_2, ec);

    if (ec) {
      return MagneticRange::LOW;
    }

    return static_cast<MagneticRange>((reg >> 4) & 0x01);
  }

  // ==========================================================================
  // Conversion averaging
  // ==========================================================================

  bool set_conversion_average(ConversionAverage average, std::error_code &ec) {

    uint8_t reg = read_u8_from_register(DEVICE_CONFIG_1, ec);

    if (ec) {
      return false;
    }

    /*
     * CONV_AVG is located in DEVICE_CONFIG_1.
     */
    reg &= ~(0x07 << 4);
    reg |= (static_cast<uint8_t>(average) & 0x07) << 4;

    write_u8_to_register(DEVICE_CONFIG_1, reg, ec);

    if (!ec) {
      conversion_average_ = average;
    }

    return !ec;
  }

  ConversionAverage conversion_average(std::error_code &ec) const {

    const uint8_t reg = read_u8_from_register(DEVICE_CONFIG_1, ec);

    if (ec) {
      return ConversionAverage::AVG_1;
    }

    return static_cast<ConversionAverage>((reg >> 4) & 0x07);
  }

  // ==========================================================================
  // Low-noise mode
  // ==========================================================================

  bool set_low_noise(bool enabled, std::error_code &ec) {

    uint8_t reg = read_u8_from_register(DEVICE_CONFIG_2, ec);

    if (ec) {
      return false;
    }

    if (enabled) {
      reg |= 0x01;
    } else {
      reg &= ~0x01;
    }

    write_u8_to_register(DEVICE_CONFIG_2, reg, ec);

    if (!ec) {
      low_noise_ = enabled;
    }

    return !ec;
  }

  bool low_noise(std::error_code &ec) const {

    const uint8_t reg = read_u8_from_register(DEVICE_CONFIG_2, ec);

    if (ec) {
      return false;
    }

    return (reg & 0x01) != 0;
  }

  // ==========================================================================
  // Conversion
  // ==========================================================================

  bool trigger_conversion(std::error_code &ec) {

    uint8_t reg = read_u8_from_register(DEVICE_CONFIG_1, ec);

    if (ec) {
      return false;
    }

    /*
     * Trigger a conversion by entering standby from a
     * single-shot configuration.
     *
     * The exact trigger behavior depends on the selected
     * TMAG5273 operating mode.
     */
    reg &= 0xFC;
    reg |= static_cast<uint8_t>(OperatingMode::STANDBY);

    write_u8_to_register(DEVICE_CONFIG_1, reg, ec);

    return !ec;
  }

  bool conversion_ready(std::error_code &ec) const {

    const uint8_t status = read_u8_from_register(CONV_STATUS, ec);

    if (ec) {
      return false;
    }

    return (status & STATUS_RESULT_READY) != 0;
  }

  // ==========================================================================
  // Raw magnetic measurements
  // ==========================================================================

  RawMagneticData read_raw(std::error_code &ec) const {

    uint8_t data[6]{};

    read_many_from_register(X_MSB_RESULT, data, sizeof(data), ec);

    if (ec) {
      return {};
    }

    RawMagneticData result{};

    result.x = static_cast<int16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);

    result.y = static_cast<int16_t>((static_cast<uint16_t>(data[2]) << 8) | data[3]);

    result.z = static_cast<int16_t>((static_cast<uint16_t>(data[4]) << 8) | data[5]);

    return result;
  }

  // ==========================================================================
  // Magnetic measurements
  // ==========================================================================

  MagneticData read_magnetic(std::error_code &ec) const {

    const RawMagneticData raw = read_raw(ec);

    if (ec) {
      return {};
    }

    return {.x = convert_raw_to_mT(raw.x, xy_range_),
            .y = convert_raw_to_mT(raw.y, xy_range_),
            .z = convert_raw_to_mT(raw.z, z_range_)};
  }

  float read_x(std::error_code &ec) const {

    uint8_t data[2]{};

    read_many_from_register(X_MSB_RESULT, data, sizeof(data), ec);

    if (ec) {
      return 0.0f;
    }

    const int16_t raw = static_cast<int16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);

    return convert_raw_to_mT(raw, xy_range_);
  }

  float read_y(std::error_code &ec) const {

    uint8_t data[2]{};

    read_many_from_register(Y_MSB_RESULT, data, sizeof(data), ec);

    if (ec) {
      return 0.0f;
    }

    const int16_t raw = static_cast<int16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);

    return convert_raw_to_mT(raw, xy_range_);
  }

  float read_z(std::error_code &ec) const {

    uint8_t data[2]{};

    read_many_from_register(Z_MSB_RESULT, data, sizeof(data), ec);

    if (ec) {
      return 0.0f;
    }

    const int16_t raw = static_cast<int16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);

    return convert_raw_to_mT(raw, z_range_);
  }

  // ==========================================================================
  // Temperature
  // ==========================================================================

  int16_t read_raw_temperature(std::error_code &ec) const {

    uint8_t data[2]{};

    read_many_from_register(T_MSB_RESULT, data, sizeof(data), ec);

    if (ec) {
      return 0;
    }

    return static_cast<int16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
  }

  float read_temperature(std::error_code &ec) const {

    const int16_t raw = read_raw_temperature(ec);

    if (ec) {
      return 0.0f;
    }

    /*
     * TMAG5273 temperature conversion.
     */
    return (static_cast<float>(raw) / 128.0f) + 25.0f;
  }

  // ==========================================================================
  // Angle
  // ==========================================================================

  bool set_angle_conversion(AngleConversion conversion, std::error_code &ec) {

    uint8_t reg = read_u8_from_register(SENSOR_CONFIG_2, ec);

    if (ec) {
      return false;
    }

    reg &= ~(0x03 << 6);
    reg |= (static_cast<uint8_t>(conversion) & 0x03) << 6;

    write_u8_to_register(SENSOR_CONFIG_2, reg, ec);

    if (!ec) {
      angle_conversion_ = conversion;
    }

    return !ec;
  }

  AngleConversion angle_conversion(std::error_code &ec) const {

    const uint8_t reg = read_u8_from_register(SENSOR_CONFIG_2, ec);

    if (ec) {
      return AngleConversion::OFF;
    }

    return static_cast<AngleConversion>((reg >> 6) & 0x03);
  }

  float read_angle(std::error_code &ec) const {

    uint8_t data[2]{};

    read_many_from_register(ANGLE_RESULT_MSB, data, sizeof(data), ec);

    if (ec) {
      return 0.0f;
    }

    const int16_t raw = static_cast<int16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);

    return static_cast<float>(raw) / 16.0f;
  }

  uint8_t read_magnitude(std::error_code &ec) const {

    return read_u8_from_register(MAGNITUDE_RESULT, ec);
  }

  // ==========================================================================
  // Direct register access
  // ==========================================================================

  uint8_t read_register(uint8_t reg, std::error_code &ec) const {

    return read_u8_from_register(reg, ec);
  }

  bool read_registers(uint8_t reg, uint8_t *data, size_t length, std::error_code &ec) const {

    read_many_from_register(reg, data, length, ec);

    return !ec;
  }

  bool write_register(uint8_t reg, uint8_t value, std::error_code &ec) {

    write_u8_to_register(reg, value, ec);

    return !ec;
  }

private:
  // ==========================================================================
  // Conversion helpers
  // ==========================================================================

  float convert_raw_to_mT(int16_t raw, MagneticRange range) const {

    /*
     * TMAG5273 provides a signed 16-bit magnetic result.
     *
     * The full-scale range corresponds to approximately
     * 32768 counts in the positive direction.
     */
    const float full_scale = range == MagneticRange::LOW ? range_xy_mT() : range_xy_mT() * 2.0f;

    return static_cast<float>(raw) * full_scale / 32768.0f;
  }

  float range_xy_mT() const {

    /*
     * A1 device:
     *   LOW  = ±40 mT
     *   HIGH = ±80 mT
     *
     * If using A2, these values need to be changed to:
     *   LOW  = ±133 mT
     *   HIGH = ±266 mT
     */
    return 266.0f;
  }

  float range_z_mT() const { return 266.0f; }

  // ==========================================================================
  // State
  // ==========================================================================

  MagneticChannels channels_{MagneticChannels::XYZ};

  MagneticRange xy_range_{MagneticRange::HIGH};

  MagneticRange z_range_{MagneticRange::HIGH};

  OperatingMode operating_mode_{OperatingMode::CONTINUOUS};

  ConversionAverage conversion_average_{ConversionAverage::AVG_1};

  bool low_noise_{false};

  AngleConversion angle_conversion_{AngleConversion::OFF};
};

} // namespace espp
