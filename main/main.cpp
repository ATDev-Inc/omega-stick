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
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "esp_app_desc.h" // esp_app_get_description() -- firmware version banner at boot
#include "esp_attr.h"     // RTC_NOINIT_ATTR -- the crash-loop handover marker
#include "esp_mac.h"      // esp_efuse_mac_get_default() -- per-chip USB serial number
#include "esp_ota_ops.h"  // esp_ota_get_running_partition() -- slot tag in the reset log
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
// board with no saved calibration (it is carried into the first console
// calibration) -- see the calibration section below.
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
  // Starts "not observed" (D22): the console must not read the boot-time zero
  // as a healthy sensor before the polling loop has seen it answer.
  std::atomic<uint8_t> fault{espp::omega_control::SENSOR_NOT_OBSERVED};
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

  // The sensor has answered enough polls in a row. Only replaces "not
  // observed": a fault published meanwhile stays until it clears.
  void observed_healthy() {
    uint8_t expected = espp::omega_control::SENSOR_NOT_OBSERVED;
    fault.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
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
  std::atomic<bool> &needs_calibration;   ///< D26 hold; a successful sweep clears it
  /// The sweep opens its FINISH_CALIBRATION window here and polls it.
  espp::omega_control::RecalibrationGate &recalibration_gate;

  EspPorts(espp::TMAG5273 &tmag_, espp::OmegaStore &store_, espp::Joystick &js_,
           espp::UsbDevice &usb_, espp::CalibrationService &vendor_, espp::CalibrationService &cdc_,
           const bool &sensor_read_ok_, const uint8_t &sensor_conversion_count_,
           SensorFaultStatus &sensor_status_, std::atomic<bool> &needs_calibration_,
           espp::omega_control::RecalibrationGate &recalibration_gate_)
      : tmag(tmag_)
      , store(store_)
      , js(js_)
      , usb(usb_)
      , calibration_vendor(vendor_)
      , calibration_cdc(cdc_)
      , sensor_read_ok(sensor_read_ok_)
      , sensor_conversion_count(sensor_conversion_count_)
      , sensor_status(sensor_status_)
      , needs_calibration(needs_calibration_)
      , recalibration_gate(recalibration_gate_) {}

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
  // offset carried over. range_calibration() refuses a result that fails
  // OmegaCalibration::check_usable(), so an unusable sweep is a failure here:
  // nothing is applied or saved, and a needs-calibration hold stays (D26).
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
        // CALIBRATION_PROGRESS to both links (refusals are dropped quietly),
        // and the FINISH_CALIBRATION window follows the range stage.
        .progress =
            [this](const espp::OmegaCalibration::Progress &p) {
              recalibration_gate.sweep_stage(p.stage);
              calibration_vendor.notify_calibration_progress(p);
              calibration_cdc.notify_calibration_progress(p);
            },
        .finish_requested = [this] { return recalibration_gate.take_finish(); },
        .verbosity = espp::Logger::Verbosity::ERROR,
    });
    bool recal_ok = recal.center_calibration(new_center, recal_ec) &&
                    recal.range_calibration(new_center, new_range, recal_ec);
    recalibration_gate.set_finish_window(false);
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
      if (needs_calibration.exchange(false)) {
        printf("Calibration is usable; output resumes.\n");
      }
    } else {
      printf("ERROR: re-calibration failed: %s\n", recal_ec.message().c_str());
      if (needs_calibration.load()) {
        printf("Output stays neutral until a calibration succeeds.\n");
      }
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

  void sensor_observed_healthy() override { sensor_status.observed_healthy(); }

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

// USB serial number: the factory MAC as 12 lowercase hex digits, unique per
// chip. Without it every stick reports espp's default, so the console cannot
// tell two sticks apart and a persisted WebUSB permission is shared by all.
static std::string usb_serial_number() {
  uint8_t mac[6] = {};
  if (esp_efuse_mac_get_default(mac) != ESP_OK) {
    printf("WARNING: could not read the factory MAC; using the default USB serial\n");
    return "000000000001";
  }
  char buf[13];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5]);
  return buf;
}

// The subset of abnormal resets that point at the firmware itself: the kind a
// different image might not have. Brownouts and power glitches are left out,
// since the other slot would not survive them either (D18 amendment).
static bool reset_reason_is_crash(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_PANIC:
  case ESP_RST_INT_WDT:
  case ESP_RST_TASK_WDT:
  case ESP_RST_WDT:
  case ESP_RST_CPU_LOCKUP:
    return true;
  default:
    return false;
  }
}

// The reset name as the console shows it: the slot is appended in
// parentheses ("panic (ota_0)"), the wire text older consoles already read.
// The slot itself lives in its own field in the log.
static std::string reset_log_display_name(const espp::ResetLog::Entry &entry) {
  return entry.slot.empty() ? entry.name : entry.name + " (" + entry.slot + ")";
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
static constexpr auto OTA_IDLE_TIMEOUT = espp::CalibrationService::OTA_IDLE_TIMEOUT;

// The console's live position display (SET_POSITION_STREAM): every
// POSITION_STREAM_PERIOD the polling loops send the latest raw reading as a
// stage-0 CALIBRATION_PROGRESS to each link whose lease is running. Idle cost
// is one clock read and a compare per tick. The center is the live
// calibration's, unless there is none usable (D26).
static void stream_position(espp::CalibrationService &vendor, espp::CalibrationService &cdc,
                            const espp::omega_control::StickSample &sample,
                            const espp::OmegaStore &store, bool calibrated,
                            espp::omega_control::Clock::time_point &next_at) {
  const auto now = espp::omega_control::Clock::now();
  if (now < next_at) {
    return;
  }
  next_at = now + espp::CalibrationService::POSITION_STREAM_PERIOD;
  if (!vendor.position_stream_active(now) && !cdc.position_stream_active(now)) {
    return;
  }
  const espp::OmegaCalibration::CenterResult center = store.center();
  const auto live = espp::OmegaCalibration::live_progress(
      sample.read_ok, sample.raw_x, sample.raw_y, calibrated ? &center : nullptr);
  vendor.notify_position(live, now);
  cdc.notify_position(live, now);
}

#if CONFIG_OMEGA_STICK_OTA
// The crash-loop handover's note to the boot it starts, in memory that a
// software reset keeps and a power-on loses. See
// FirmwareHealthCheck::take_handover_marker().
static RTC_NOINIT_ATTR uint32_t crash_loop_handover_marker;

// Acts on FirmwareHealthCheck::tick() once per polling-loop iteration. Past the
// deadline the output is held neutral for ROLLBACK_HOLD before the rollback
// reboots, so the host is not left with a deflection (D22 rule 1).
static void run_health_check(espp::FirmwareHealthCheck<> &health, std::atomic<int> &output_holds) {
  switch (health.tick()) {
  case espp::FirmwareHealthCheck<>::Action::HOLD_OUTPUT:
    output_holds.fetch_add(1);
    break;
  case espp::FirmwareHealthCheck<>::Action::ROLL_BACK:
    health.rollback_now(); // only returns if the host confirmed meanwhile or it failed
    output_holds.fetch_sub(1);
    break;
  case espp::FirmwareHealthCheck<>::Action::NONE:
    break;
  }
}
#endif

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
  // Each entry records the slot that boot ran in (Entry::slot), so
  // the crash-loop check below can tell this image's crashes from the other
  // image's (see FirmwareHealthCheck::crashes_while_running).
  const esp_partition_t *running_slot = esp_ota_get_running_partition();
  const std::string running_slot_label = running_slot ? running_slot->label : "factory";
  std::vector<espp::FirmwareHealthCheck<>::BootRecord> boots; // oldest first, filled below
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
                     reset_reason_is_abnormal(reason), running_slot_label);
    for (const auto &entry : reset_log.entries()) {
      boots.push_back(
          {.slot = entry.slot,
           .crash = reset_reason_is_crash(static_cast<esp_reset_reason_t>(entry.reason))});
    }
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
          printf("  boot %u: %s\n", static_cast<unsigned>(entry.boot),
                 reset_log_display_name(entry).c_str());
        }
      }
    }
  }

#if CONFIG_OMEGA_STICK_OTA
  // OTA engine shared by every CalibrationService instance below (vendor + CDC, mouse + XAC).
  // Must outlive them; this function never returns, so a local is fine.
  espp::Ota ota({.reject_same_version = false, .log_level = espp::Logger::Verbosity::ERROR});
  espp::Ota *const ota_engine = &ota;

  // A confirmed image that has crashed on its last few boots hands the device
  // to the other slot (D18 amendment); a pending one is the bootloader's job.
  // It runs here, before any peripheral is touched, because a crash loop in
  // the I2C or sensor setup is exactly the case it exists for. At most once
  // per power-on: if the image it hands over to crash-loops too, both are
  // broken, and swapping between them would only wear out the otadata sectors.
  using HealthCheck = espp::FirmwareHealthCheck<>;
  const auto handover = HealthCheck::take_handover_marker(crash_loop_handover_marker,
                                                          esp_reset_reason() == ESP_RST_POWERON);
  if (!handover.handed_over_since_power_on &&
      HealthCheck::revert_if_crash_looping(
          ota, HealthCheck::crashes_while_running(boots, running_slot_label))) {
    crash_loop_handover_marker = HealthCheck::HANDOVER_REQUESTED;
    std::this_thread::sleep_for(100ms); // let the message leave the console
    esp_restart();
  }
  // The handover boots the other image pending (esp_ota_set_boot_partition()
  // writes it as NEW), and with no console attached nothing would confirm it:
  // the deadline would roll it back into the crash loop. It ran before and was
  // not rejected, so it is accepted now: the one image the firmware confirms.
  HealthCheck::accept_after_handover(ota, handover.booted_by_handover);
#else
  // Firmware updates over USB are off in this build (D25): no OTA engine, so
  // the console's update messages reply function_not_supported, and there is
  // no pending image to confirm or roll back.
  espp::Ota *const ota_engine = nullptr;
  printf("Firmware updates over USB are off in this build; install new firmware from the "
         "first-flash page.\n");
  // This image can still arrive by update (from an OTA-on build), which boots it
  // pending verification. Nothing here can confirm it (the console's MARK_VALID
  // is refused and there is no health check), so the bootloader would roll it
  // back at the next reset. Accept it now, before any peripheral bring-up.
  {
    esp_ota_img_states_t img_state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && esp_ota_get_state_partition(running, &img_state) == ESP_OK &&
        img_state == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
      printf("This image arrived by update and cannot be confirmed by the console, so it "
             "accepted itself.\n");
    }
  }
#endif
  // Tells the bootloader to roll back a pending image that cannot work. A no-op
  // when updates are off: an image flashed over serial is never pending, and one that
  // arrived by update accepted itself at boot (above).
  auto reject_if_pending = [&](const char *reason) {
#if CONFIG_OMEGA_STICK_OTA
    espp::FirmwareHealthCheck<>::reject_if_pending(ota, reason);
#else
    (void)reason;
#endif
  };
  {
    // Version banner: the same fields the console reports via GET_FW_INFO, so a
    // serial log and a bug report can be matched up.
    const esp_app_desc_t *app = esp_app_get_description();
#if CONFIG_OMEGA_STICK_OTA
    const char *pending = ota.is_pending_verify() ? " [PENDING VERIFY]" : "";
#else
    const char *pending = "";
#endif
    printf("Firmware %s %s (built %s %s, idf %s) running from %s%s\n", app->project_name,
           app->version, app->date, app->time, app->idf_ver, running_slot_label.c_str(), pending);
  }
  // Published by the control loop through EspPorts::sensor_fault_changed;
  // served to the console together with the reset log by GET_DIAGNOSTICS
  // (D21, D23).
  SensorFaultStatus sensor_status;
  // D26: set at boot when there is no usable calibration, cleared by
  // EspPorts::run_sweep when a console calibration succeeds. Read by the
  // polling loop (neutral output while set) and by GET_DIAGNOSTICS.
  std::atomic<bool> needs_calibration{false};
  auto make_diagnostics = [&reset_log, &sensor_status, &needs_calibration]() {
    espp::CalibrationService::DiagnosticsData d;
    d.calibration_state = needs_calibration.load() ? espp::CalibrationService::CALIBRATION_NEEDED
                                                   : espp::CalibrationService::CALIBRATION_USABLE;
    d.sensor_fault = sensor_status.fault.load(std::memory_order_relaxed);
    d.sensor_first_fault = sensor_status.first_fault.load(std::memory_order_relaxed);
    d.sensor_fault_count = sensor_status.count.load(std::memory_order_relaxed);
    d.boot_count = reset_log.boot_count();
    d.resets.reserve(reset_log.entries().size());
    for (const auto &entry : reset_log.entries()) {
      d.resets.push_back({.boot = entry.boot,
                          .reason = static_cast<uint8_t>(std::clamp(entry.reason, 0, 255)),
                          .abnormal = entry.abnormal,
                          .name = reset_log_display_name(entry)});
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

  // ==========================================================================
  // Initialize
  // ==========================================================================

  printf("Initializing TMAG5273...\n");

  if (!tmag.initialize(ec)) {
    printf("ERROR: TMAG5273 initialization failed: %s\n", ec.message().c_str());
    reject_if_pending("TMAG5273 init failed");

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
            // CalibrationService queues the OK before calling this; give it
            // the same time to go out over USB as the other console reboots
            // before the reset tears the link down.
            std::this_thread::sleep_for(espp::CalibrationService::REPLY_FLUSH_DELAY);
            esp_restart();
          },
      .log_level = espp::Logger::Verbosity::ERROR,
  });

  espp::OmegaCalibration::CenterResult center;
  espp::OmegaCalibration::RangeResult range;
  std::error_code cal_ec;

  // D26: without a usable calibration the stick boots quiet. USB, the console
  // and the control loop all start, but the output is held neutral (the
  // needs_calibration hold, see omega_control::holds_output()) until a
  // START_CALIBRATION from the console succeeds. No sweep runs at boot: its
  // prompts only reach a serial log, and a sweep of a stick nobody moved saved
  // a calibration that turned resting noise into cursor movement.
  if (espp::OmegaCalibration::load(center, range, calibration_path, cal_ec)) {
    printf("\nLoaded existing calibration.");
  } else {
    needs_calibration.store(true);
    if (cal_ec != espp::OmegaCalibration::make_error_code(
                      espp::OmegaCalibration::Error::calibration_unusable)) {
      // Nothing usable was read. Start from zero with the manual per-axis trim
      // (see MANUAL_OFFSET_X/Y above), which the console calibration keeps and
      // then persists. A saved but unusable calibration keeps its own offset
      // instead: it might have been adjusted from the console since.
      center = {};
      range = {};
      espp::OmegaCalibration::set_manual_offset(center, MANUAL_OFFSET_X, MANUAL_OFFSET_Y);
    }
    printf("\nNo usable calibration (%s).\n", cal_ec.message().c_str());
    printf("Output is held neutral until a calibration succeeds: connect the calibration "
           "console and use \"Re-run calibration\".\n");
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
  // itself, inline. This is still "async" as far as the calibration
  // console's request/reply pair is
  // concerned (the OK reply for START_CALIBRATION returns immediately,
  // before the sequence has even started), it just does the actual work on
  // the one task that is allowed to touch the sensor.
  //
  // The flag is an omega_control::RecalibrationGate: it stays busy until the
  // loop calls finish() after the sweep, so a second RECALIBRATE that arrives
  // mid-sweep is refused rather than queued.
  espp::omega_control::RecalibrationGate recalibration_gate;

  // D22 rule 1: raised by CalibrationService (Config::output_hold) from an
  // accepted OTA_BEGIN, and before any console-initiated reboot; lowered when
  // an upload ends without one. Written on the dispatcher tasks, read once per
  // iteration by the polling loop, which then sends neutral reports. It counts
  // holds rather than storing a flag because more than one service shares the
  // OTA engine: each reports only its own edges, so one releasing never ends
  // another's hold. No ordering needed, as with recalibration_gate.
  std::atomic<int> output_holds{0};

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
    usb_cfg.serial_number = usb_serial_number();
    printf("USB serial number: %s\n", usb_cfg.serial_number.c_str());
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
    // (espp/usb_device 1.3.8) and dispatcher_worker.hpp (espp/dispatcher
    // 1.3.8), see dependencies.lock. One CalibrationService and
    // one DispatcherWorker per USB byte stream (vendor for WebUSB, CDC for
    // Web Serial). The receive callbacks only push() bytes into the worker:
    // they run on the TinyUSB task, where UsbDevice's write path fails fast
    // instead of waiting for FIFO room, so replying from there could drop
    // frames. The worker parses and dispatches on its own task, and also
    // answers the console's module-0xFF discovery request.
    // --------------------------------------------------------------------
    static espp::UsbDevice *usb_ptr = nullptr; // set once `usb` exists, below
    // One sender per link, shared by the service and its worker. Both USB
    // writes are fail-fast, and the bool tells the service whether a reply
    // was queued.
    auto send_vendor = [](std::span<const uint8_t> frame) {
      return usb_ptr && usb_ptr->write_vendor(frame);
    };
    auto send_cdc = [](std::span<const uint8_t> frame) {
      return usb_ptr && usb_ptr->write_cdc(frame);
    };
    espp::CalibrationService::Config calibration_cfg{
        .store = store,
        .send = send_vendor,
        .start_calibration =
            [&recalibration_gate](std::error_code &start_ec) {
              return recalibration_gate.request(start_ec);
            },
        .finish_calibration =
            [&recalibration_gate]() { return recalibration_gate.request_finish(); },
        .restart = []() { esp_restart(); },
        .ota = ota_engine,
        .diagnostics = make_diagnostics,
        .output_hold =
            [&output_holds](bool hold) {
              if (hold) {
                output_holds.fetch_add(1);
              } else {
                output_holds.fetch_sub(1);
              }
            },
        .log_level = espp::Logger::Verbosity::ERROR,
    };
    espp::CalibrationService calibration_vendor(calibration_cfg);
    calibration_cfg.send = send_cdc;
    espp::CalibrationService calibration_cdc(calibration_cfg);
    espp::DispatcherWorker vendor_link({
        .send = send_vendor,
        .task_config = {.name = "cal_vendor_rx", .stack_size_bytes = 8192},
    });
    vendor_link.register_module(calibration_vendor);
    vendor_link.serve_discovery("omega-stick", esp_app_get_description()->version);
    espp::DispatcherWorker cdc_link({
        .send = send_cdc,
        .task_config = {.name = "cal_cdc_rx", .stack_size_bytes = 8192},
    });
    cdc_link.register_module(calibration_cdc);
    cdc_link.serve_discovery("omega-stick", esp_app_get_description()->version);

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
      reject_if_pending("USB init failed");
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
                   sensor_conversion_count, sensor_status, needs_calibration, recalibration_gate);
    espp::omega_control::MouseState state;
    state.common.center = center;
    state.common.range = range;
    espp::omega_control::Clock::time_point next_position{}; // SET_POSITION_STREAM pacing

#if CONFIG_OMEGA_STICK_OTA
    espp::FirmwareHealthCheck<> fw_health(ota);
#endif
    while (true) {
#if CONFIG_OMEGA_STICK_OTA
      run_health_check(fw_health, output_holds);
#endif
      // Cheap when idle: a try_lock and a timestamp compare per service.
      calibration_vendor.expire_stale_update(OTA_IDLE_TIMEOUT);
      calibration_cdc.expire_stale_update(OTA_IDLE_TIMEOUT);

      // The recalibration sweep itself runs inside mouse_tick (via
      // EspPorts::run_sweep), on this task, so it never races the sensor reads.
      const bool hold = output_holds.load() > 0;
      // A recalibration requested during an update waits for the hold to end.
      const bool recal = !hold && recalibration_gate.take();
      const auto sample = ports.read_stick();
      stream_position(calibration_vendor, calibration_cdc, sample, store, !needs_calibration.load(),
                      next_position);
      mouse_tick(state,
                 {.stick = sample,
                  .now = espp::omega_control::Clock::now(),
                  .recalibration_requested = recal,
                  .output_hold = hold,
                  .needs_calibration = needs_calibration.load(),
                  .settings = store.settings()},
                 ports);
      if (recal) {
        recalibration_gate.finish(); // the sweep ran inside the tick; accept new requests
      }
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
    usb_cfg.serial_number = usb_serial_number();
    printf("USB serial number: %s\n", usb_cfg.serial_number.c_str());
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
    // One sender per link, shared by the service and its worker. Both USB
    // writes are fail-fast, and the bool tells the service whether a reply
    // was queued.
    auto send_vendor = [](std::span<const uint8_t> frame) {
      return usb_ptr && usb_ptr->write_vendor(frame);
    };
    auto send_cdc = [](std::span<const uint8_t> frame) {
      return usb_ptr && usb_ptr->write_cdc(frame);
    };
    espp::CalibrationService::Config calibration_cfg{
        .store = store,
        .send = send_vendor,
        .start_calibration =
            [&recalibration_gate](std::error_code &start_ec) {
              return recalibration_gate.request(start_ec);
            },
        .finish_calibration =
            [&recalibration_gate]() { return recalibration_gate.request_finish(); },
        .restart = []() { esp_restart(); },
        .ota = ota_engine,
        .diagnostics = make_diagnostics,
        .output_hold =
            [&output_holds](bool hold) {
              if (hold) {
                output_holds.fetch_add(1);
              } else {
                output_holds.fetch_sub(1);
              }
            },
        .log_level = espp::Logger::Verbosity::ERROR,
    };
    espp::CalibrationService calibration_vendor(calibration_cfg);
    calibration_cfg.send = send_cdc;
    espp::CalibrationService calibration_cdc(calibration_cfg);
    espp::DispatcherWorker vendor_link({
        .send = send_vendor,
        .task_config = {.name = "cal_vendor_rx", .stack_size_bytes = 8192},
    });
    vendor_link.register_module(calibration_vendor);
    vendor_link.serve_discovery("omega-stick", esp_app_get_description()->version);
    espp::DispatcherWorker cdc_link({
        .send = send_cdc,
        .task_config = {.name = "cal_cdc_rx", .stack_size_bytes = 8192},
    });
    cdc_link.register_module(calibration_cdc);
    cdc_link.serve_discovery("omega-stick", esp_app_get_description()->version);
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
      reject_if_pending("USB init failed");

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
                   sensor_conversion_count, sensor_status, needs_calibration, recalibration_gate);
    espp::omega_control::XacState state;
    state.common.center = center;
    state.common.range = range;
    espp::omega_control::Clock::time_point next_position{}; // SET_POSITION_STREAM pacing

#if CONFIG_OMEGA_STICK_OTA
    espp::FirmwareHealthCheck<> fw_health(ota);
#endif
    while (true) {
#if CONFIG_OMEGA_STICK_OTA
      run_health_check(fw_health, output_holds);
#endif
      // Cheap when idle: a try_lock and a timestamp compare per service.
      calibration_vendor.expire_stale_update(OTA_IDLE_TIMEOUT);
      calibration_cdc.expire_stale_update(OTA_IDLE_TIMEOUT);

      // The recalibration sweep itself runs inside xac_tick (via
      // EspPorts::run_sweep), on this task, so it never races the sensor reads.
      const bool hold = output_holds.load() > 0;
      // A recalibration requested during an update waits for the hold to end.
      const bool recal = !hold && recalibration_gate.take();
      const auto sample = ports.read_stick();
      stream_position(calibration_vendor, calibration_cdc, sample, store, !needs_calibration.load(),
                      next_position);
      xac_tick(state,
               {.stick = sample,
                .now = espp::omega_control::Clock::now(),
                .recalibration_requested = recal,
                .output_hold = hold,
                .needs_calibration = needs_calibration.load(),
                .settings = store.settings()},
               ports);
      if (recal) {
        recalibration_gate.finish(); // the sweep ran inside the tick; accept new requests
      }
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