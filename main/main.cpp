#include <chrono>
#include <cstdint>
#include <cstdio>
#include <sdkconfig.h>
#include <system_error>
#include <thread>
#include <vector>

#include "i2c.hpp"
#include "i2c_master.hpp"
#include "i2c_master_device_menu.hpp"
#include "i2c_master_menu.hpp"
#include "i2c_menu.hpp"
#include "i2c_slave_menu.hpp"
#include "logger.hpp"
#include "qtpy.hpp"
#include "tmag5273.hpp"

using namespace std::chrono_literals;

static constexpr uint8_t TMAG_ADDRESS = 0x35;

extern "C" void app_main(void) {

  // ==========================================================================
  // Create I2C bus
  // ==========================================================================
  std::error_code ec;
  auto &qtpy = espp::QtPy::get();
  qtpy.initialize_qwiic_i2c();
  auto i2c = qtpy.qwiic_i2c();
  auto device = i2c->add_device<uint8_t>(
      espp::I2c::DeviceConfig<uint8_t>{.device_address = TMAG_ADDRESS,
                                       .timeout_ms = 1000,
                                       .scl_speed_hz = 400000,
                                       .log_level = espp::Logger::Verbosity::INFO},
      ec);
  printf("\n");
  printf("========================================\n");
  printf("       TMAG5273 Driver Test\n");
  printf("========================================\n");
  printf("I2C Address: 0x%02X\n", TMAG_ADDRESS);
  printf("========================================\n\n");

  // ==========================================================================
  // Probe device
  // ==========================================================================

  printf("Probing for TMAG5273...\n");

  if (!i2c->probe_device(TMAG_ADDRESS)) {
    printf("ERROR: TMAG5273 not found at address 0x%02X\n", TMAG_ADDRESS);

    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  printf("TMAG5273 found!\n\n");

  // ==========================================================================
  // Create callbacks for TMAG5273
  // ==========================================================================
  espp::TMAG5273 tmag(espp::TMAG5273::Config{

      .device_address = TMAG_ADDRESS,
      .write = espp::make_i2c_addressed_write(device),
      //.read = espp::make_i2c_addressed_read(device),
      .read_register = espp::make_i2c_addressed_read_register(device),
      .verbosity = espp::Logger::Verbosity::INFO,
      .channels = espp::TMAG5273::MagneticChannels::XYZ,

      .xy_range = espp::TMAG5273::MagneticRange::LOW,

      .z_range = espp::TMAG5273::MagneticRange::LOW,

      .operating_mode = espp::TMAG5273::OperatingMode::CONTINUOUS,

      .conversion_average = espp::TMAG5273::ConversionAverage::AVG_1,

      .low_noise = false,

      .auto_init = false,

  });

  // ==========================================================================
  // Initialize
  // ==========================================================================

  printf("Initializing TMAG5273...\n");

  if (!tmag.initialize(ec)) {
    printf("ERROR: TMAG5273 initialization failed: %s\n", ec.message().c_str());

    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  printf("TMAG5273 initialized successfully!\n\n");

  // ==========================================================================
  // Read device information
  // ==========================================================================

  printf("----------------------------------------\n");
  printf("Device Information\n");
  printf("----------------------------------------\n");

  uint8_t device_id = tmag.device_id(ec);

  if (ec) {
    printf("Device ID read failed: %s\n", ec.message().c_str());
    ec.clear();
  } else {
    printf("Device ID:       0x%02X\n", device_id);
  }

  uint16_t manufacturer_id = tmag.manufacturer_id(ec);

  if (ec) {
    printf("Manufacturer ID read failed: %s\n", ec.message().c_str());
    ec.clear();
  } else {
    printf("Manufacturer ID: 0x%04X\n", manufacturer_id);
  }

  // ==========================================================================
  // Read configuration
  // ==========================================================================

  printf("\n");
  printf("----------------------------------------\n");
  printf("Configuration\n");
  printf("----------------------------------------\n");

  auto channels = tmag.magnetic_channels(ec);

  if (!ec) {
    printf("Magnetic channels: 0x%02X\n", static_cast<uint8_t>(channels));
  } else {
    printf("Failed to read magnetic channels: %s\n", ec.message().c_str());
    ec.clear();
  }

  auto xy_range = tmag.xy_range(ec);

  if (!ec) {
    printf("XY range:         %s\n",
           xy_range == espp::TMAG5273::MagneticRange::LOW ? "LOW" : "HIGH");
  } else {
    printf("Failed to read XY range: %s\n", ec.message().c_str());
    ec.clear();
  }

  auto z_range = tmag.z_range(ec);

  if (!ec) {
    printf("Z range:          %s\n",
           z_range == espp::TMAG5273::MagneticRange::LOW ? "LOW" : "HIGH");
  } else {
    printf("Failed to read Z range: %s\n", ec.message().c_str());
    ec.clear();
  }

  auto mode = tmag.operating_mode(ec);

  if (!ec) {
    printf("Operating mode:   0x%02X\n", static_cast<uint8_t>(mode));
  } else {
    printf("Failed to read operating mode: %s\n", ec.message().c_str());
    ec.clear();
  }

  auto average = tmag.conversion_average(ec);

  if (!ec) {
    printf("Conversion avg:   0x%02X\n", static_cast<uint8_t>(average));
  } else {
    printf("Failed to read conversion average: %s\n", ec.message().c_str());
    ec.clear();
  }

  bool low_noise = tmag.low_noise(ec);

  if (!ec) {
    printf("Low noise:        %s\n", low_noise ? "ON" : "OFF");
  } else {
    printf("Failed to read low noise: %s\n", ec.message().c_str());
    ec.clear();
  }

  // ==========================================================================
  // Continuous magnetic measurement
  // ==========================================================================

  printf("\n");
  printf("----------------------------------------\n");
  printf("Magnetic Measurements\n");
  printf("----------------------------------------\n");

  printf("Reading X/Y/Z every 100 ms...\n");
  printf("Move a magnet near the sensor.\n\n");

  while (true) {

    ec.clear();

    auto magnetic = tmag.read_magnetic(ec);

    if (ec) {
      printf("Read error: %s\n", ec.message().c_str());

      ec.clear();

      std::this_thread::sleep_for(std::chrono::milliseconds(500));

      continue;
    }

    printf("X: %8.3f mT   "
           "Y: %8.3f mT   "
           "Z: %8.3f mT",
           magnetic.x, magnetic.y, magnetic.z);

    // --------------------------------------------------------------------------
    // Temperature
    // --------------------------------------------------------------------------

    float temperature = tmag.read_temperature(ec);

    if (!ec) {
      printf("   T: %6.2f C", temperature);
    } else {
      ec.clear();
    }

    // --------------------------------------------------------------------------
    // Conversion status
    // --------------------------------------------------------------------------

    bool ready = tmag.conversion_ready(ec);

    if (!ec) {
      printf("   Ready: %s", ready ? "YES" : "NO");
    } else {
      ec.clear();
    }

    printf("\n");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}