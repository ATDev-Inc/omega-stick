// FirmwareHealthCheck: confirms a pending OTA image once it has been healthy
// long enough, rolls it back past the deadline, and stands down when the
// image is confirmed by someone else (console MARK_VALID).

#include <chrono>
#include <vector>

#include "esp_ota_ops.h"
#include "firmware_health_check.hpp"
#include "ota.hpp"
#include "test_harness.hpp"

namespace {

// Manually advanced clock so the 10 s / 90 s thresholds run instantly.
struct FakeClock {
  using duration = std::chrono::steady_clock::duration;
  using time_point = std::chrono::time_point<FakeClock>;
  static inline time_point current{};
  static time_point now() { return current; }
  static void advance(std::chrono::seconds s) { current += s; }
};

using Health = espp::FirmwareHealthCheck<FakeClock>;

// espp::Ota is not copyable, so the fixture owns it and the tests use `ota`.
struct Fixture {
  espp::Ota ota{espp::Ota::Config{}};
  explicit Fixture(bool pending) { ota.pending_verify = pending; }
};

} // namespace

TEST(health_pending_and_healthy_is_confirmed) {
  Fixture ota_fx(true);
  auto &ota = ota_fx.ota;
  Health health(ota);
  for (int i = 0; i < 12; i++) {
    FakeClock::advance(std::chrono::seconds(1));
    health.tick(true, [] { return true; });
  }
  CHECK_EQ(ota.mark_valid_calls, 1);
  CHECK_EQ(ota.rollback_calls, 0);
  CHECK(!ota.pending_verify);
  // Done: further ticks do nothing.
  FakeClock::advance(std::chrono::seconds(100));
  health.tick(false, [] { return false; });
  CHECK_EQ(ota.mark_valid_calls, 1);
  CHECK_EQ(ota.rollback_calls, 0);
}

TEST(health_pending_and_unhealthy_rolls_back_once) {
  Fixture ota_fx(true);
  auto &ota = ota_fx.ota;
  Health health(ota);
  for (int i = 0; i < 120; i++) {
    FakeClock::advance(std::chrono::seconds(1));
    health.tick(true, [] { return false; });
  }
  CHECK_EQ(ota.rollback_calls, 1);
  CHECK_EQ(ota.mark_valid_calls, 0);
}

TEST(health_manual_confirm_is_not_undone) {
  Fixture ota_fx(true);
  auto &ota = ota_fx.ota;
  Health health(ota);
  FakeClock::advance(std::chrono::seconds(2));
  health.tick(true, [] { return true; });
  // Console MARK_VALID confirms the image behind the check's back.
  std::error_code ec;
  CHECK(ota.mark_app_valid(ec));
  const int manual_calls = ota.mark_valid_calls;
  // The sensor then fails all the way past the deadline.
  for (int i = 0; i < 120; i++) {
    FakeClock::advance(std::chrono::seconds(1));
    health.tick(true, [] { return false; });
  }
  CHECK_EQ(ota.rollback_calls, 0);
  CHECK_EQ(ota.mark_valid_calls, manual_calls);
}

TEST(health_not_pending_never_touches_sensor_or_ota) {
  Fixture ota_fx(false);
  auto &ota = ota_fx.ota;
  Health health(ota);
  int sensor_calls = 0;
  for (int i = 0; i < 120; i++) {
    FakeClock::advance(std::chrono::seconds(1));
    health.tick(false, [&] {
      ++sensor_calls;
      return false;
    });
  }
  CHECK_EQ(sensor_calls, 0);
  CHECK_EQ(ota.mark_valid_calls, 0);
  CHECK_EQ(ota.rollback_calls, 0);
}

TEST(health_reject_if_pending_rolls_back_only_when_pending) {
  Fixture pending_fx(true);
  auto &pending = pending_fx.ota;
  Health::reject_if_pending(pending, "test failure");
  CHECK_EQ(pending.rollback_calls, 1);

  Fixture settled_fx(false);
  auto &settled = settled_fx.ota;
  Health::reject_if_pending(settled, "test failure");
  CHECK_EQ(settled.rollback_calls, 0);
}

// ---- crash-loop revert (fed from the reset log at boot) ----

TEST(health_crash_loop_switches_to_the_other_slot) {
  esp_ota_fake().reset();
  Fixture fx(false);
  const bool crashes[] = {false, true, true, true};
  CHECK(Health::revert_if_crash_looping(fx.ota, crashes));
  CHECK_EQ(esp_ota_fake().set_boot_calls, 1);
  CHECK(esp_ota_fake().last_set_boot == &esp_ota_fake().other);
}

TEST(health_crash_loop_needs_consecutive_crashes) {
  esp_ota_fake().reset();
  Fixture fx(false);
  const bool interrupted[] = {true, true, false, true};
  CHECK(!Health::revert_if_crash_looping(fx.ota, interrupted));
  const bool too_few[] = {true, true};
  CHECK(!Health::revert_if_crash_looping(fx.ota, too_few));
  CHECK(!Health::revert_if_crash_looping(fx.ota, std::vector<bool>{}));
  CHECK_EQ(esp_ota_fake().set_boot_calls, 0);
}

TEST(health_crash_loop_leaves_a_pending_image_to_the_bootloader) {
  esp_ota_fake().reset();
  Fixture fx(true);
  const bool crashes[] = {true, true, true};
  CHECK(!Health::revert_if_crash_looping(fx.ota, crashes));
  CHECK_EQ(esp_ota_fake().set_boot_calls, 0);
}

TEST(health_crash_loop_stays_put_without_another_image) {
  esp_ota_fake().reset();
  Fixture fx(false);
  const bool crashes[] = {true, true, true};
  esp_ota_fake().other_has_image = false;
  CHECK(!Health::revert_if_crash_looping(fx.ota, crashes));
  esp_ota_fake().other_has_image = true;
  esp_ota_fake().has_other_partition = false;
  CHECK(!Health::revert_if_crash_looping(fx.ota, crashes));
  CHECK_EQ(esp_ota_fake().set_boot_calls, 0);
}

TEST(health_crash_loop_reports_a_failed_switch) {
  esp_ota_fake().reset();
  Fixture fx(false);
  const bool crashes[] = {true, true, true};
  esp_ota_fake().set_boot_result = ESP_FAIL;
  CHECK(!Health::revert_if_crash_looping(fx.ota, crashes));
  CHECK_EQ(esp_ota_fake().set_boot_calls, 1);
}

TEST(health_crash_loop_threshold_is_adjustable_and_zero_disables) {
  esp_ota_fake().reset();
  Fixture fx(false);
  const bool crashes[] = {true};
  CHECK(Health::revert_if_crash_looping(fx.ota, crashes, 1));
  CHECK(!Health::revert_if_crash_looping(fx.ota, crashes, 0));
  CHECK_EQ(esp_ota_fake().set_boot_calls, 1);
}
