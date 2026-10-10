#pragma once

// ============================================================================
// OTA rollback health check
// ============================================================================
//
// With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE (sdkconfig.defaults) a freshly
// installed image boots in the PENDING_VERIFY state, and the bootloader
// reverts to the previous image on the next reset unless the app confirms
// itself. Nothing confirms it automatically (espp::Ota deliberately leaves
// that to the application), so this is where it happens.
//
// The new image is confirmed only once it has demonstrably done its job: USB
// has enumerated, the magnetic sensor answers, and the polling loop has been
// running for HEALTHY_UPTIME. If it cannot manage that within
// ROLLBACK_DEADLINE it is marked invalid and the device reboots into the
// previous image on purpose, instead of limping along until a power cycle
// happens to trigger the rollback.
//
// Three related rules:
//  - A fatal init failure (sensor, calibration, USB) on a pending image calls
//    reject_if_pending() so it rolls back immediately instead of halting on a
//    broken image forever.
//  - The calibration console's MARK_VALID button is a manual override. The
//    pending state is re-read on every check, so once someone confirms the
//    image the automatic check stands down and never rolls it back.
//  - A CONFIRMED image that crashes on CRASH_LOOP_RESETS boots in a row hands
//    the device to the other slot (revert_if_crash_looping(), fed from the
//    reset log at boot). The bootloader only protects a pending image, so
//    without this a crash loop after confirmation would run until someone
//    re-flashed over serial.
//
// Consequence worth knowing: any reset (mode switch, unplug) during the first
// HEALTHY_UPTIME after an update reverts to the previous firmware. The
// console shows the image as "pending verify" until it is confirmed.

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <system_error>

#include "esp_ota_ops.h"
#include "ota.hpp"

namespace espp {

/// The clock is injectable so host tests can advance time by hand.
template <typename Clock = std::chrono::steady_clock> class FirmwareHealthCheck {
public:
  static constexpr std::chrono::seconds HEALTHY_UPTIME{10};
  static constexpr std::chrono::seconds ROLLBACK_DEADLINE{90};
  static constexpr std::chrono::seconds CHECK_INTERVAL{1};
  /// Consecutive crashing boots before the running image is given up on.
  /// Three keeps one panic, which a core dump already records, from
  /// switching firmware on its own.
  static constexpr size_t CRASH_LOOP_RESETS{3};

  explicit FirmwareHealthCheck(espp::Ota &ota)
      : ota_(ota)
      , pending_(ota.is_pending_verify())
      , start_(Clock::now())
      , last_check_(start_) {
    if (pending_) {
      printf("OTA: running image is PENDING VERIFY; confirming after %llds of healthy operation\n",
             static_cast<long long>(HEALTHY_UPTIME.count()));
    }
  }

  /// For a fatal init failure: if the running image is still pending, mark it
  /// invalid and reboot into the previous one. Returns only if the image was
  /// not pending or the rollback itself failed; the caller then halts as before.
  template <typename OtaT> static void reject_if_pending(OtaT &ota, const char *why) {
    if (!ota.is_pending_verify()) {
      return;
    }
    printf("OTA: %s on a pending image; rolling back\n", why);
    std::error_code ec;
    ota.mark_app_invalid_and_rollback(ec); // only returns on failure
    printf("OTA: rollback failed (%s); next reset will roll back\n", ec.message().c_str());
  }

  /// For a confirmed image that keeps crashing. Call once at boot, after the
  /// reset log has recorded this boot, with the recent resets of THIS image
  /// (oldest first; true = a crash: panic, watchdog, CPU lockup; not a
  /// brownout, which no other image would survive either). Pass only resets
  /// that happened while this slot was running, or a crash in the other
  /// slot could bounce the device straight back. If the last `threshold`
  /// entries are all crashes and the other slot holds an app image, that
  /// slot becomes the boot partition and this returns true; the caller then
  /// restarts. Nothing happens while the running image is still pending:
  /// the bootloader reverts a pending image by itself at the next reset.
  /// `recent_crashes` is any indexable range of bools (std::vector<bool>
  /// included, which is why this is not a std::span).
  template <typename OtaT, typename Crashes>
  static bool revert_if_crash_looping(OtaT &ota, const Crashes &recent_crashes,
                                      size_t threshold = CRASH_LOOP_RESETS) {
    const size_t count = std::size(recent_crashes);
    if (threshold == 0 || count < threshold) {
      return false;
    }
    for (size_t i = count - threshold; i < count; i++) {
      if (!recent_crashes[i]) {
        return false;
      }
    }
    if (ota.is_pending_verify()) {
      return false;
    }
    const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
    esp_app_desc_t desc;
    if (!other || esp_ota_get_partition_description(other, &desc) != ESP_OK) {
      printf("OTA: the last %u boots crashed, but the other slot holds no app image; staying\n",
             static_cast<unsigned>(threshold));
      return false;
    }
    if (const esp_err_t err = esp_ota_set_boot_partition(other); err != ESP_OK) {
      printf("OTA: the last %u boots crashed; could not switch to %s (%s)\n",
             static_cast<unsigned>(threshold), other->label, esp_err_to_name(err));
      return false;
    }
    printf("OTA: the last %u boots crashed; booting %s (%s) instead\n",
           static_cast<unsigned>(threshold), other->label, desc.version);
    return true;
  }

  /// Call once per polling-loop iteration. Free when nothing is pending;
  /// otherwise `sensor_ok` runs at most once per CHECK_INTERVAL, so it may
  /// do an I2C transaction.
  template <typename SensorOk> void tick(bool hid_ready, SensorOk &&sensor_ok) {
    if (!pending_) {
      return;
    }
    const auto now = Clock::now();
    if (now - last_check_ < CHECK_INTERVAL) {
      return;
    }
    last_check_ = now;
    if (!ota_.is_pending_verify()) {
      printf("OTA: image was confirmed elsewhere (console MARK_VALID); health check stopped\n");
      pending_ = false;
      return;
    }
    const bool sensor = sensor_ok();
    const bool healthy = hid_ready && sensor;
    std::error_code ec;
    if (healthy && now - start_ >= HEALTHY_UPTIME) {
      if (ota_.mark_app_valid(ec)) {
        printf("OTA: new firmware confirmed healthy; rollback cancelled\n");
      } else {
        printf("OTA: mark_app_valid failed (%s); next reset will roll back\n",
               ec.message().c_str());
      }
      pending_ = false;
      return;
    }
    if (now - start_ >= ROLLBACK_DEADLINE) {
      printf("OTA: new firmware never became healthy (hid_ready=%d sensor_ok=%d); rolling back\n",
             hid_ready, sensor);
      ota_.mark_app_invalid_and_rollback(ec); // only returns on failure
      printf("OTA: rollback failed (%s); next reset will roll back\n", ec.message().c_str());
      pending_ = false;
    }
  }

private:
  espp::Ota &ota_;
  bool pending_;
  typename Clock::time_point start_;
  typename Clock::time_point last_check_;
};

} // namespace espp
