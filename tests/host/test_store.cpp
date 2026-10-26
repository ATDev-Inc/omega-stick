// OmegaStore: load-on-construct, live setters, change callbacks, save.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "file_system.hpp"
#include "omega_store.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using espp::OmegaCalibration;
using espp::OmegaStore;
using support::TempDir;

namespace {

OmegaStore::Config config_for(const TempDir &dir) {
  return {.calibration_path = dir.file("omega_calibration.cfg"),
          .settings_path = dir.file("omega_settings.cfg")};
}

} // namespace

TEST(store_defaults_when_no_files_exist) {
  TempDir dir("store_empty");
  OmegaStore store(config_for(dir));
  CHECK_NEAR(store.center().center_x, 0.0f, 1e-6);
  CHECK_NEAR(store.range().max_x, 0.0f, 1e-6);
  CHECK(store.settings().input_mode == OmegaStore::InputMode::MOUSE);
  CHECK(!store.settings().manual_center_deadzone.has_value());
}

TEST(store_loads_existing_files_on_construction) {
  TempDir dir("store_load");
  auto cfg = config_for(dir);
  std::error_code ec;
  CHECK(OmegaCalibration::save(support::sample_center(), support::sample_range(),
                               cfg.calibration_path, ec));
  OmegaCalibration::DeviceSettings s;
  s.input_mode = OmegaStore::InputMode::XAC;
  s.manual_range_deadzone = 0.04f;
  CHECK(OmegaCalibration::DeviceSettings::save(s, cfg.settings_path, ec));

  OmegaStore store(cfg);
  CHECK_NEAR(store.center().center_x, support::sample_center().center_x, 1e-5);
  CHECK_NEAR(store.range().min_y, support::sample_range().min_y, 1e-5);
  CHECK(store.settings().input_mode == OmegaStore::InputMode::XAC);
  CHECK_NEAR(store.settings().manual_range_deadzone.value_or(0), 0.04f, 1e-5);
}

TEST(store_ignores_out_of_range_saved_deadzone_overrides) {
  TempDir dir("store_bad_dz");
  const auto cfg = config_for(dir);
  OmegaCalibration::DeviceSettings s;
  s.manual_center_deadzone = 2.0f; // above MAX_CENTER_DEADZONE
  s.manual_range_deadzone = 0.05f; // valid, kept
  std::error_code ec;
  CHECK(OmegaCalibration::DeviceSettings::save(s, cfg.settings_path, ec));
  OmegaStore store(cfg);
  CHECK(!store.settings().manual_center_deadzone.has_value());
  CHECK_NEAR(store.settings().manual_range_deadzone.value_or(0), 0.05f, 1e-6);
}

TEST(store_manual_offset_fires_callback_outside_lock) {
  TempDir dir("store_offset");
  auto cfg = config_for(dir);
  int fired = 0;
  float seen_x = 0;
  OmegaStore *self = nullptr;
  cfg.on_calibration_changed = [&]() {
    ++fired;
    // Reading back through the store from the callback would deadlock if the
    // store still held its mutex; the docs promise it does not.
    seen_x = self->center().manual_offset_x;
  };
  OmegaStore store(cfg);
  self = &store;

  store.set_manual_offset(1.5f, -2.0f);
  CHECK_EQ(fired, 1);
  CHECK_NEAR(seen_x, 1.5f, 1e-6);
  CHECK_NEAR(store.center().manual_offset_y, -2.0f, 1e-6);
}

TEST(store_deadzone_override_fires_callback_and_nudge_settings_do_not) {
  TempDir dir("store_dz");
  auto cfg = config_for(dir);
  int fired = 0;
  cfg.on_calibration_changed = [&]() { ++fired; };
  OmegaStore store(cfg);

  store.set_deadzone_override(0.2f, std::nullopt);
  CHECK_EQ(fired, 1);
  CHECK_NEAR(store.settings().manual_center_deadzone.value_or(-1), 0.2f, 1e-6);
  CHECK(!store.settings().manual_range_deadzone.has_value());

  store.set_nudge_settings(0.3f, 0.6f, std::chrono::milliseconds(80));
  CHECK_EQ(fired, 1);
  const auto s = store.settings();
  CHECK_NEAR(s.nudge_distance_threshold, 0.3f, 1e-6);
  CHECK_NEAR(s.nudge_exit_threshold, 0.6f, 1e-6);
  CHECK_EQ(s.nudge_hold_time.count(), 80);
}

TEST(store_input_mode_change_fires_mode_callback_with_new_mode) {
  TempDir dir("store_mode");
  auto cfg = config_for(dir);
  std::vector<OmegaStore::InputMode> seen;
  cfg.on_mode_changed = [&](OmegaStore::InputMode m) { seen.push_back(m); };
  OmegaStore store(cfg);

  store.set_input_mode(OmegaStore::InputMode::XAC);
  CHECK_EQ(seen.size(), size_t{1});
  CHECK(seen[0] == OmegaStore::InputMode::XAC);
  CHECK(store.settings().input_mode == OmegaStore::InputMode::XAC);
}

// Re-selecting the active mode from the console must not save + restart the device.
TEST(store_input_mode_same_value_is_a_no_op) {
  TempDir dir("store_mode_same");
  auto cfg = config_for(dir);
  int fired = 0;
  cfg.on_mode_changed = [&](OmegaStore::InputMode) { ++fired; };
  OmegaStore store(cfg); // defaults to MOUSE

  store.set_input_mode(OmegaStore::InputMode::MOUSE);
  CHECK_EQ(fired, 0);
}

// A drift nudge or recalibration pushed from the control loop's stale local
// CenterResult must not revert a manual offset the console set (CenterResult
// docs say the offset is unaffected by recalibration and drift correction).
TEST(store_set_measured_calibration_preserves_live_manual_offset) {
  TempDir dir("store_preserve");
  OmegaStore store(config_for(dir));
  store.set_manual_offset(1.0f, 0.5f);

  // What the control loop holds: measured values with the OLD (zero) offset.
  auto stale = support::sample_center();
  stale.manual_offset_x = 0.0f;
  stale.manual_offset_y = 0.0f;
  store.set_measured_calibration(stale, support::sample_range());

  CHECK_NEAR(store.center().center_x, stale.center_x, 1e-6); // measured part updated
  CHECK_NEAR(store.center().manual_offset_x, 1.0f, 1e-6);    // trim kept
  CHECK_NEAR(store.center().manual_offset_y, 0.5f, 1e-6);
}

TEST(store_save_to_flash_persists_both_files) {
  TempDir dir("store_save");
  auto cfg = config_for(dir);
  {
    OmegaStore store(cfg);
    store.set_calibration(support::sample_center(), support::sample_range());
    store.set_manual_offset(0.25f, 0.0f);
    store.set_deadzone_override(std::nullopt, 0.07f);
    store.set_input_mode(OmegaStore::InputMode::XAC);
    std::error_code ec;
    CHECK(store.save_to_flash(ec));
    CHECK(!ec);
  }
  CHECK(std::filesystem::exists(cfg.calibration_path));
  CHECK(std::filesystem::exists(cfg.settings_path));

  OmegaStore reloaded(cfg);
  CHECK_NEAR(reloaded.center().center_x, support::sample_center().center_x, 1e-5);
  CHECK_NEAR(reloaded.center().manual_offset_x, 0.25f, 1e-6);
  CHECK_NEAR(reloaded.settings().manual_range_deadzone.value_or(0), 0.07f, 1e-5);
  CHECK(reloaded.settings().input_mode == OmegaStore::InputMode::XAC);
}

TEST(store_nudge_enabled_saves_and_loads) {
  TempDir dir("store_nudge_enabled");
  auto cfg = config_for(dir);
  {
    OmegaStore store(cfg);
    CHECK(store.settings().nudge_enabled); // default on
    store.set_nudge_settings(0.3f, 0.6f, std::chrono::milliseconds(80));
    store.set_nudge_enabled(false);
    CHECK(!store.settings().nudge_enabled);
    std::error_code ec;
    CHECK(store.save_to_flash(ec));
    CHECK(!ec);
  }
  {
    std::ifstream in(cfg.settings_path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("nudge_enabled=0\n") != std::string::npos);
  }
  OmegaStore reloaded(cfg);
  const auto s = reloaded.settings();
  CHECK(!s.nudge_enabled);
  // Switching nudge clicks off keeps the tuning.
  CHECK_NEAR(s.nudge_distance_threshold, 0.3f, 1e-6);
  CHECK_NEAR(s.nudge_exit_threshold, 0.6f, 1e-6);
  CHECK_EQ(s.nudge_hold_time.count(), 80);

  reloaded.set_nudge_enabled(true);
  std::error_code ec;
  CHECK(reloaded.save_to_flash(ec));
  OmegaStore again(cfg);
  CHECK(again.settings().nudge_enabled);
}

// A settings file from firmware before the switch has no nudge_enabled key.
TEST(store_settings_file_without_nudge_enabled_loads_as_enabled) {
  TempDir dir("store_nudge_missing");
  auto cfg = config_for(dir);
  {
    std::ofstream out(cfg.settings_path);
    out << "schema=1\nnudge_distance_threshold=0.4\nnudge_exit_threshold=0.7\n"
           "nudge_hold_time_ms=90\ninput_mode=0\n";
  }
  OmegaStore store(cfg);
  CHECK(store.settings().nudge_enabled);
  CHECK_NEAR(store.settings().nudge_distance_threshold, 0.4f, 1e-6);
}

TEST(store_save_to_flash_reports_unwritable_path) {
  TempDir dir("store_badpath");
  auto cfg = config_for(dir);
  cfg.calibration_path = dir.path / "missing_dir" / "omega_calibration.cfg";
  OmegaStore store(cfg);
  std::error_code ec;
  CHECK(!store.save_to_flash(ec));
  CHECK(support::is_error(ec, OmegaCalibration::Error::file_open_failed));
  CHECK(!std::filesystem::exists(cfg.settings_path)); // settings left untouched
}

TEST(store_snapshot_is_consistent_copy) {
  TempDir dir("store_snap");
  OmegaStore store(config_for(dir));
  store.set_calibration(support::sample_center(), support::sample_range());
  const auto snap = store.snapshot();
  store.set_manual_offset(9.0f, 9.0f);
  CHECK_NEAR(snap.center.manual_offset_x, support::sample_center().manual_offset_x, 1e-6);
  CHECK_NEAR(store.center().manual_offset_x, 9.0f, 1e-6);
}

TEST(concurrent_save_to_flash_never_loses_the_files) {
  TempDir dir("store_concurrent");
  const auto cfg = config_for(dir);
  OmegaStore store(cfg);
  store.set_calibration(support::sample_center(), support::sample_range());

  // CHECK throws, which would terminate the process from a worker thread, so
  // the workers only count failures.
  std::atomic<int> failed_saves{0};
  auto worker = [&](bool nudge) {
    for (int i = 0; i < 200; ++i) {
      store.set_nudge_enabled(nudge);
      std::error_code ec;
      if (!store.save_to_flash(ec) || ec) {
        failed_saves++;
      }
    }
  };
  std::thread a(worker, true);
  std::thread b(worker, false);
  a.join();
  b.join();

  CHECK_EQ(failed_saves.load(), 0);
  CHECK(std::filesystem::exists(cfg.calibration_path));
  CHECK(std::filesystem::exists(cfg.settings_path));
  CHECK(!std::filesystem::exists(cfg.calibration_path.string() + ".tmp"));
  CHECK(!std::filesystem::exists(cfg.settings_path.string() + ".tmp"));
  OmegaStore reloaded(cfg);
  CHECK_NEAR(reloaded.center().center_x, support::sample_center().center_x, 1e-5);
}
