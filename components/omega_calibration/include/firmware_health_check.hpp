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
// Two related rules:
//  - A fatal init failure (sensor, calibration, USB) on a pending image calls
//    reject_if_pending() so it rolls back immediately instead of halting on a
//    broken image forever.
//  - The calibration console's MARK_VALID button is a manual override. The
//    pending state is re-read on every check, so once someone confirms the
//    image the automatic check stands down and never rolls it back.
//
// Consequence worth knowing: any reset (mode switch, unplug) during the first
// HEALTHY_UPTIME after an update reverts to the previous firmware. The
// console shows the image as "pending verify" until it is confirmed.

#include <chrono>
#include <cstdio>
#include <system_error>

#include "ota.hpp"

namespace espp {

/// The clock is injectable so host tests can advance time by hand.
template <typename Clock = std::chrono::steady_clock> class FirmwareHealthCheck {
public:
  static constexpr std::chrono::seconds HEALTHY_UPTIME{10};
  static constexpr std::chrono::seconds ROLLBACK_DEADLINE{90};
  static constexpr std::chrono::seconds CHECK_INTERVAL{1};

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
