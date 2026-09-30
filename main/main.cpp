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

#include "esp_system.h" // esp_restart() -- used to apply a live mode switch (see on_mode_changed below)

#include "calibration_service.hpp"
#include "file_system.hpp"
#include "hid-rp-mouse.hpp"
#include "hid-rp.hpp"
#include "i2c.hpp"
#include "joystick.hpp"
#include "logger.hpp"
#include "omega_calibration.hpp"
#include "omega_store.hpp"
#include "qtpy.hpp"
#include "task.hpp"
#include "tmag5273.hpp"
#include "usb_device.hpp"
#include "xac.hpp"

using namespace std::chrono_literals;

static constexpr uint8_t TMAG_ADDRESS = 0x35;

// Scales the normalized joystick output (-1..1) into mouse movement counts
// per polling interval. Lower this to slow the cursor down, raise it to
// speed the cursor up.
static constexpr float SENSITIVITY = 15.0f;

// Manual per-axis trim, added on top of the measured center, ONLY on a
// fresh (never-before-calibrated) run -- see the calibration section below.
// Once calibrated, the manual offset lives in OmegaStore/DeviceSettings and
// is live-adjustable from the calibration console instead.
static constexpr float MANUAL_OFFSET_X = 0.0f;
static constexpr float MANUAL_OFFSET_Y = 0.0f;

// ============================================================================
// Nudge click
// ============================================================================
//
//     left    -> scroll up   (held: repeated wheel-up ticks)
//     right   -> right click (held: right button down)
//     forward -> left click  (held: left button down)
//     back    -> scroll down (held: repeated wheel-down ticks)
//
// "Forward" is taken to mean pushing the stick away from the user, which is
// the same direction that already drives the cursor upward on screen, i.e.
// negative js.y(); "back" (pulling the stick toward the user) is positive
// js.y(). If a given build's mechanical/sensor orientation has the Y axis
// flipped, set INVERT_NUDGE_Y to true rather than touching the logic below.
static constexpr bool INVERT_NUDGE_Y = false;

// The enter threshold (delta), exit threshold (absolute), and measurement
// time window used to previously live here as NUDGE_ENTER_THRESHOLD /
// NUDGE_EXIT_THRESHOLD / NUDGE_TIME. They now live in
// espp::OmegaCalibration::DeviceSettings (nudge_distance_threshold /
// nudge_exit_threshold / nudge_hold_time) so the calibration console can
// tune them live -- see OmegaStore::settings() below. Their defaults there
// match what was hardcoded here before (0.5 / 0.8 / 100ms).

// The polling loop below runs every 10ms. Emitting a wheel tick on every
// single poll while a scroll nudge is held would scroll far too fast, so a
// tick is only sent every Nth poll instead.
static constexpr int SCROLL_POLL_DIVIDER = 5;
static constexpr std::int8_t SCROLL_STEP = 1;

enum class NudgeDirection { NONE, LEFT, RIGHT, FORWARD, BACK };

// Classifies a normalized (x, y) stick position into one of the four cardinal
// nudge directions. Only called once the stick's magnitude has already
// crossed the enter threshold, so this only needs to pick a direction, not
// decide whether one is active.
static NudgeDirection classify_nudge(float x, float y) {
  const float effective_y = INVERT_NUDGE_Y ? -y : y;
  if (std::abs(x) >= std::abs(effective_y)) {
    return (x >= 0.0f) ? NudgeDirection::RIGHT : NudgeDirection::LEFT;
  } else {
    return (effective_y < 0.0f) ? NudgeDirection::FORWARD : NudgeDirection::BACK;
  }
}

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

// Runs entirely on the dedicated "omega-stick" task (see app_main below),
// not on the small default main-task stack. Matches espp::Task's callback
// signature; m/cv are unused since this function never yields back to the
// task's own wait/dispatch loop -- it does its one-time setup and then
// never returns, spending the rest of its life in the polling loop at the
// bottom.

static bool run_omega_stick(std::mutex & /*m*/, std::condition_variable & /*cv*/) {
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
            // Give the calibration console's OK reply a moment to actually
            // go out over USB before the reset tears the link down.
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

  // cppcheck-suppress knownConditionTrueFalse
  if (!XAC_SELECTION) { // TODO

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
    espp::UsbDevice::CdcFunction cdc_fn;
    cdc_fn.route_console = true;
    usb_cfg.cdc = cdc_fn;

    espp::UsbDevice::HidFunction hid_fn;
    hid_fn.interface_name = "omega-stick HID";
    hid_fn.report_descriptor = hid_report_descriptor;
    hid_fn.poll_interval_ms = 10;
    usb_cfg.hid = hid_fn;

    // --------------------------------------------------------------------
    // Calibration console wiring (module 5) -- BEST-EFFORT / UNVERIFIED.
    // We don't have usb_device.hpp in this session, so the exact field and
    // method names below (VendorFunction, .on_receive, write_vendor(),
    // write_cdc()) are inferred from coredump_service.hpp's own header
    // comment ("send = usb.write_vendor(frame), and call feed(data) from
    // the vendor receive callback" / the equivalent CDC line). Confirm
    // these against your actual espp::UsbDevice API before relying on it;
    // everything else in this file does not depend on this section.
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
        .log_level = espp::Logger::Verbosity::ERROR,
    };
    espp::CalibrationService calibration_vendor(calibration_cfg);
    calibration_cfg.send = [](std::span<const uint8_t> frame) {
      if (usb_ptr)
        usb_ptr->write_cdc(frame);
    };
    espp::CalibrationService calibration_cdc(calibration_cfg);

    espp::UsbDevice::VendorFunction vendor_fn;
    vendor_fn.interface_name = "omega-stick vendor";
    vendor_fn.on_receive = [&calibration_vendor](std::span<const uint8_t> data) {
      calibration_vendor.feed(data);
    };
    usb_cfg.vendor = vendor_fn;
    cdc_fn.on_receive = [&calibration_cdc](std::span<const uint8_t> data) {
      calibration_cdc.feed(data);
    };
    usb_cfg.cdc = cdc_fn; // re-assign now that on_receive is set

    espp::UsbDevice usb(usb_cfg);
    usb_ptr = &usb;

    std::error_code usb_ec;
    if (!usb.initialize(usb_ec)) {
      printf("ERROR: USB init failed: %s\n", usb_ec.message().c_str());
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

    espp::MouseInputReport<0, 3, true> mouse;
    mouse.reset();

    espp::Joystick js(espp::OmegaCalibration::make_joystick_config(
        center, range, store.settings(), [&tmag](float *x, float *y) {
          std::error_code read_ec;
          auto magnetic = tmag.read_magnetic(read_ec);
          if (read_ec) {
            return false;
          }
          *x = magnetic.x;
          *y = magnetic.y;
          return true;
        }));
    live_joystick = &js; // let on_calibration_changed re-apply live edits

    std::this_thread::sleep_for(1s);

    // Corrects slow center drift while the device runs (see
    // DriftCompensator's docs in omega_calibration.hpp). Persisting the
    // drift-corrected calibration on every single nudge would wear the flash
    // for no real benefit, so it's only saved every SAVE_EVERY_N_NUDGES.
    espp::OmegaCalibration::DriftCompensator drift_compensator;
    int drift_nudge_count = 0;
    static constexpr int SAVE_EVERY_N_NUDGES = 20;

    // Current nudge-click state (see the "Nudge click" section above). This
    // persists across polling iterations/loops so a held nudge stays held.
    NudgeDirection current_nudge = NudgeDirection::NONE;
    int scroll_poll_count = 0;

    auto start = std::chrono::high_resolution_clock::now();
    js.update();
    float prev_x = js.x();
    float prev_y = js.y();
    while (true) {
      // --- Recalibration-on-demand (see the section above) ---
      if (recalibration_requested.exchange(false)) {
        printf("\n=== RE-CALIBRATION REQUESTED (calibration console) ===\n");
        const float old_manual_offset_x = center.manual_offset_x;
        const float old_manual_offset_y = center.manual_offset_y;
        espp::OmegaCalibration::CenterResult new_center;
        espp::OmegaCalibration::RangeResult new_range;
        std::error_code recal_ec;
        espp::OmegaCalibration recal({
            .read =
                [&tmag](float *x, float *y) {
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
          // Preserve whatever manual offset was already live -- a fresh
          // center_calibration() starts it at zero, and recalibration is
          // only supposed to redo the center/range sweep, not discard a
          // previously-tuned trim (deadzone overrides, nudge tuning and
          // input mode already live outside CenterResult/RangeResult, in
          // DeviceSettings, so they're untouched either way).
          espp::OmegaCalibration::set_manual_offset(new_center, old_manual_offset_x,
                                                    old_manual_offset_y);
          recal.finalize_center_deadzone(new_center, new_range);
          center = new_center;
          range = new_range;
          store.set_calibration(center, range); // fires on_calibration_changed -> re-applies live
          std::error_code save_ec;
          if (!store.save_to_flash(save_ec)) {
            printf("WARNING: recalibration succeeded but saving to flash failed: %s\n",
                   save_ec.message().c_str());
          }
          printf("=== RE-CALIBRATION COMPLETE ===\n\n");
        } else {
          printf("ERROR: re-calibration failed: %s\n", recal_ec.message().c_str());
        }
        calibration_vendor.notify_calibration_result(recal_ok,
                                                     recal_ok ? std::error_code{} : recal_ec);
        calibration_cdc.notify_calibration_result(recal_ok,
                                                  recal_ok ? std::error_code{} : recal_ec);
        // The sweep just consumed a lot of wall-clock time and moved the
        // stick all over the place; reset the nudge/timing bookkeeping so
        // the very next iteration doesn't see a bogus giant delta.
        current_nudge = NudgeDirection::NONE;
        scroll_poll_count = 0;
        start = std::chrono::high_resolution_clock::now();
        js.update();
        prev_x = js.x();
        prev_y = js.y();
        continue;
      }

      js.update();
      float raw_x = js.raw().x();
      float raw_y = js.raw().y();
      float jx = js.x();
      float jy = js.y();

      auto end = std::chrono::high_resolution_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
      const float cur_mag = std::hypot(jx, jy);

      // Nudge tuning is read live each measurement window, so a console
      // edit (SET_NUDGE_SETTINGS) takes effect on the very next window
      // without needing a restart.
      const auto nudge_settings = store.settings();

      // --- Nudge direction detection only (runs every nudge_hold_time window) ---
      if (duration >= nudge_settings.nudge_hold_time) {
        const float prev_mag = std::hypot(prev_x, prev_y);
        const float mapped_magnitude = cur_mag - prev_mag;

        if (current_nudge == NudgeDirection::NONE) {
          if (mapped_magnitude >= nudge_settings.nudge_distance_threshold) {
            current_nudge = classify_nudge(jx, jy);
            scroll_poll_count = 0;
          }
        } else if (cur_mag <= nudge_settings.nudge_exit_threshold) {
          current_nudge = NudgeDirection::NONE;
        }

        prev_x = jx;
        prev_y = jy;
        start = end;
      }

      // --- Per-report mouse update: runs every 10ms iteration ---
      mouse.reset_movement();
      mouse.set_button(1, current_nudge == NudgeDirection::FORWARD);
      mouse.set_button(2, current_nudge == NudgeDirection::RIGHT);

      switch (current_nudge) {
      case NudgeDirection::NONE:
        mouse.set_movement(jx * SENSITIVITY, jy * SENSITIVITY);
        break;
      case NudgeDirection::LEFT:
        scroll_poll_count++;
        if (scroll_poll_count % SCROLL_POLL_DIVIDER == 0)
          mouse.set_wheel(SCROLL_STEP);
        break;
      case NudgeDirection::BACK:
        scroll_poll_count++;
        if (scroll_poll_count % SCROLL_POLL_DIVIDER == 0)
          mouse.set_wheel(-SCROLL_STEP);
        break;
      case NudgeDirection::FORWARD:
      case NudgeDirection::RIGHT:
        break;
      }

      if (drift_compensator.update(raw_x, raw_y, cur_mag, center)) {
        store.set_calibration(center, range); // keeps the store (and console) in sync
        espp::OmegaCalibration::apply_calibration(js, center, range, store.settings());
        drift_nudge_count++;
        if (drift_nudge_count % SAVE_EVERY_N_NUDGES == 0) {
          std::error_code save_ec;
          if (!store.save_to_flash(save_ec)) {
            printf("WARNING: Failed to persist drift-corrected calibration: %s\n",
                   save_ec.message().c_str());
          }
        }
      }

      auto report = mouse.get_report();
      std::error_code hid_ec;
      if (!usb.write_hid_report(0, report, hid_ec)) {
        printf("HID send failed: %s\n", hid_ec.message().c_str());
      }

      mouse.reset_movement();
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

    espp::UsbDevice::CdcFunction cdc_fn;
    cdc_fn.route_console = true;
    usb_cfg.cdc = cdc_fn;

    espp::UsbDevice::HidFunction hid_fn;
    hid_fn.interface_name = "omega-stick XAC";
    hid_fn.report_descriptor = hid_report_descriptor;
    hid_fn.poll_interval_ms = 10;

    usb_cfg.hid = hid_fn;

    // Same best-effort calibration-console wiring as the mouse branch above
    // (see the comment there) -- kept here too so the console still works
    // when the stick boots in XAC mode.
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
        .log_level = espp::Logger::Verbosity::ERROR,
    };
    espp::CalibrationService calibration_vendor(calibration_cfg);
    calibration_cfg.send = [](std::span<const uint8_t> frame) {
      if (usb_ptr)
        usb_ptr->write_cdc(frame);
    };
    espp::CalibrationService calibration_cdc(calibration_cfg);
    espp::UsbDevice::VendorFunction vendor_fn;
    vendor_fn.interface_name = "omega-stick vendor";
    vendor_fn.on_receive = [&calibration_vendor](std::span<const uint8_t> data) {
      calibration_vendor.feed(data);
    };
    usb_cfg.vendor = vendor_fn;
    cdc_fn.on_receive = [&calibration_cdc](std::span<const uint8_t> data) {
      calibration_cdc.feed(data);
    };
    usb_cfg.cdc = cdc_fn;

    espp::UsbDevice usb(usb_cfg);
    usb_ptr = &usb;

    std::error_code usb_ec;

    if (!usb.initialize(usb_ec)) {
      printf("ERROR: USB init failed: %s\n", usb_ec.message().c_str());

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
    // XAC input report
    // ------------------------------------------------------------

    XacInput xac_input_report;
    xac_input_report.reset();

    // ------------------------------------------------------------
    // Joystick
    // ------------------------------------------------------------

    espp::Joystick js(espp::OmegaCalibration::make_joystick_config(
        center, range, store.settings(), [&tmag](float *x, float *y) {
          std::error_code read_ec;

          auto magnetic = tmag.read_magnetic(read_ec);

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
    // Drift compensation
    // ------------------------------------------------------------

    espp::OmegaCalibration::DriftCompensator drift_compensator;

    int drift_nudge_count = 0;

    static constexpr int SAVE_EVERY_N_NUDGES = 20;

    // ------------------------------------------------------------
    // HID polling loop
    // ------------------------------------------------------------

    while (true) {
      // --- Recalibration-on-demand (see the section above the mouse loop) ---
      if (recalibration_requested.exchange(false)) {
        printf("\n=== RE-CALIBRATION REQUESTED (calibration console) ===\n");
        const float old_manual_offset_x = center.manual_offset_x;
        const float old_manual_offset_y = center.manual_offset_y;
        espp::OmegaCalibration::CenterResult new_center;
        espp::OmegaCalibration::RangeResult new_range;
        std::error_code recal_ec;
        espp::OmegaCalibration recal({
            .read =
                [&tmag](float *x, float *y) {
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
          espp::OmegaCalibration::set_manual_offset(new_center, old_manual_offset_x,
                                                    old_manual_offset_y);
          recal.finalize_center_deadzone(new_center, new_range);
          center = new_center;
          range = new_range;
          store.set_calibration(center, range);
          std::error_code save_ec;
          if (!store.save_to_flash(save_ec)) {
            printf("WARNING: recalibration succeeded but saving to flash failed: %s\n",
                   save_ec.message().c_str());
          }
          printf("=== RE-CALIBRATION COMPLETE ===\n\n");
        } else {
          printf("ERROR: re-calibration failed: %s\n", recal_ec.message().c_str());
        }
        calibration_vendor.notify_calibration_result(recal_ok,
                                                     recal_ok ? std::error_code{} : recal_ec);
        calibration_cdc.notify_calibration_result(recal_ok,
                                                  recal_ok ? std::error_code{} : recal_ec);
        continue;
      }

      js.update();

      const float raw_x = js.raw().x();
      const float raw_y = js.raw().y();
      const float mapped_magnitude = std::hypot(js.x(), js.y());

      if (drift_compensator.update(raw_x, raw_y, mapped_magnitude, center)) {

        store.set_calibration(center, range);
        espp::OmegaCalibration::apply_calibration(js, center, range, store.settings());

        drift_nudge_count++;

        if (drift_nudge_count % SAVE_EVERY_N_NUDGES == 0) {
          std::error_code save_ec;

          if (!store.save_to_flash(save_ec)) {

            printf("WARNING: Failed to persist "
                   "drift-corrected calibration: %s\n",
                   save_ec.message().c_str());
          }
        }
      }

      // IMPORTANT:
      // Gamepad/XAC axes are already normalized [-1, +1].
      xac_input_report.set_joystick(js.x(), js.y());
      auto report = xac_input_report.get_report();

      if (!usb.write_hid_report(1, report, usb_ec)) {

        printf("HID send failed: %s\n", usb_ec.message().c_str());
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