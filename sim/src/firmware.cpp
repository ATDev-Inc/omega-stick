// The simulated firmware: run_omega_stick() from main/main.cpp, with the hardware
// replaced (see docs/simulator.md). The structure and the order of operations
// follow main.cpp on purpose; the places where it differs are marked "sim:".
//
// Real code used as is: OmegaStore, CalibrationService + espp::Dispatcher,
// OmegaCalibration, omega_control (mouse_tick / xac_tick, RecalibrationGate),
// FirmwareHealthCheck, ResetLog, espp::Joystick, and the TMAG5273 driver.
//
// Mirrored from main.cpp rather than shared (they sit in main.cpp's static
// scope and depend on esp_system.h): the reset-reason helpers, SensorFaultStatus,
// the EspPorts logic, and the boot sequence.

#include "firmware.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "calibration_service.hpp"
#include "dispatcher.hpp"
#include "file_system.hpp"
#include "firmware_health_check.hpp"
#include "joystick.hpp"
#include "omega_calibration.hpp"
#include "omega_control.hpp"
#include "omega_store.hpp"
#include "ota.hpp"
#include "reset_log.hpp"
#include "tmag5273.hpp"

using namespace std::chrono_literals;

namespace sim {

namespace {

constexpr uint8_t TMAG_ADDRESS = 0x35;
constexpr size_t RESET_LOG_ENTRIES = 16;
constexpr int64_t SLOW_SENSOR_START_NS = 3'000'000'000;
constexpr float MANUAL_OFFSET_X = 0.0f;
constexpr float MANUAL_OFFSET_Y = 0.0f;
const std::string CALIBRATION_FILE_NAME = "omega_calibration.cfg";

using Health = espp::FirmwareHealthCheck<SimSteady>;

// sim: main.cpp keeps the crash-loop handover marker in RTC_NOINIT memory, which a
// software reset keeps and a power-on loses. The simulator restarts in-process, so a
// process-level variable stands in for it; boot() treats a power-on reset as clearing it.
uint32_t crash_loop_handover_marker = 0;

// ---- reset-reason helpers (mirror of main.cpp, keyed on esp_reset_reason_t values) ----

const char *reset_reason_name(int reason) {
  switch (reason) {
  case 1:
    return "power-on";
  case 2:
    return "external pin";
  case 3:
    return "software";
  case 4:
    return "panic";
  case 5:
    return "interrupt watchdog";
  case 6:
    return "task watchdog";
  case 7:
    return "watchdog";
  case 8:
    return "deep sleep";
  case 9:
    return "brownout";
  case 10:
    return "sdio";
  case 11:
    return "usb";
  case 12:
    return "jtag";
  case 13:
    return "efuse error";
  case 14:
    return "power glitch";
  case 15:
    return "cpu lockup";
  default:
    return "unknown";
  }
}

bool reset_reason_is_crash(int reason) {
  return reason == 4 || reason == 5 || reason == 6 || reason == 7 || reason == 15;
}

bool reset_reason_is_abnormal(int reason) {
  return reset_reason_is_crash(reason) || reason == 9 || reason == 13 || reason == 14;
}

// The reset name as the console shows it (mirror of main.cpp).
std::string reset_log_display_name(const espp::ResetLog::Entry &entry) {
  return entry.slot.empty() ? entry.name : entry.name + " (" + entry.slot + ")";
}

// ---- sensor fault status (mirror of main.cpp) ----

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

std::string format(const char *fmt, auto... args) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), fmt, args...);
  return buf;
}

/// sim: the Ota object is never destroyed, because CalibrationService's detached
/// reboot thread (REPLY_FLUSH_DELAY after RESTART/REVERT/OTA_FINISH) holds a pointer to it
/// and may run after a restart has already torn the boot down.
espp::Ota &make_ota() {
  static std::vector<std::unique_ptr<espp::Ota>> keep;
  keep.push_back(std::make_unique<espp::Ota>(espp::Ota::Config{
      .reject_same_version = false, .log_level = espp::Logger::Verbosity::ERROR}));
  return *keep.back();
}

/// Registers the calibration link with the host for as long as it lives.
struct LinkGuard {
  Host &host;
  LinkGuard(Host &h, std::function<void(std::span<const uint8_t>)> feed)
      : host(h) {
    host.set_link(std::move(feed));
  }
  ~LinkGuard() { host.clear_link(); }
};

// Simulated side of espp::omega_control::Ports (EspPorts in main.cpp).
struct SimPorts : espp::omega_control::Ports {
  Host &host;
  espp::TMAG5273 &tmag;
  FakeTmagBus &bus;
  espp::OmegaStore &store;
  espp::Joystick &js;
  espp::CalibrationService &calibration;
  const bool &sensor_read_ok;
  const uint8_t &sensor_conversion_count;
  SensorFaultStatus &sensor_status;
  std::atomic<bool> &needs_calibration;

  SimPorts(Host &host_, espp::TMAG5273 &tmag_, FakeTmagBus &bus_, espp::OmegaStore &store_,
           espp::Joystick &js_, espp::CalibrationService &calibration_, const bool &ok_,
           const uint8_t &count_, SensorFaultStatus &status_, std::atomic<bool> &needs_)
      : host(host_)
      , tmag(tmag_)
      , bus(bus_)
      , store(store_)
      , js(js_)
      , calibration(calibration_)
      , sensor_read_ok(ok_)
      , sensor_conversion_count(count_)
      , sensor_status(status_)
      , needs_calibration(needs_) {}

  void send_mouse(const espp::omega_control::MouseReport &r) override { host.on_mouse(r); }
  void send_xac(const espp::omega_control::XacReport &r) override { host.on_xac(r); }

  bool run_sweep(espp::OmegaCalibration::CenterResult &center,
                 espp::OmegaCalibration::RangeResult &range) override {
    host.console_text("\n=== RE-CALIBRATION REQUESTED (calibration console) ===");
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
        // sim: prompts also go to the page, and the sweep's sleeps advance simulated time.
        .message = [this](const std::string &text) { host.calibration_prompt(text); },
        .sleep = [this](std::chrono::milliseconds d) { host.sleep(d); },
        .verbosity = espp::Logger::Verbosity::ERROR,
    });
    bool recal_ok = recal.center_calibration(new_center, recal_ec) &&
                    recal.range_calibration(new_center, new_range, recal_ec);
    if (recal_ok) {
      recal.finalize_center_deadzone(new_center, new_range);
      range = new_range;
      store.set_measured_calibration(new_center, range);
      center = store.center();
      std::error_code save_ec;
      if (!store.save_to_flash(save_ec)) {
        host.console_text("WARNING: recalibration succeeded but saving to flash failed: " +
                          save_ec.message());
      }
      host.console_text("=== RE-CALIBRATION COMPLETE ===\n");
      if (needs_calibration.exchange(false)) {
        host.console_text("Calibration is usable; output resumes.");
      }
      host.live.center_x = center.center_x;
      host.live.center_y = center.center_y;
      host.live.have_calibration = true;
    } else {
      host.console_text("ERROR: re-calibration failed: " + recal_ec.message());
      if (needs_calibration.load()) {
        host.console_text("Output stays neutral until a calibration succeeds.");
      }
    }
    host.live.last_sweep = recal_ok ? 1 : 0;
    calibration.notify_calibration_result(recal_ok, recal_ok ? std::error_code{} : recal_ec);
    return recal_ok;
  }

  void save_to_flash() override {
    std::error_code save_ec;
    if (!store.save_to_flash(save_ec)) {
      host.console_text("WARNING: Failed to persist drift-corrected calibration: " +
                        save_ec.message());
    }
  }

  void apply_drift(espp::OmegaCalibration::CenterResult &center,
                   const espp::OmegaCalibration::RangeResult &range) override {
    store.set_measured_calibration(center, range);
    center = store.center();
    espp::OmegaCalibration::apply_calibration(js, center, range, store.settings());
  }

  void refresh_manual_offset(espp::OmegaCalibration::CenterResult &center) override {
    const auto live = store.center();
    center.manual_offset_x = live.manual_offset_x;
    center.manual_offset_y = live.manual_offset_y;
  }

  espp::omega_control::StickSample read_stick() override {
    js.update();
    return {js.x(), js.y(), js.raw().x(), js.raw().y(), sensor_read_ok, sensor_conversion_count};
  }

  void sensor_fault_changed(bool faulted, espp::OmegaCalibration::SensorFaultMonitor::Fault fault,
                            std::uint32_t fault_count) override {
    sensor_status.publish(faulted, fault, fault_count);
    host.live.sensor_fault = faulted;
    if (faulted) {
      host.console_text(format("WARNING: sensor fault (%s); holding the stick centered",
                               espp::OmegaCalibration::SensorFaultMonitor::to_string(fault)));
    } else {
      host.console_text(
          format("Sensor recovered (fault %u since boot)", static_cast<unsigned>(fault_count)));
    }
  }
  void sensor_observed_healthy() override {
    sensor_status.observed_healthy();
    host.live.sensor_observed = true;
  }

  espp::omega_control::Clock::time_point now() override { return steady_at(host.now_ns()); }
};

std::string boot(Host &host, int reset_reason) {
  host.live.have_calibration = false;
  host.live.last_sweep = -1;

  // ==========================================================================
  // Reset-reason log (D23)
  // ==========================================================================
  const std::string running_slot_label = esp_ota_get_running_partition()->label;
  std::vector<Health::BootRecord> boots; // oldest first
  espp::ResetLog reset_log(
      {.path = espp::ResetLog::default_path(), .max_entries = RESET_LOG_ENTRIES});
  {
    std::error_code log_ec;
    if (!reset_log.load(log_ec)) {
      host.console_text("WARNING: could not read the reset log: " + log_ec.message());
    }
    reset_log.record(reset_reason, reset_reason_name(reset_reason),
                     reset_reason_is_abnormal(reset_reason), running_slot_label);
    for (const auto &entry : reset_log.entries()) {
      boots.push_back({.slot = entry.slot, .crash = reset_reason_is_crash(entry.reason)});
    }
    if (!reset_log.save(log_ec)) {
      host.console_text("WARNING: could not save the reset log: " + log_ec.message());
    }
    host.console_text(format("Boot %u, reset reason: %s%s",
                             static_cast<unsigned>(reset_log.boot_count()),
                             reset_reason_name(reset_reason),
                             reset_reason_is_abnormal(reset_reason) ? " (abnormal)" : ""));
  }

  // --no-ota mirrors a CONFIG_OMEGA_STICK_OTA=n build (D25): no OTA engine, no health
  // check, no crash-loop revert.
  const bool no_ota = host.options().no_ota;
  espp::Ota *const ota = no_ota ? nullptr : &make_ota();
  auto reject_if_pending = [ota](const char *reason) {
    if (ota) {
      Health::reject_if_pending(*ota, reason);
    }
  };
  // cppcheck-suppress knownConditionTrueFalse ; null under --no-ota, set in another file
  if (!ota) {
    host.console_text("Firmware updates over USB are off in this build; install new firmware from "
                      "the first-flash page.");
    // An image that arrived by update boots pending verification and nothing here can
    // confirm it, so it accepts itself before anything else runs (as main.cpp does).
    if (sim::flash().pending_verify()) {
      sim::flash().mark_valid();
      host.console_text("This image arrived by update and cannot be confirmed by the console, so "
                        "it accepted itself.");
    }
  }

  // A confirmed image that has crashed on its last few boots hands the device to
  // the other slot (D18 amendment), at most once per power-on.
  const auto handover =
      Health::take_handover_marker(crash_loop_handover_marker, reset_reason == kRstPoweron);
  // cppcheck-suppress knownConditionTrueFalse ; null under --no-ota, set in another file
  if (ota && !handover.handed_over_since_power_on &&
      Health::revert_if_crash_looping(*ota,
                                      Health::crashes_while_running(boots, running_slot_label))) {
    crash_loop_handover_marker = Health::HANDOVER_REQUESTED;
    sim::flash().take_restart_reason();
    return "revert";
  }
  // The image the handover booted is pending, and with no console attached the deadline
  // would send the device back into the crash loop: it ran before, so accept it now.
  // cppcheck-suppress knownConditionTrueFalse ; null under --no-ota, set in another file
  if (ota) {
    Health::accept_after_handover(*ota, handover.booted_by_handover);
  }
  {
    const esp_app_desc_t *app = esp_app_get_description();
    const char *pending = ota && sim::flash().pending_verify() ? " [PENDING VERIFY]" : "";
    host.console_text(format("Firmware %s %s (built %s %s, idf %s) running from %s%s",
                             app->project_name, app->version, app->date, app->time, app->idf_ver,
                             running_slot_label.c_str(), pending));
  }
  // sim: an image flashed with next_image=crash_loop crashes right after the reset log
  // records the boot, before the application starts.
  if (sim::flash().running_behavior() == NextImage::CRASH_LOOP) {
    host.console_text("PANIC: simulated crash on boot (next_image crash_loop)");
    return "crash";
  }
  // sim: next_image=crash_when_confirmed runs normally until it has been confirmed, then
  // crashes the same way on every boot.
  if (sim::flash().running_behavior() == NextImage::CRASH_WHEN_CONFIRMED &&
      !sim::flash().pending_verify()) {
    host.console_text("PANIC: simulated crash on boot (next_image crash_when_confirmed)");
    return "crash";
  }
  const NextImage behavior = sim::flash().running_behavior();
  const bool self_test_fails =
      behavior == NextImage::FAIL_SELF_TEST || behavior == NextImage::SLOW_SENSOR_FAIL;

  SensorFaultStatus sensor_status;
  host.live.sensor_fault = false;
  host.live.sensor_observed = false;
  std::atomic<bool> needs_calibration{false}; // D26, as in main.cpp
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
  // I2C bus and TMAG5273 (sim: a fake bus behind the real driver)
  // ==========================================================================
  std::error_code ec;
  FakeTmagBus bus(
      host.sensor, [&host] { return host.stick_now(); },
      [&host](const Field &f) {
        host.live.bx = f.bx;
        host.live.by = f.by;
      });
  espp::TMAG5273 tmag(bus.config(TMAG_ADDRESS));

  host.console_text("Initializing TMAG5273...");
  if (!tmag.initialize(ec)) {
    host.console_text("ERROR: TMAG5273 initialization failed: " + ec.message());
    reject_if_pending("TMAG5273 init failed");
    while (!host.should_exit()) { // halts like the firmware; a restart ends it
      host.sleep(1s);
    }
    return "";
  }
  host.console_text("TMAG5273 initialized successfully!\n");

  // ==========================================================================
  // Calibration + live settings store
  // ==========================================================================
  const std::filesystem::path calibration_path =
      espp::OmegaCalibration::default_path(CALIBRATION_FILE_NAME);
  espp::Joystick *live_joystick = nullptr;
  bool starting = true; // sim: --mode may set the mode on a board that has none saved yet

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
          [&store, &host, &starting](espp::OmegaStore::InputMode mode) {
            if (starting) {
              return;
            }
            host.console_text(format("Input mode changed to %s; saving settings and restarting to "
                                     "apply...",
                                     mode == espp::OmegaStore::InputMode::XAC ? "XAC" : "MOUSE"));
            std::error_code save_ec;
            if (!store.save_to_flash(save_ec)) {
              host.console_text("WARNING: failed to save settings before restart: " +
                                save_ec.message());
            }
            // As main.cpp: the OK was queued just before this runs; give it the same time to
            // reach the wire as the other console reboots (real time, like theirs).
            std::this_thread::sleep_for(espp::CalibrationService::REPLY_FLUSH_DELAY);
            host.request_restart("mode switch");
          },
      .log_level = espp::Logger::Verbosity::ERROR,
  });
  if (host.options().mode &&
      !std::filesystem::exists(espp::OmegaCalibration::DeviceSettings::default_path())) {
    store.set_input_mode(*host.options().mode == "xac" ? espp::OmegaStore::InputMode::XAC
                                                       : espp::OmegaStore::InputMode::MOUSE);
  }
  starting = false;

  espp::OmegaCalibration::CenterResult center;
  espp::OmegaCalibration::RangeResult range;
  std::error_code cal_ec;

  // D26, as in main.cpp: without a usable calibration the board boots quiet and
  // waits for a calibration from the console. No sweep runs at boot.
  if (espp::OmegaCalibration::load(center, range, calibration_path, cal_ec)) {
    host.console_text("Loaded existing calibration.");
  } else {
    needs_calibration.store(true);
    if (cal_ec != espp::OmegaCalibration::make_error_code(
                      espp::OmegaCalibration::Error::calibration_unusable)) {
      center = {};
      range = {};
      espp::OmegaCalibration::set_manual_offset(center, MANUAL_OFFSET_X, MANUAL_OFFSET_Y);
    }
    host.console_text(format("No usable calibration (%s).", cal_ec.message().c_str()));
    host.console_text("Output is held neutral until a calibration succeeds: connect the "
                      "calibration console and use \"Re-run calibration\".");
  }

  store.set_calibration(center, range);
  host.live.have_calibration = !needs_calibration.load();
  host.live.center_x = center.center_x;
  host.live.center_y = center.center_y;

  // Recalibration hand-off and output holds (D22 rule 1), as in main.cpp.
  espp::omega_control::RecalibrationGate recalibration_gate;
  std::atomic<int> output_holds{0};

  // ==========================================================================
  // Calibration link
  // ==========================================================================
  // sim: one link and one service stand in for main.cpp's vendor and CDC pairs.
  auto transmit = [&host](std::span<const uint8_t> frame) { return host.transmit(frame); };
  espp::CalibrationService::Config calibration_cfg{
      .store = store,
      .send = transmit,
      .start_calibration =
          [&recalibration_gate](std::error_code &start_ec) {
            return recalibration_gate.request(start_ec);
          },
      .restart = [&host]() { host.request_restart("restart"); },
      .ota = ota,
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
  espp::CalibrationService calibration(calibration_cfg);
  espp::Dispatcher dispatcher; // sim: DispatcherWorker needs espp::Task; same Dispatcher inside
  dispatcher.register_module(calibration);
  dispatcher.set_device_info("omega-stick", esp_app_get_description()->version);
  dispatcher.serve_discovery(transmit);
  LinkGuard link(host, [&dispatcher](std::span<const uint8_t> data) { dispatcher.feed(data); });

  const bool xac_selection = store.settings().input_mode == espp::OmegaStore::InputMode::XAC;
  host.live.mode = xac_selection ? 1 : 0;
  host.console_text(format(
      "CALIBRATION RESULTS: center %.4f, %.4f mT; usable radius %.4f mT; mode %s", center.center_x,
      center.center_y, range.usable_radius, xac_selection ? "XAC" : "MOUSE"));

  // ==========================================================================
  // Joystick and polling loop (the mouse and XAC branches of main.cpp, merged)
  // ==========================================================================
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

  SimPorts ports(host, tmag, bus, store, js, calibration, sensor_read_ok, sensor_conversion_count,
                 sensor_status, needs_calibration);
  espp::omega_control::MouseState mouse_state;
  mouse_state.common.center = center;
  mouse_state.common.range = range;
  espp::omega_control::XacState xac_state;
  xac_state.common.center = center;
  xac_state.common.range = range;

  std::optional<Health> fw_health;
  if (ota) {
    fw_health.emplace(*ota);
  }
  // sim: an image flashed with next_image=fail_self_test boots and calibrates, then its sensor
  // stops answering, so SensorFaultMonitor trips and the console sees sensor_fault in
  // GET_DIAGNOSTICS and refuses to confirm. The fault is lifted again when this boot ends.
  struct SelfTestFault {
    sim::SensorModel &model;
    sim::Fault previous;
    bool active;
    SelfTestFault(sim::SensorModel &m, bool on)
        : model(m)
        , previous(m.fault())
        , active(on) {
      if (active) {
        model.set_fault(sim::Fault::NoResponse);
      }
    }
    ~SelfTestFault() {
      if (active) {
        model.set_fault(previous);
      }
    }
  } self_test_fault{host.sensor, self_test_fails};
  // Declared last, so it runs first on the way out: the page must not reach the
  // service while the joystick it re-applies calibration to is already gone.
  struct Unlink {
    Host &h;
    ~Unlink() { h.clear_link(); }
  } unlink{host};
  // sim: slow_sensor_start and slow_sensor_fail begin polling only after a delay, as the board
  // does while it waits for HID readiness. The console is already served meanwhile, so
  // GET_DIAGNOSTICS must say "not observed", not "healthy".
  if (behavior == NextImage::SLOW_SENSOR_START || behavior == NextImage::SLOW_SENSOR_FAIL) {
    host.console_text("sim: sensor polling starts late (next_image slow_sensor_*)");
    const int64_t start_ns = host.now_ns();
    while (!host.should_exit() && !host.restart_requested() &&
           host.now_ns() - start_ns < SLOW_SENSOR_START_NS) {
      host.sleep(10ms);
    }
  }
  while (!host.should_exit()) {
    if (host.restart_requested()) {
      return host.take_restart_reason();
    }
    if (fw_health) {
      // As main.cpp: past the deadline, neutral output first, then the rollback.
      switch (fw_health->tick()) {
      case Health::Action::HOLD_OUTPUT:
        output_holds.fetch_add(1);
        break;
      case Health::Action::ROLL_BACK:
        fw_health->rollback_now(); // sim: throws RestartRequest unless the rollback failed
        output_holds.fetch_sub(1);
        break;
      case Health::Action::NONE:
        break;
      }
    }
    calibration.expire_stale_update(espp::CalibrationService::OTA_IDLE_TIMEOUT);

    const bool hold = output_holds.load() > 0;
    const bool recal = !hold && recalibration_gate.take();
    const auto sample = ports.read_stick();
    const espp::omega_control::StepInput input{.stick = sample,
                                               .now = steady_at(host.now_ns()),
                                               .recalibration_requested = recal,
                                               .output_hold = hold,
                                               .needs_calibration = needs_calibration.load(),
                                               .settings = store.settings()};
    if (xac_selection) {
      xac_tick(xac_state, input, ports);
    } else {
      mouse_tick(mouse_state, input, ports);
    }
    if (recal) {
      recalibration_gate.finish();
    }
    host.sleep(xac_selection ? 5ms : 10ms);
  }
  return "";
}

} // namespace

std::string run_firmware(Host &host, int reset_reason) {
  try {
    return boot(host, reset_reason);
  } catch (const RestartRequest &r) {
    if (host.restart_requested()) {
      host.take_restart_reason();
    }
    return r.reason;
  }
}

} // namespace sim
