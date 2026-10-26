// FirmwareHealthCheck: never confirms a pending OTA image itself (the host does,
// with the console's MARK_VALID), except the one the crash-loop handover booted;
// past the deadline it asks for a neutral output hold, then the rollback; and it
// stands down once the host has confirmed the image.

#include <chrono>
#include <vector>

#include "esp_ota_ops.h"
#include "firmware_health_check.hpp"
#include "ota.hpp"
#include "test_harness.hpp"

namespace {

// Manually advanced clock so the 120 s deadline runs instantly.
struct FakeClock {
  using duration = std::chrono::steady_clock::duration;
  using time_point = std::chrono::time_point<FakeClock>;
  static inline time_point current{};
  static time_point now() { return current; }
  static void advance(duration d) { current += d; }
};

using Health = espp::FirmwareHealthCheck<FakeClock>;

// espp::Ota is not copyable, so the fixture owns it and the tests use `ota`.
struct Fixture {
  espp::Ota ota{espp::Ota::Config{}};
  explicit Fixture(bool pending) { ota.pending_verify = pending; }
};

} // namespace

TEST(health_pending_image_is_not_confirmed_by_the_firmware) {
  Fixture ota_fx(true);
  auto &ota = ota_fx.ota;
  Health health(ota);
  // Well past the old 10 s self-confirm point, up to just before the deadline.
  const int before_deadline = static_cast<int>(Health::ROLLBACK_DEADLINE.count()) - 1;
  for (int i = 0; i < before_deadline; i++) {
    FakeClock::advance(std::chrono::seconds(1));
    health.tick();
  }
  CHECK_EQ(ota.mark_valid_calls, 0);
  CHECK_EQ(ota.rollback_calls, 0);
  CHECK(ota.pending_verify);
}

TEST(health_unconfirmed_image_rolls_back_once_at_the_deadline) {
  using Action = Health::Action;
  Fixture ota_fx(true);
  auto &ota = ota_fx.ota;
  Health health(ota);
  int holds = 0;
  int rollbacks = 0;
  int hold_at = -1;
  for (int i = 0; i < 300; i++) {
    FakeClock::advance(std::chrono::seconds(1));
    switch (health.tick()) {
    case Action::HOLD_OUTPUT:
      ++holds;
      hold_at = i + 1;
      // The output is held neutral first: no rollback in the same tick.
      CHECK_EQ(ota.rollback_calls, 0);
      break;
    case Action::ROLL_BACK:
      ++rollbacks;
      health.rollback_now();
      break;
    case Action::NONE:
      break;
    }
  }
  CHECK_EQ(holds, 1);
  CHECK_EQ(hold_at, static_cast<int>(Health::ROLLBACK_DEADLINE.count()));
  CHECK_EQ(rollbacks, 1);
  CHECK_EQ(ota.rollback_calls, 1);
  CHECK_EQ(ota.mark_valid_calls, 0);
}

// D22 rule 1: between the deadline and the rollback the loop gets ROLLBACK_HOLD
// to send neutral reports; tick() itself never rolls back.
TEST(health_rollback_waits_for_the_output_hold) {
  using Action = Health::Action;
  Fixture ota_fx(true);
  auto &ota = ota_fx.ota;
  Health health(ota);
  FakeClock::advance(Health::ROLLBACK_DEADLINE);
  CHECK(health.tick() == Action::HOLD_OUTPUT);
  const auto step = std::chrono::milliseconds(10);
  for (auto held = step; held < Health::ROLLBACK_HOLD; held += step) {
    FakeClock::advance(step);
    CHECK(health.tick() == Action::NONE);
  }
  FakeClock::advance(step);
  CHECK(health.tick() == Action::ROLL_BACK);
  CHECK_EQ(ota.rollback_calls, 0);
  health.rollback_now();
  CHECK_EQ(ota.rollback_calls, 1);
  // Once only, even if the rollback returned (failed).
  for (int i = 0; i < 300; i++) {
    FakeClock::advance(std::chrono::seconds(1));
    CHECK(health.tick() == Action::NONE);
  }
}

TEST(health_rollback_now_stands_down_when_confirmed_during_the_hold) {
  using Action = Health::Action;
  Fixture ota_fx(true);
  auto &ota = ota_fx.ota;
  Health health(ota);
  FakeClock::advance(Health::ROLLBACK_DEADLINE);
  CHECK(health.tick() == Action::HOLD_OUTPUT);
  std::error_code ec;
  CHECK(ota.mark_app_valid(ec)); // MARK_VALID arrives just in time
  FakeClock::advance(Health::ROLLBACK_HOLD);
  CHECK(health.tick() == Action::ROLL_BACK);
  health.rollback_now();
  CHECK_EQ(ota.rollback_calls, 0);
}

TEST(health_host_confirm_is_not_undone) {
  Fixture ota_fx(true);
  auto &ota = ota_fx.ota;
  Health health(ota);
  FakeClock::advance(std::chrono::seconds(2));
  health.tick();
  // The console's MARK_VALID confirms the image behind the check's back.
  std::error_code ec;
  CHECK(ota.mark_app_valid(ec));
  const int manual_calls = ota.mark_valid_calls;
  // Time runs well past the deadline: the check has stood down.
  for (int i = 0; i < 300; i++) {
    FakeClock::advance(std::chrono::seconds(1));
    health.tick();
  }
  CHECK_EQ(ota.rollback_calls, 0);
  CHECK_EQ(ota.mark_valid_calls, manual_calls);
}

TEST(health_not_pending_never_touches_ota) {
  Fixture ota_fx(false);
  auto &ota = ota_fx.ota;
  Health health(ota);
  for (int i = 0; i < 300; i++) {
    FakeClock::advance(std::chrono::seconds(1));
    health.tick();
  }
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

TEST(health_crash_loop_stays_put_when_the_other_slot_was_rejected) {
  esp_ota_fake().reset();
  Fixture fx(false);
  const bool crashes[] = {true, true, true};
  esp_ota_fake().other_state = ESP_OTA_IMG_INVALID;
  CHECK(!Health::revert_if_crash_looping(fx.ota, crashes));
  esp_ota_fake().other_state = ESP_OTA_IMG_ABORTED;
  CHECK(!Health::revert_if_crash_looping(fx.ota, crashes));
  CHECK_EQ(esp_ota_fake().set_boot_calls, 0);
  esp_ota_fake().other_state = ESP_OTA_IMG_VALID;
  CHECK(Health::revert_if_crash_looping(fx.ota, crashes));
  CHECK_EQ(esp_ota_fake().set_boot_calls, 1);
}

// A reset is charged to the boot it ended, which is the previous entry's slot.
TEST(health_crashes_while_running_charges_a_reset_to_the_previous_boot) {
  using Boot = Health::BootRecord;
  // A (ota_0) is confirmed; a broken B is flashed into ota_1 and retried twice:
  // each B boot panics and the bootloader comes back to A. Oldest first.
  const std::vector<Boot> boots = {
      {"ota_0", false}, // power-on
      {"ota_1", false}, // software: update to B
      {"ota_0", true},  // panic: B crashed, bootloader returned to A
      {"ota_1", false}, // software: retry
      {"ota_0", true},  // panic
      {"ota_1", false}, // software: retry
      {"ota_0", true},  // panic
  };
  const auto a = Health::crashes_while_running(boots, "ota_0");
  const auto b = Health::crashes_while_running(boots, "ota_1");
  CHECK_EQ(a.size(), std::size_t{3});
  CHECK(!a[0] && !a[1] && !a[2]); // A only ever ended by deliberate updates
  CHECK_EQ(b.size(), std::size_t{3});
  CHECK(b[0] && b[1] && b[2]); // every B run ended in a panic
  // So the good image stays put, and B would be reverted if it were running.
  esp_ota_fake().reset();
  Fixture fx(false);
  CHECK(!Health::revert_if_crash_looping(fx.ota, a));
  CHECK(Health::revert_if_crash_looping(fx.ota, b));
}

TEST(health_crashes_while_running_skips_the_oldest_entry_and_unknown_slots) {
  using Boot = Health::BootRecord;
  const std::vector<Boot> single = {{"ota_0", true}};
  CHECK(Health::crashes_while_running(single, "ota_0").empty());
  // Entries from before slots were recorded carry an empty slot and never match.
  const std::vector<Boot> legacy = {{"", true}, {"", true}, {"ota_0", true}, {"ota_0", true}};
  const auto out = Health::crashes_while_running(legacy, "ota_0");
  CHECK_EQ(out.size(), std::size_t{1});
  CHECK(out[0]);
}

TEST(health_crash_loop_threshold_is_adjustable_and_zero_disables) {
  esp_ota_fake().reset();
  Fixture fx(false);
  const bool crashes[] = {true};
  CHECK(Health::revert_if_crash_looping(fx.ota, crashes, 1));
  CHECK(!Health::revert_if_crash_looping(fx.ota, crashes, 0));
  CHECK_EQ(esp_ota_fake().set_boot_calls, 1);
}

// ---- crash-loop handover marker ----

TEST(health_handover_marker_is_read_once_and_remembered_until_power_on) {
  uint32_t marker = 0;
  auto m = Health::take_handover_marker(marker, false);
  CHECK(!m.booted_by_handover);
  CHECK(!m.handed_over_since_power_on);
  CHECK_EQ(marker, 0u);

  // The handover leaves HANDOVER_REQUESTED before its restart.
  marker = Health::HANDOVER_REQUESTED;
  m = Health::take_handover_marker(marker, false);
  CHECK(m.booted_by_handover);
  CHECK(m.handed_over_since_power_on);
  CHECK_EQ(marker, Health::HANDOVER_DONE);

  // Later software resets: no longer "booted by the handover", but no second
  // handover either.
  m = Health::take_handover_marker(marker, false);
  CHECK(!m.booted_by_handover);
  CHECK(m.handed_over_since_power_on);
  CHECK_EQ(marker, Health::HANDOVER_DONE);

  // A power-on clears it, whatever the memory holds.
  marker = Health::HANDOVER_REQUESTED;
  m = Health::take_handover_marker(marker, true);
  CHECK(!m.booted_by_handover);
  CHECK(!m.handed_over_since_power_on);
  CHECK_EQ(marker, 0u);

  // Garbage is no marker.
  marker = 0xDEADBEEF;
  m = Health::take_handover_marker(marker, false);
  CHECK(!m.handed_over_since_power_on);
  CHECK_EQ(marker, 0u);
}

TEST(health_accept_after_handover_confirms_a_pending_image) {
  Fixture fx(true);
  CHECK(Health::accept_after_handover(fx.ota, true));
  CHECK_EQ(fx.ota.mark_valid_calls, 1);
  CHECK(!fx.ota.pending_verify);
}

TEST(health_accept_after_handover_leaves_a_confirmed_image_alone) {
  Fixture fx(false);
  CHECK(!Health::accept_after_handover(fx.ota, true));
  CHECK_EQ(fx.ota.mark_valid_calls, 0);
}

TEST(health_accept_after_handover_needs_the_marker) {
  // An ordinary update boots pending too; only the console may confirm that one.
  Fixture fx(true);
  CHECK(!Health::accept_after_handover(fx.ota, false));
  CHECK_EQ(fx.ota.mark_valid_calls, 0);
  CHECK(fx.ota.pending_verify);
}

TEST(health_accept_after_handover_reports_a_failed_confirm) {
  Fixture fx(true);
  fx.ota.fail_mark_valid = true;
  CHECK(!Health::accept_after_handover(fx.ota, true));
  CHECK(fx.ota.pending_verify);
}
