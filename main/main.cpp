#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <mutex>
#include <sdkconfig.h>
#include <span>
#include <system_error>
#include <thread>
#include <vector>

#include "esp_app_desc.h" // esp_app_get_description() -- firmware version banner at boot
#include "esp_system.h" // esp_restart() -- used to apply a live mode switch (see on_mode_changed below)

#include "calibration_service.hpp"
#include "dispatcher_worker.hpp"
#include "file_system.hpp"
#include "firmware_health_check.hpp"
#include "hid-rp-mouse.hpp"
#include "hid-rp.hpp"
#include "i2c.hpp"
#include "joystick.hpp"
#include "logger.hpp"
#include "omega_calibration.hpp"
#include "omega_control.hpp"
#include "omega_store.hpp"
#include "ota.hpp"
#include "qtpy.hpp"
#include "reset_log.hpp"
#include "task.hpp"
#include "tmag5273.hpp"
#include "usb_device.hpp"
#include "xac.hpp"

using namespace std::chrono_literals;

static constexpr uint8_t TMAG_ADDRESS = 0x35;

// USB ids. These are espp::UsbDevice's defaults (a borrowed pid.codes pair),
// spelled out so the calibration console's WebUSB filter (DEFAULT_VID /
// DEFAULT_PID in components/omega_calibration/web/calibration_console.html)
// has one place to match. Replace both with an allocated PID before shipping.
static constexpr uint16_t OMEGA_USB_VID = 0x1209;
static constexpr uint16_t OMEGA_USB_PID = 0x0d32;

// Manual per-axis trim, added on top of the measured center, ONLY on a
// fresh (never-before-calibrated) run -- see the calibration section below.
// Once calibrated, the manual offset lives in OmegaStore/DeviceSettings and
// is live-adjustable from the calibration console instead.
static constexpr float MANUAL_OFFSET_X = 0.0f;
static constexpr float MANUAL_OFFSET_Y = 0.0f;

// The nudge-click logic (SENSITIVITY, INVERT_NUDGE_Y, classify_nudge, scroll
// pacing), drift handling and the recalibration hand-off live in
// components/omega_control (host-tested). This file only wires hardware to it
// through EspPorts below.

// Sensor fault state as the control loop last reported it, read by the
// calibration service's GET_DIAGNOSTICS handler on the USB task. Plain
// relaxed atomics: each field is independently meaningful and a tick of
// skew between them does not matter.
struct SensorFaultStatus {
  std::atomic<uint8_t> fault{0};
  std::atomic<uint8_t> first_fault{0};
  std::atomic<uint32_t> count{0};

  void publish(bool faulted, espp::OmegaCalibration::SensorFaultMonitor::Fault fault_kind,
               uint32_t fault_count) {
    using Fault = espp::OmegaCalibration::SensorFaultMonitor::Fault;
    fault.store(faulted ? static_cast<uint8_t>(fault_kind) : static_cast<uint8_t>(Fault::NONE),
                std::memory_order_relaxed);
    if (faulted && first_fault.load(std::memory_order_relaxed) == 0) {
      first_fault.store(static_cast<uint8_t>(fault_kind), std::memory_order_relaxed);
    }
    count.store(fault_count, std::memory_order_relaxed);
  }
};

// Hardware side of espp::omega_control::Ports: HID reports, the recalibration
// sweep, and the store/joystick updates. The control loops themselves are
// omega_control::mouse_tick / xac_tick.
struct EspPorts : espp::omega_control::Ports {
  espp::TMAG5273 &tmag;
  espp::OmegaStore &store;
  espp::Joystick &js;
  espp::UsbDevice &usb;
  espp::CalibrationService &calibration_vendor;
  espp::CalibrationService &calibration_cdc;
  // Set by the joystick's read callback: Joystick::update() keeps its old
  // output when a read fails, so read_stick() learns the result from here.
  const bool &sensor_read_ok;
  const uint8_t &sensor_conversion_count; ///< same source; CONV_STATUS.SET_COUNT
  SensorFaultStatus &sensor_status;       ///< published for GET_DIAGNOSTICS

  EspPorts(espp::TMAG5273 &tmag_, espp::OmegaStore &store_, espp::Joystick &js_,
           espp::UsbDevice &usb_, espp::CalibrationService &vendor_, espp::CalibrationService &cdc_,
           const bool &sensor_read_ok_, const uint8_t &sensor_conversion_count_,
           SensorFaultStatus &sensor_status_)
      : tmag(tmag_)
      , store(store_)
      , js(js_)
      , usb(usb_)
      , calibration_vendor(vendor_)
      , calibration_cdc(cdc_)
      , sensor_read_ok(sensor_read_ok_)
      , sensor_conversion_count(sensor_conversion_count_)
      , sensor_status(sensor_status_) {}

  void send_mouse(const espp::omega_control::MouseReport &r) override {
    espp::MouseInputReport<0, 3, true> mouse;
    mouse.reset();
    mouse.set_button(1, (r.buttons & 0x01) != 0);
    mouse.set_button(2, (r.buttons & 0x02) != 0);
    mouse.set_movement(r.dx, r.dy);
    mouse.set_wheel(r.wheel);
    auto report = mouse.get_report();
    std::error_code hid_ec;
    if (!usb.write_hid_report(0, report, hid_ec)) {
      printf("HID send failed: %s\n", hid_ec.message().c_str());
    }
  }

  void send_xac(const espp::omega_control::XacReport &r) override {
    espp::XacGamepadInputReport xac_input_report;
    xac_input_report.reset();
    xac_input_report.set_joystick(r.x, r.y);
    auto report = xac_input_report.get_report();
    std::error_code hid_ec;
    if (!usb.write_hid_report(1, report, hid_ec)) {
      printf("HID send failed: %s\n", hid_ec.message().c_str());
    }
  }

  // On entry `center` holds the control loop's copy of the calibration; on
  // success both hold the finalised new results, with the store's live manual
  // offset carried over.
  bool run_sweep(espp::OmegaCalibration::CenterResult &center,
                 espp::OmegaCalibration::RangeResult &range) override {
    printf("\n=== RE-CALIBRATION REQUESTED (calibration console) ===\n");
    espp::OmegaCalibration::CenterResult new_center;
    espp::OmegaCalibration::RangeResult new_range;
    std::error_code recal_ec;
    espp::OmegaCalibration recal({
        .read =
            [this](float *x, float *y) {
              std::error_code read_ec;
              auto magnetic = tmag.read_magnetic(read_ec);
              if (read_ec)
                return false;
              *x = magnetic.x;
              *y = magnetic.y;
              return true;
            },
        .message = [](const std::string &text) { printf("%s\n", text.c_str()); },
        .verbosity = espp::Logger::Verbosity::ERROR,
    });
    bool recal_ok = recal.center_calibration(new_center, recal_ec) &&
                    recal.range_calibration(new_center, new_range, recal_ec);
    if (recal_ok) {
      // Recalibration redoes only the center/range sweep. A fresh
      // center_calibration() starts the manual offset at zero, so
      // set_measured_calibration() keeps the store's live trim, and we
      // read it back (deadzone overrides, nudge tuning and input mode
      // live in DeviceSettings, so they're untouched either way).
      recal.finalize_center_deadzone(new_center, new_range);
      range = new_range;
      // fires on_calibration_changed -> re-applies live
      store.set_measured_calibration(new_center, range);
      center = store.center();
      std::error_code save_ec;
      if (!store.save_to_flash(save_ec)) {
        printf("WARNING: recalibration succeeded but saving to flash failed: %s\n",
               save_ec.message().c_str());
      }
      printf("=== RE-CALIBRATION COMPLETE ===\n\n");
    } else {
      printf("ERROR: re-calibration failed: %s\n", recal_ec.message().c_str());
    }
    calibration_vendor.notify_calibration_result(recal_ok, recal_ok ? std::error_code{} : recal_ec);
    calibration_cdc.notify_calibration_result(recal_ok, recal_ok ? std::error_code{} : recal_ec);
    return recal_ok;
  }

  void save_to_flash() override {
    std::error_code save_ec;
    if (!store.save_to_flash(save_ec)) {
      printf("WARNING: Failed to persist drift-corrected calibration: %s\n",
             save_ec.message().c_str());
    }
  }

  void apply_drift(espp::OmegaCalibration::CenterResult &center,
                   const espp::OmegaCalibration::RangeResult &range) override {
    // Keeps the store (and console) in sync without reverting a manual
    // offset set since our copy was taken; read the live one back.
    store.set_measured_calibration(center, range);
    center = store.center();
    espp::OmegaCalibration::apply_calibration(js, center, range, store.settings());
  }

  espp::omega_control::StickSample read_stick() override {
    js.update();
    return {js.x(), js.y(), js.raw().x(), js.raw().y(), sensor_read_ok, sensor_conversion_count};
  }

  void sensor_fault_changed(bool faulted, espp::OmegaCalibration::SensorFaultMonitor::Fault fault,
                            std::uint32_t fault_count) override {
    sensor_status.publish(faulted, fault, fault_count);
    if (faulted) {
      printf("WARNING: sensor fault (%s); holding the stick centered\n",
             espp::OmegaCalibration::SensorFaultMonitor::to_string(fault));
    } else {
      printf("Sensor recovered (fault %u since boot)\n", static_cast<unsigned>(fault_count));
    }
  }

  espp::omega_control::Clock::time_point now() override {
    return espp::omega_control::Clock::now();
  }
};

// Where the calibration results are cached so the user doesn't have to
// recalibrate on every boot. Filename is relative to the file system's root
// (e.g. "/<partition_label>/omega_calibration.cfg") via
// OmegaCalibration::default_path(). DeviceSettings (deadzone overrides,
// nudge tuning, input mode) are cached separately -- see
// OmegaCalibration::DeviceSettings::default_path() -- and both are loaded
// together by OmegaStore below.
static const std::string CALIBRATION_FILE_NAME = "omega_calibration.cfg";

// app_main() itself runs on FreeRTOS's "main" task, whose stack
// (CONFIG_ESP_MAIN_TASK_STACK_SIZE) defaults to a very small 3584 bytes.
// I2C transactions, USB/HID setup, and the printf-heavy calibration flow
// easily blow through that, so all of the real work runs on a dedicated
// espp::Task with its own generously-sized stack instead.
static constexpr size_t MAIN_TASK_STACK_SIZE_BYTES = 16 * 1024;

// How many resets the reset-reason log keeps (see D23 in docs/decisions.md).
static constexpr size_t RESET_LOG_ENTRIES = 16;

static const char *reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_POWERON:
    return "power-on";
  case ESP_RST_EXT:
    return "external pin";
  case ESP_RST_SW:
    return "software";
  case ESP_RST_PANIC:
    return "panic";
  case ESP_RST_INT_WDT:
    return "interrupt watchdog";
  case ESP_RST_TASK_WDT:
    return "task watchdog";
  case ESP_RST_WDT:
    return "watchdog";
  case ESP_RST_DEEPSLEEP:
    return "deep sleep";
  case ESP_RST_BROWNOUT:
    return "brownout";
  case ESP_RST_SDIO:
    return "sdio";
  case ESP_RST_USB:
    return "usb";
  case ESP_RST_JTAG:
    return "jtag";
  case ESP_RST_EFUSE:
    return "efuse error";
  case ESP_RST_PWR_GLITCH:
    return "power glitch";
  case ESP_RST_CPU_LOCKUP:
    return "cpu lockup";
  case ESP_RST_UNKNOWN:
  default:
    return "unknown";
  }
}

// A reset nobody asked for: the firmware crashed, hung, or lost power
// unexpectedly. Power-on, the mode-switch esp_restart() and a USB or JTAG
// reset are all deliberate.
static bool reset_reason_is_abnormal(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_PANIC:
  case ESP_RST_INT_WDT:
  case ESP_RST_TASK_WDT:
  case ESP_RST_WDT:
  case ESP_RST_BROWNOUT:
  case ESP_RST_EFUSE:
  case ESP_RST_PWR_GLITCH:
  case ESP_RST_CPU_LOCKUP:
    return true;
  default:
    return false;
  }
}
// A firmware upload whose console vanished mid-transfer (cable pulled, tab
// closed) keeps the OTA engine claimed, which blocks a retry and REVERT. The
// polling loops drop such a session after this long without OTA traffic (see
// CalibrationService::expire_stale_update()).
static constexpr auto OTA_IDLE_TIMEOUT = 30s;

// Runs entirely on the dedicated "omega-stick" task (see app_main below),
// not on the small default main-task stack. Matches espp::Task's callback
// signature; m/cv are unused since this function never yields back to the
// task's own wait/dispatch loop -- it does its one-time setup and then
// never returns, spending the rest of its life in the polling loop at the
// bottom.

static bool run_omega_stick(std::mutex & /*m*/, std::condition_variable & /*cv*/) {
  // ==========================================================================
  // Reset-reason log (D23): why did we boot, and how have recent boots gone?
  // ==========================================================================
  // Recorded before anything that can fail, so a boot that dies in sensor
  // or USB setup still leaves its reason behind for the next one to print.
  // ResetLog::default_path() is the first use of espp::FileSystem, which
  // mounts the filesystem, so this has to come before the I2C and sensor
  // setup below.
  // Lives for the whole run (read-only after this block) so the calibration
  // console's GET_DIAGNOSTICS can report it.
  espp::ResetLog reset_log(
      {.path = espp::ResetLog::default_path(), .max_entries = RESET_LOG_ENTRIES});
  {
    std::error_code log_ec;
    if (!reset_log.load(log_ec)) {
      printf("WARNING: could not read the reset log: %s\n", log_ec.message().c_str());
    }
    const esp_reset_reason_t reason = esp_reset_reason();
    reset_log.record(static_cast<int>(reason), reset_reason_name(reason),
                     reset_reason_is_abnormal(reason));
    if (!reset_log.save(log_ec)) {
      printf("WARNING: could not save the reset log: %s\n", log_ec.message().c_str());
    }
    printf("Boot %u, reset reason: %s%s\n", static_cast<unsigned>(reset_log.boot_count()),
           reset_reason_name(reason), reset_reason_is_abnormal(reason) ? " (abnormal)" : "");
    if (reset_log.abnormal_count() > 0) {
      printf("%u of the last %u resets were abnormal:\n",
             static_cast<unsigned>(reset_log.abnormal_count()),
             static_cast<unsigned>(reset_log.entries().size()));
      for (const auto &entry : reset_log.entries()) {
        if (entry.abnormal) {
          printf("  boot %u: %s\n", static_cast<unsigned>(entry.boot), entry.name.c_str());
        }
      }
    }
  }

  // Published by the control loop through EspPorts::sensor_fault_changed;
  // served to the console together with the reset log by GET_DIAGNOSTICS
  // (D21, D23).
  SensorFaultStatus sensor_status;
  auto make_diagnostics = [&reset_log, &sensor_status]() {
    espp::CalibrationService::DiagnosticsData d;
    d.sensor_fault = sensor_status.fault.load(std::memory_order_relaxed);
    d.sensor_first_fault = sensor_status.first_fault.load(std::memory_order_relaxed);
    d.sensor_fault_count = sensor_status.count.load(std::memory_order_relaxed);
    d.boot_count = reset_log.boot_count();
    d.resets.reserve(reset_log.entries().size());
    for (const auto &entry : reset_log.entries()) {
      d.resets.push_back({.boot = entry.boot,
                          .reason = static_cast<uint8_t>(std::clamp(entry.reason, 0, 255)),
                          .abnormal = entry.abnormal,
                          .name = entry.name});
    }
    return d;
  };

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
                                       .log_level = espp::Logger::Verbosity::ERROR},
      ec);

  // ==========================================================================
  // Create callbacks for TMAG5273
  // ==========================================================================
  espp::TMAG5273 tmag(espp::TMAG5273::Config{

      .device_address = TMAG_ADDRESS,
      .write = espp::make_i2c_addressed_write(device),
      //.read = espp::make_i2c_addressed_read(device),
      .read_register = espp::make_i2c_addressed_read_register(device),
      .verbosity = espp::Logger::Verbosity::ERROR,
      .channels = espp::TMAG5273::MagneticChannels::XYZ,

      .xy_range = espp::TMAG5273::MagneticRange::HIGH,

      .z_range = espp::TMAG5273::MagneticRange::HIGH,

      .operating_mode = espp::TMAG5273::OperatingMode::CONTINUOUS,

      .conversion_average = espp::TMAG5273::ConversionAverage::AVG_32,

      .low_noise = false,

      .auto_init = false,

  });

  // OTA engine shared by every CalibrationService instance below (vendor + CDC, mouse + XAC).
  // Must outlive them; this function never returns, so a local is fine.
  espp::Ota ota({.reject_same_version = false, .log_level = espp::Logger::Verbosity::ERROR});
  {
    // Version banner: the same fields the console reports via GET_FW_INFO, so a
    // serial log and a bug report can be matched up.
    const esp_app_desc_t *app = esp_app_get_description();
    printf("Firmware %s %s (built %s %s, idf %s) running from %s%s\n", app->project_name,
           app->version, app->date, app->time, app->idf_ver, ota.running_partition_label().c_str(),
           ota.is_pending_verify() ? " [PENDING VERIFY]" : "");
  }

  // ==========================================================================
  // Initialize
  // ==========================================================================

  printf("Initializing TMAG5273...\n");

  if (!tmag.initialize(ec)) {
    printf("ERROR: TMAG5273 initialization failed: %s\n", ec.message().c_str());
    espp::FirmwareHealthCheck<>::reject_if_pending(ota, "TMAG5273 init failed");

    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  printf("TMAG5273 initialized successfully!\n\n");

#if CONFIG_OMEGA_STICK_BOOT_FLASH_WINDOW_S > 0
  // Development aid, off by default: until TinyUSB takes over the USB port,
  // the chip's USB Serial/JTAG can still reset it into the bootloader, so
  // `idf.py flash` works without holding BOOT. See main/Kconfig.projbuild.
  printf("Waiting %d s for flashing...\n", CONFIG_OMEGA_STICK_BOOT_FLASH_WINDOW_S);
  std::this_thread::sleep_for(std::chrono::seconds(CONFIG_OMEGA_STICK_BOOT_FLASH_WINDOW_S));
#endif

  // ==========================================================================
  // Calibration + live settings store
  // ==========================================================================
  const std::filesystem::path calibration_path =
      espp::OmegaCalibration::default_path(CALIBRATION_FILE_NAME);

  // Set once the mouse/XAC Joystick below is constructed, so a live
  // manual-offset or deadzone-override edit from the calibration console
  // (OmegaStore::on_calibration_changed, fired from CalibrationService) can
  // be re-applied immediately instead of only taking effect on next boot.
  espp::Joystick *live_joystick = nullptr;

  // NOTE: on_mode_changed/on_calibration_changed capture `store` by
  // reference before `store` itself finishes constructing. This is safe
  // here: the lambdas only take store's address (never dereference it)
  // until CalibrationService actually invokes them later, by which point
  // construction has long completed.
  espp::OmegaStore store({
      .calibration_path = calibration_path,
      .on_calibration_changed =
          [&live_joystick, &store]() {
            if (live_joystick) {
              espp::OmegaCalibration::apply_calibration(*live_joystick, store.center(),
                                                        store.range(), store.settings());
            }
          },
      .on_mode_changed =
          [&store](espp::OmegaStore::InputMode mode) {
            printf("Input mode changed to %s; saving settings and restarting to apply...\n",
                   mode == espp::OmegaStore::InputMode::XAC ? "XAC" : "MOUSE");
            std::error_code save_ec;
            if (!store.save_to_flash(save_ec)) {
              printf("WARNING: failed to save settings before restart: %s\n",
                     save_ec.message().c_str());
            }
            // CalibrationService sends the OK before calling this; give it a
            // moment to actually go out over USB before the reset tears the
            // link down.
            std::this_thread::sleep_for(200ms);
            esp_restart();
          },
      .log_level = espp::Logger::Verbosity::ERROR,
  });

  espp::OmegaCalibration::CenterResult center;
  espp::OmegaCalibration::RangeResult range;
  std::error_code cal_ec;

  bool have_calibration = espp::OmegaCalibration::load(center, range, calibration_path, cal_ec);
  bool run_calibration = !have_calibration;

  if (have_calibration) {
    printf("\nLoaded existing calibration.");
  }

  if (run_calibration) {
    printf("\n");
    printf("No usable cached calibration (%s); running interactive calibration.\n",
           cal_ec.message().c_str());

    printf("\n");
    printf("========================================\n");
    printf("       CENTER CALIBRATION\n");
    printf("========================================\n\n");

    espp::OmegaCalibration calibration({
        .read =
            [&tmag](float *x, float *y) {
              std::error_code read_ec;
              auto magnetic = tmag.read_magnetic(read_ec);
              if (read_ec) {
                return false;
              }
              *x = magnetic.x;
              *y = magnetic.y;
              return true;
            },
        .message = [](const std::string &text) { printf("%s\n", text.c_str()); },
        .verbosity = espp::Logger::Verbosity::ERROR,
    });

    if (!calibration.center_calibration(center, cal_ec)) {
      printf("ERROR: Center calibration failed: %s\n", cal_ec.message().c_str());
      espp::FirmwareHealthCheck<>::reject_if_pending(ota, "center calibration failed");
      while (true) {
        std::this_thread::sleep_for(1s);
      }
    }

    printf("\n");
    printf("========================================\n");
    printf("       RANGE CALIBRATION\n");
    printf("========================================\n\n");

    if (!calibration.range_calibration(center, range, cal_ec)) {
      printf("ERROR: Range calibration failed: %s\n", cal_ec.message().c_str());
      espp::FirmwareHealthCheck<>::reject_if_pending(ota, "range calibration failed");
      while (true) {
        std::this_thread::sleep_for(1s);
      }
    }

    calibration.finalize_center_deadzone(center, range);

    // Manual per-axis trim (see MANUAL_OFFSET_X/Y above) is applied here,
    // on a fresh calibration, and then persisted below. A LOADED
    // calibration keeps whatever offset was already saved instead of
    // being reset to these constants every boot -- that offset might have
    // been runtime-adjusted since (via the calibration console) and saved
    // again, and we don't want to clobber that.
    espp::OmegaCalibration::set_manual_offset(center, MANUAL_OFFSET_X, MANUAL_OFFSET_Y);

    if (!espp::OmegaCalibration::save(center, range, calibration_path, cal_ec)) {
      // Not fatal: we still have a valid in-memory calibration, it just
      // won't be cached for next boot.
      printf("WARNING: Failed to save calibration to %s: %s\n", calibration_path.c_str(),
             cal_ec.message().c_str());
    } else {
      printf("Saved calibration to %s\n", calibration_path.c_str());
    }
  }

  // Keep the store's cached copy in sync with whatever we just loaded/ran,
  // regardless of which path above produced it.
  store.set_calibration(center, range);

  // Mode is now a live setting rather than a hardcoded constant -- flip it
  // from the calibration console (which saves + restarts to apply it).
  const bool XAC_SELECTION = store.settings().input_mode == espp::OmegaStore::InputMode::XAC;

  // ==========================================================================
  // Recalibration-on-demand (calibration console's START_CALIBRATION, module
  // 5, see calibration_service.hpp)
  // ==========================================================================
  //
  // The interactive center/range calibration sequence reads the SAME tmag
  // sensor and drives the SAME espp::Joystick the polling loop below uses
  // every 10ms (mouse branch) / 5ms (XAC branch). Running it on a second,
  // separately-scheduled task -- the obvious way to make START_CALIBRATION
  // "async" from the host's point of view -- would let that task's I2C
  // transactions interleave with the polling loop's own, which is not
  // something espp::I2c/TMAG5273 are documented here as supporting.
  //
  // Instead, CalibrationService::Config::start_calibration below does the
  // absolute minimum required to satisfy its "return almost immediately"
  // contract: it just raises this flag. The SAME task that already owns
  // exclusive access to tmag/js -- the polling loop a few dozen lines down
  // -- polls the flag once per iteration and, when set, temporarily steps
  // out of normal mouse/XAC operation to run the calibration sequence
  // itself, inline, exactly like the fresh-boot path above does. This is
  // still "async" as far as the calibration console's request/reply pair is
  // concerned (the OK reply for START_CALIBRATION returns immediately,
  // before the sequence has even started), it just does the actual work on
  // the one task that is allowed to touch the sensor.
  std::atomic<bool> recalibration_requested{false};

  // ============================================================================
  // Final results
  // ============================================================================

  printf("\n");
  printf("========================================\n");
  printf("       CALIBRATION RESULTS\n");
  printf("========================================\n");

  printf("\nCENTER\n");
  printf("----------------------------------------\n");

  printf("Center X:                  %.6f\n", center.center_x);

  printf("Center Y:                  %.6f\n", center.center_y);

  printf("99%% noise radius:          %.6f\n", center.noise_radius);

  printf("Center margin:             %.2fx\n", espp::OmegaCalibration::CENTER_MARGIN);

  printf("Center deadzone (auto):    %.6f\n", center.deadzone);

  printf("Center deadzone (auto):    %.2f%%\n", center.deadzone * 100.0f);

  printf("Manual offset X:           %.6f\n", center.manual_offset_x);

  printf("Manual offset Y:           %.6f\n", center.manual_offset_y);

  printf("Effective center X:        %.6f\n", center.effective_center_x());

  printf("Effective center Y:        %.6f\n", center.effective_center_y());

  printf("\nRANGE\n");
  printf("----------------------------------------\n");

  printf("Usable axis radius:        %.6f\n", range.usable_radius);

  printf("Reliable max radius:       %.6f\n", range.reliable_max_radius);

  printf("Normalized max radius:     %.6f\n", range.normalized_max_radius);

  printf("Range deadzone (auto):     %.6f\n", range.deadzone);

  printf("Range deadzone (auto):     %.2f%%\n", range.deadzone * 100.0f);

  printf("\nRAW AXIS CALIBRATION\n");
  printf("----------------------------------------\n");

  printf("X minimum:                 %.6f\n", center.effective_center_x() + range.min_x);

  printf("X maximum:                 %.6f\n", center.effective_center_x() + range.max_x);

  printf("Y minimum:                 %.6f\n", center.effective_center_y() + range.min_y);

  printf("Y maximum:                 %.6f\n", center.effective_center_y() + range.max_y);

  if (!XAC_SELECTION) { // MOUSE mode; the XAC branch is the else below

    printf("\n");
    printf("========================================\n");
    printf("       MOUSE CONFIGURATION\n");
    printf("========================================\n\n");

    std::vector<uint8_t> hid_report_descriptor;
    {
      // WITH_WHEEL=true: nudge-click needs a relative wheel axis to scroll.
      auto raw = espp::mouse_app_report_descriptor<0, 3, true>();
      hid_report_descriptor.assign(raw.begin(), raw.end());
    }
    printf("HID mouse report descriptor: %zu bytes\n", hid_report_descriptor.size());

    espp::UsbDevice::Config usb_cfg;
    usb_cfg.manufacturer = "espp";
    usb_cfg.product = "omega-stick mouse";
    usb_cfg.vid = OMEGA_USB_VID;
    usb_cfg.pid = OMEGA_USB_PID;
    espp::UsbDevice::CdcFunction cdc_fn;
    cdc_fn.route_console = true;
    usb_cfg.cdc = cdc_fn;

    espp::UsbDevice::HidFunction hid_fn;
    hid_fn.interface_name = "omega-stick HID";
    hid_fn.report_descriptor = hid_report_descriptor;
    hid_fn.poll_interval_ms = 10;
    usb_cfg.hid = hid_fn;

    // --------------------------------------------------------------------
    // Calibration console wiring (module 5), checked against usb_device.hpp
    // (espp/usb_device 1.3.3) and dispatcher_worker.hpp (espp/dispatcher
    // 1.3.5), see dependencies.lock. One CalibrationService and
    // one DispatcherWorker per USB byte stream (vendor for WebUSB, CDC for
    // Web Serial). The receive callbacks only push() bytes into the worker:
    // they run on the TinyUSB task, where UsbDevice's write path fails fast
    // instead of waiting for FIFO room, so replying from there could drop
    // frames. The worker parses and dispatches on its own task, and also
    // answers the console's module-0xFF discovery request.
    // --------------------------------------------------------------------
    static espp::UsbDevice *usb_ptr = nullptr; // set once `usb` exists, below
    espp::CalibrationService::Config calibration_cfg{
        .store = store,
        .send =
            [](std::span<const uint8_t> frame) {
              if (usb_ptr)
                usb_ptr->write_vendor(frame);
            },
        .start_calibration =
            [&recalibration_requested](std::error_code &start_ec) {
              bool expected = false;
              if (!recalibration_requested.compare_exchange_strong(expected, true)) {
                start_ec = std::make_error_code(std::errc::device_or_resource_busy);
                return false;
              }
              return true;
            },
        .restart = []() { esp_restart(); },
        .ota = &ota,
        .diagnostics = make_diagnostics,
        .log_level = espp::Logger::Verbosity::ERROR,
    };
    espp::CalibrationService calibration_vendor(calibration_cfg);
    calibration_cfg.send = [](std::span<const uint8_t> frame) {
      if (usb_ptr)
        usb_ptr->write_cdc(frame);
    };
    espp::CalibrationService calibration_cdc(calibration_cfg);
    espp::DispatcherWorker vendor_link({
        .send =
            [](std::span<const uint8_t> frame) {
              if (usb_ptr)
                usb_ptr->write_vendor(frame);
            },
        .task_config = {.name = "cal_vendor_rx", .stack_size_bytes = 8192},
    });
    vendor_link.register_module(calibration_vendor);
    vendor_link.serve_discovery("omega-stick");
    espp::DispatcherWorker cdc_link({
        .send =
            [](std::span<const uint8_t> frame) {
              if (usb_ptr)
                usb_ptr->write_cdc(frame);
            },
        .task_config = {.name = "cal_cdc_rx", .stack_size_bytes = 8192},
    });
    cdc_link.register_module(calibration_cdc);
    cdc_link.serve_discovery("omega-stick");

    espp::UsbDevice::VendorFunction vendor_fn;
    vendor_fn.interface_name = "omega-stick vendor";
    vendor_fn.on_receive = [&vendor_link](std::span<const uint8_t> data) {
      vendor_link.push(data);
    };
    usb_cfg.vendor = vendor_fn;
    cdc_fn.on_receive = [&cdc_link](std::span<const uint8_t> data) { cdc_link.push(data); };
    usb_cfg.cdc = cdc_fn; // re-assign now that on_receive is set

    espp::UsbDevice usb(usb_cfg);
    // cppcheck-suppress danglingLifetime ; run_omega_stick() never returns, so usb outlives usb_ptr
    usb_ptr = &usb;

    std::error_code usb_ec;
    if (!usb.initialize(usb_ec)) {
      printf("ERROR: USB init failed: %s\n", usb_ec.message().c_str());
      espp::FirmwareHealthCheck<>::reject_if_pending(ota, "USB init failed");
      while (true) {
        std::this_thread::sleep_for(1s);
      }
    }

    // Wait for the host to enumerate and open the HID endpoint.
    printf("Waiting for USB host...\n");
    for (int i = 0; i < 100 && !usb.is_hid_ready(); i++) {
      std::this_thread::sleep_for(100ms);
    }
    printf("HID ready: %s\n", usb.is_hid_ready() ? "yes" : "no");

    bool sensor_read_ok = true;
    uint8_t sensor_conversion_count = 0;
    espp::Joystick js(espp::OmegaCalibration::make_joystick_config(
        center, range, store.settings(),
        [&tmag, &sensor_read_ok, &sensor_conversion_count](float *x, float *y) {
          std::error_code read_ec;
          auto magnetic = tmag.read_magnetic(read_ec);
          sensor_read_ok = !read_ec;
          sensor_conversion_count = magnetic.conversion_count;
          if (read_ec) {
            return false;
          }
          *x = magnetic.x;
          *y = magnetic.y;
          return true;
        }));
    live_joystick = &js; // let on_calibration_changed re-apply live edits

    std::this_thread::sleep_for(1s);

    // Nudge-click state, drift compensation and the recalibration hand-off
    // live in omega_control (see components/omega_control).
    EspPorts ports(tmag, store, js, usb, calibration_vendor, calibration_cdc, sensor_read_ok,
                   sensor_conversion_count, sensor_status);
    espp::omega_control::MouseState state;
    state.common.center = center;
    state.common.range = range;

    espp::FirmwareHealthCheck<> fw_health(ota);
    while (true) {
      fw_health.tick(usb.is_hid_ready(), [&tmag] {
        std::error_code read_ec;
        tmag.read_magnetic(read_ec);
        return !read_ec;
      });
      // Cheap when idle: a try_lock and a timestamp compare per service.
      calibration_vendor.expire_stale_update(OTA_IDLE_TIMEOUT);
      calibration_cdc.expire_stale_update(OTA_IDLE_TIMEOUT);

      // The recalibration sweep itself runs inside mouse_tick (via
      // EspPorts::run_sweep), on this task, so it never races the sensor reads.
      const bool recal = recalibration_requested.exchange(false);
      const auto sample = ports.read_stick();
      mouse_tick(state,
                 {.stick = sample,
                  .now = espp::omega_control::Clock::now(),
                  .recalibration_requested = recal,
                  .settings = store.settings()},
                 ports);
      std::this_thread::sleep_for(10ms);
    }
  } else {
    using XacInput = espp::XacGamepadInputReport;

    // ------------------------------------------------------------
    // USB HID configuration
    // ------------------------------------------------------------

    std::vector<uint8_t> hid_report_descriptor;
    {
      auto raw = XacInput::get_descriptor();
      hid_report_descriptor.assign(raw.begin(), raw.end());
    }

    printf("XAC HID report descriptor: %zu bytes\n", hid_report_descriptor.size());

    espp::UsbDevice::Config usb_cfg;
    usb_cfg.manufacturer = "espp";
    usb_cfg.product = "omega-stick XAC";
    usb_cfg.vid = OMEGA_USB_VID;
    usb_cfg.pid = OMEGA_USB_PID;

    espp::UsbDevice::CdcFunction cdc_fn;
    cdc_fn.route_console = true;
    usb_cfg.cdc = cdc_fn;

    espp::UsbDevice::HidFunction hid_fn;
    hid_fn.interface_name = "omega-stick XAC";
    hid_fn.report_descriptor = hid_report_descriptor;
    hid_fn.poll_interval_ms = 10;

    usb_cfg.hid = hid_fn;

    // Same calibration-console wiring as the mouse branch above (see the
    // comment there) -- kept here too so the console still works when the
    // stick boots in XAC mode.
    static espp::UsbDevice *usb_ptr = nullptr;
    espp::CalibrationService::Config calibration_cfg{
        .store = store,
        .send =
            [](std::span<const uint8_t> frame) {
              if (usb_ptr)
                usb_ptr->write_vendor(frame);
            },
        .start_calibration =
            [&recalibration_requested](std::error_code &start_ec) {
              bool expected = false;
              if (!recalibration_requested.compare_exchange_strong(expected, true)) {
                start_ec = std::make_error_code(std::errc::device_or_resource_busy);
                return false;
              }
              return true;
            },
        .restart = []() { esp_restart(); },
        .ota = &ota,
        .diagnostics = make_diagnostics,
        .log_level = espp::Logger::Verbosity::ERROR,
    };
    espp::CalibrationService calibration_vendor(calibration_cfg);
    calibration_cfg.send = [](std::span<const uint8_t> frame) {
      if (usb_ptr)
        usb_ptr->write_cdc(frame);
    };
    espp::CalibrationService calibration_cdc(calibration_cfg);
    espp::DispatcherWorker vendor_link({
        .send =
            [](std::span<const uint8_t> frame) {
              if (usb_ptr)
                usb_ptr->write_vendor(frame);
            },
        .task_config = {.name = "cal_vendor_rx", .stack_size_bytes = 8192},
    });
    vendor_link.register_module(calibration_vendor);
    vendor_link.serve_discovery("omega-stick");
    espp::DispatcherWorker cdc_link({
        .send =
            [](std::span<const uint8_t> frame) {
              if (usb_ptr)
                usb_ptr->write_cdc(frame);
            },
        .task_config = {.name = "cal_cdc_rx", .stack_size_bytes = 8192},
    });
    cdc_link.register_module(calibration_cdc);
    cdc_link.serve_discovery("omega-stick");
    espp::UsbDevice::VendorFunction vendor_fn;
    vendor_fn.interface_name = "omega-stick vendor";
    vendor_fn.on_receive = [&vendor_link](std::span<const uint8_t> data) {
      vendor_link.push(data);
    };
    usb_cfg.vendor = vendor_fn;
    cdc_fn.on_receive = [&cdc_link](std::span<const uint8_t> data) { cdc_link.push(data); };
    usb_cfg.cdc = cdc_fn;

    espp::UsbDevice usb(usb_cfg);
    // cppcheck-suppress danglingLifetime ; run_omega_stick() never returns, so usb outlives usb_ptr
    usb_ptr = &usb;

    std::error_code usb_ec;

    if (!usb.initialize(usb_ec)) {
      printf("ERROR: USB init failed: %s\n", usb_ec.message().c_str());
      espp::FirmwareHealthCheck<>::reject_if_pending(ota, "USB init failed");

      while (true) {
        std::this_thread::sleep_for(1s);
      }
    }

    printf("Waiting for USB host...\n");

    for (int i = 0; i < 100 && !usb.is_hid_ready(); i++) {
      std::this_thread::sleep_for(100ms);
    }

    printf("HID ready: %s\n", usb.is_hid_ready() ? "yes" : "no");

    // ------------------------------------------------------------
    // Joystick
    // ------------------------------------------------------------

    bool sensor_read_ok = true;
    uint8_t sensor_conversion_count = 0;
    espp::Joystick js(espp::OmegaCalibration::make_joystick_config(
        center, range, store.settings(),
        [&tmag, &sensor_read_ok, &sensor_conversion_count](float *x, float *y) {
          std::error_code read_ec;

          auto magnetic = tmag.read_magnetic(read_ec);
          sensor_read_ok = !read_ec;
          sensor_conversion_count = magnetic.conversion_count;

          if (read_ec) {
            return false;
          }

          *x = magnetic.x;
          *y = magnetic.y;

          return true;
        }));
    live_joystick = &js;

    std::this_thread::sleep_for(1s);

    // ------------------------------------------------------------
    // HID polling loop
    // ------------------------------------------------------------

    EspPorts ports(tmag, store, js, usb, calibration_vendor, calibration_cdc, sensor_read_ok,
                   sensor_conversion_count, sensor_status);
    espp::omega_control::XacState state;
    state.common.center = center;
    state.common.range = range;

    espp::FirmwareHealthCheck<> fw_health(ota);
    while (true) {
      fw_health.tick(usb.is_hid_ready(), [&tmag] {
        std::error_code read_ec;
        tmag.read_magnetic(read_ec);
        return !read_ec;
      });
      // Cheap when idle: a try_lock and a timestamp compare per service.
      calibration_vendor.expire_stale_update(OTA_IDLE_TIMEOUT);
      calibration_cdc.expire_stale_update(OTA_IDLE_TIMEOUT);

      // The recalibration sweep itself runs inside xac_tick (via
      // EspPorts::run_sweep), on this task, so it never races the sensor reads.
      const bool recal = recalibration_requested.exchange(false);
      const auto sample = ports.read_stick();
      xac_tick(state,
               {.stick = sample,
                .now = espp::omega_control::Clock::now(),
                .recalibration_requested = recal,
                .settings = store.settings()},
               ports);
      std::this_thread::sleep_for(5ms);
    }
  }
}

extern "C" void app_main(void) {
  // Keep the task object alive for the lifetime of the program -- if it
  // were a local variable here, it would be destroyed (stopping the task)
  // the moment app_main() fell through to the sleep loop below.

  static auto task = espp::Task::make_unique(espp::Task::Config{
      .callback = run_omega_stick,
      .task_config =
          {
              .name = "omega-stick",
              .stack_size_bytes = MAIN_TASK_STACK_SIZE_BYTES,
          },
      .log_level = espp::Logger::Verbosity::ERROR,
  });
  task->start();

  // app_main()'s own task can now stay small and idle forever; all the
  // real work happens on the "omega-stick" task above.
  while (true) {
    std::this_thread::sleep_for(1s);
  }
}