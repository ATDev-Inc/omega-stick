// OmegaCalibration::save/load and DeviceSettings::save/load round-trips.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "file_system.hpp"
#include "omega_calibration.hpp"
#include "omega_store.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using espp::OmegaCalibration;
using support::TempDir;

TEST(calibration_round_trips_through_file) {
  TempDir dir("cal");
  const auto path = dir.file("omega_calibration.cfg");
  std::error_code ec;

  CHECK(OmegaCalibration::save(support::sample_center(), support::sample_range(), path, ec));
  CHECK(!ec);

  OmegaCalibration::CenterResult center;
  OmegaCalibration::RangeResult range;
  CHECK(OmegaCalibration::load(center, range, path, ec));
  CHECK(!ec);

  const auto want_c = support::sample_center();
  const auto want_r = support::sample_range();
  CHECK_NEAR(center.center_x, want_c.center_x, 1e-5);
  CHECK_NEAR(center.center_y, want_c.center_y, 1e-5);
  CHECK_NEAR(center.noise_radius, want_c.noise_radius, 1e-5);
  CHECK_NEAR(center.deadzone, want_c.deadzone, 1e-5);
  CHECK_NEAR(center.manual_offset_x, want_c.manual_offset_x, 1e-5);
  CHECK_NEAR(center.manual_offset_y, want_c.manual_offset_y, 1e-5);
  CHECK_NEAR(range.min_x, want_r.min_x, 1e-5);
  CHECK_NEAR(range.max_x, want_r.max_x, 1e-5);
  CHECK_NEAR(range.min_y, want_r.min_y, 1e-5);
  CHECK_NEAR(range.max_y, want_r.max_y, 1e-5);
  CHECK_NEAR(range.usable_radius, want_r.usable_radius, 1e-5);
  CHECK_NEAR(range.reliable_max_radius, want_r.reliable_max_radius, 1e-5);
  CHECK_NEAR(range.normalized_max_radius, want_r.normalized_max_radius, 1e-4);
  CHECK_NEAR(range.deadzone, want_r.deadzone, 1e-5);
  CHECK(range.sector_max_radius.empty()); // diagnostic only, not persisted
}

TEST(calibration_load_rejects_missing_required_field) {
  TempDir dir("cal_missing");
  const auto path = dir.file("omega_calibration.cfg");
  {
    std::ofstream out(path);
    out << "center_x=1\ncenter_y=2\ncenter_deadzone=0.1\nrange_min_x=-1\nrange_max_x=1\n";
    // range_min_y / range_max_y / range_deadzone missing
  }
  OmegaCalibration::CenterResult center;
  OmegaCalibration::RangeResult range;
  std::error_code ec;
  CHECK(!OmegaCalibration::load(center, range, path, ec));
  CHECK(support::is_error(ec, OmegaCalibration::Error::file_missing_field));
}

TEST(calibration_load_rejects_unparseable_value) {
  TempDir dir("cal_parse");
  const auto path = dir.file("omega_calibration.cfg");
  {
    std::ofstream out(path);
    out << "center_x=abc\n";
  }
  OmegaCalibration::CenterResult center;
  OmegaCalibration::RangeResult range;
  std::error_code ec;
  CHECK(!OmegaCalibration::load(center, range, path, ec));
  CHECK(support::is_error(ec, OmegaCalibration::Error::file_parse_failed));
}

TEST(calibration_load_reports_missing_file) {
  TempDir dir("cal_nofile");
  OmegaCalibration::CenterResult center;
  OmegaCalibration::RangeResult range;
  std::error_code ec;
  CHECK(!OmegaCalibration::load(center, range, dir.file("nope.cfg"), ec));
  CHECK(support::is_error(ec, OmegaCalibration::Error::file_open_failed));
}

// The calibration the maintainer's board saved after a first-boot sweep with
// the stick untouched (D26): usable radius below the center noise radius.
TEST(calibration_load_treats_an_unusable_file_as_no_calibration) {
  TempDir dir("cal_unusable");
  const auto path = dir.file("omega_calibration.cfg");
  OmegaCalibration::CenterResult bad_c;
  bad_c.center_x = 0.31f;
  bad_c.center_y = -0.12f;
  bad_c.noise_radius = 0.086f;
  bad_c.deadzone = 0.5f;
  bad_c.manual_offset_x = 0.02f;
  OmegaCalibration::RangeResult bad_r;
  bad_r.min_x = -0.058f;
  bad_r.max_x = 0.060f;
  bad_r.min_y = -0.049f;
  bad_r.max_y = 0.055f;
  bad_r.usable_radius = 0.049f;
  bad_r.deadzone = 0.1f;
  std::error_code ec;
  CHECK(OmegaCalibration::save(bad_c, bad_r, path, ec));

  OmegaCalibration::CenterResult center;
  OmegaCalibration::RangeResult range;
  CHECK(!OmegaCalibration::load(center, range, path, ec));
  CHECK(support::is_error(ec, OmegaCalibration::Error::calibration_unusable));
  // A distinct error from a missing or broken file, the file is left in
  // place, and the values are still read (the boot keeps the manual offset).
  CHECK(!support::is_error(ec, OmegaCalibration::Error::file_open_failed));
  CHECK(std::filesystem::exists(path));
  CHECK_NEAR(center.manual_offset_x, 0.02f, 1e-6);
  CHECK_NEAR(range.usable_radius, 0.049f, 1e-6);
}

TEST(calibration_load_derives_a_missing_usable_radius_from_the_bounds) {
  TempDir dir("cal_no_usable");
  const auto path = dir.file("omega_calibration.cfg");
  {
    std::ofstream out(path);
    out << "center_x=1\ncenter_y=2\ncenter_noise_radius=0.1\ncenter_deadzone=0.1\n"
           "range_min_x=-4\nrange_max_x=5\nrange_min_y=-6\nrange_max_y=7\nrange_deadzone=0.05\n";
  }
  OmegaCalibration::CenterResult center;
  OmegaCalibration::RangeResult range;
  std::error_code ec;
  CHECK(OmegaCalibration::load(center, range, path, ec));
  CHECK_NEAR(range.usable_radius, 4.0f, 1e-6); // the weakest axis extreme
  {
    std::ofstream out(path); // the same bounds against noise 2: 4 < 3 x 2
    out << "center_x=1\ncenter_y=2\ncenter_noise_radius=2\ncenter_deadzone=0.1\n"
           "range_min_x=-4\nrange_max_x=5\nrange_min_y=-6\nrange_max_y=7\nrange_deadzone=0.05\n";
  }
  CHECK(!OmegaCalibration::load(center, range, path, ec));
  CHECK(support::is_error(ec, OmegaCalibration::Error::calibration_unusable));
}

TEST(calibration_clear_tolerates_missing_and_removes_present) {
  TempDir dir("cal_clear");
  const auto path = dir.file("omega_calibration.cfg");
  std::error_code ec;
  CHECK(OmegaCalibration::clear(path, ec));
  CHECK(OmegaCalibration::save(support::sample_center(), support::sample_range(), path, ec));
  CHECK(std::filesystem::exists(path));
  CHECK(OmegaCalibration::clear(path, ec));
  CHECK(!ec);
  CHECK(!std::filesystem::exists(path));
}

TEST(settings_round_trip_with_overrides_set) {
  TempDir dir("settings");
  const auto path = dir.file("omega_settings.cfg");
  OmegaCalibration::DeviceSettings in;
  in.manual_center_deadzone = 0.12f;
  in.manual_range_deadzone = 0.03f;
  in.nudge_distance_threshold = 0.4f;
  in.nudge_exit_threshold = 0.7f;
  in.nudge_hold_time = std::chrono::milliseconds(250);
  in.input_mode = OmegaCalibration::InputMode::XAC;
  in.nudge_enabled = false;

  std::error_code ec;
  CHECK(OmegaCalibration::DeviceSettings::save(in, path, ec));

  OmegaCalibration::DeviceSettings out;
  CHECK(OmegaCalibration::DeviceSettings::load(out, path, ec));
  CHECK(out.manual_center_deadzone.has_value());
  CHECK_NEAR(*out.manual_center_deadzone, 0.12f, 1e-5);
  CHECK(out.manual_range_deadzone.has_value());
  CHECK_NEAR(*out.manual_range_deadzone, 0.03f, 1e-5);
  CHECK_NEAR(out.nudge_distance_threshold, 0.4f, 1e-5);
  CHECK_NEAR(out.nudge_exit_threshold, 0.7f, 1e-5);
  CHECK_EQ(out.nudge_hold_time.count(), 250);
  CHECK(out.input_mode == OmegaCalibration::InputMode::XAC);
  CHECK(!out.nudge_enabled);
}

TEST(settings_round_trip_with_overrides_cleared) {
  TempDir dir("settings_clear");
  const auto path = dir.file("omega_settings.cfg");
  OmegaCalibration::DeviceSettings in; // defaults: no overrides, MOUSE
  std::error_code ec;
  CHECK(OmegaCalibration::DeviceSettings::save(in, path, ec));

  OmegaCalibration::DeviceSettings out;
  out.manual_center_deadzone = 0.9f; // must be cleared by load()
  CHECK(OmegaCalibration::DeviceSettings::load(out, path, ec));
  CHECK(!out.manual_center_deadzone.has_value());
  CHECK(!out.manual_range_deadzone.has_value());
  CHECK(out.input_mode == OmegaCalibration::InputMode::MOUSE);
}

TEST(settings_partial_file_falls_back_to_defaults) {
  TempDir dir("settings_partial");
  const auto path = dir.file("omega_settings.cfg");
  {
    std::ofstream out(path);
    out << "input_mode=1\n";
  }
  OmegaCalibration::DeviceSettings out;
  out.nudge_enabled = false; // a missing key restores the default, it does not keep this
  std::error_code ec;
  CHECK(OmegaCalibration::DeviceSettings::load(out, path, ec));
  CHECK(out.input_mode == OmegaCalibration::InputMode::XAC);
  CHECK(out.nudge_enabled);
  CHECK_NEAR(out.nudge_distance_threshold, 0.5f, 1e-6);
  CHECK_NEAR(out.nudge_exit_threshold, 0.8f, 1e-6);
  CHECK_EQ(out.nudge_hold_time.count(), 100);
}

TEST(effective_values_prefer_manual_overrides) {
  const auto center = support::sample_center();
  const auto range = support::sample_range();
  OmegaCalibration::DeviceSettings s;
  CHECK_NEAR(s.effective_center_deadzone(center), center.deadzone, 1e-6);
  CHECK_NEAR(s.effective_range_deadzone(range), range.deadzone, 1e-6);
  s.manual_center_deadzone = 0.2f;
  s.manual_range_deadzone = 0.08f;
  CHECK_NEAR(s.effective_center_deadzone(center), 0.2f, 1e-6);
  CHECK_NEAR(s.effective_range_deadzone(range), 0.08f, 1e-6);
  CHECK_NEAR(center.effective_center_x(), center.center_x + center.manual_offset_x, 1e-6);
  CHECK_NEAR(center.effective_center_y(), center.center_y + center.manual_offset_y, 1e-6);
}

TEST(apply_calibration_pushes_effective_center_and_deadzones) {
  espp::Joystick js({.x_calibration = {.center = 0, .minimum = -1, .maximum = 1},
                     .y_calibration = {.center = 0, .minimum = -1, .maximum = 1},
                     .type = espp::Joystick::Type::CIRCULAR});
  const auto center = support::sample_center();
  const auto range = support::sample_range();
  OmegaCalibration::DeviceSettings s;
  s.manual_center_deadzone = 0.3f;

  OmegaCalibration::apply_calibration(js, center, range, s);
  CHECK_NEAR(js.center_deadzone_radius(), 0.3f, 1e-6);
  CHECK_NEAR(js.range_deadzone(), range.deadzone, 1e-6);

  const float cx = center.effective_center_x();
  const float cy = center.effective_center_y();

  // Raw reading at the effective center maps to the origin.
  js.update(cx, cy);
  CHECK_NEAR(js.x(), 0.0f, 1e-5);
  CHECK_NEAR(js.y(), 0.0f, 1e-5);

  // Full deflection on each side of the calibrated range lands on the unit
  // circle (the range deadzone makes the edge snap to magnitude 1).
  js.update(cx + range.max_x, cy);
  CHECK_NEAR(js.x(), 1.0f, 1e-5);
  CHECK_NEAR(js.y(), 0.0f, 1e-5);
  js.update(cx + range.min_x, cy);
  CHECK_NEAR(js.x(), -1.0f, 1e-5);
  js.update(cx, cy + range.max_y);
  CHECK_NEAR(js.y(), 1.0f, 1e-5);
  js.update(cx, cy + range.min_y);
  CHECK_NEAR(js.y(), -1.0f, 1e-5);

  // A small deflection inside the 0.3 manual center deadzone is zeroed.
  js.update(cx + 0.1f * range.max_x, cy);
  CHECK_NEAR(js.x(), 0.0f, 1e-5);
  CHECK_NEAR(js.y(), 0.0f, 1e-5);

  // Half deflection is rescaled past the center deadzone:
  // (0.5 - 0.3) / (1 - 0.3 - range.deadzone).
  js.update(cx + 0.5f * range.max_x, cy);
  CHECK_NEAR(js.x(), 0.2f / (1.0f - 0.3f - range.deadzone), 1e-4);
}

namespace {
const char *kCalibrationBody =
    "center_x=1\ncenter_y=2\ncenter_deadzone=0.1\nrange_min_x=-1\n"
    "range_max_x=1\nrange_min_y=-1\nrange_max_y=1\nrange_deadzone=0.05\n";

bool load_calibration(const std::filesystem::path &path, std::error_code &ec) {
  OmegaCalibration::CenterResult center;
  OmegaCalibration::RangeResult range;
  return OmegaCalibration::load(center, range, path, ec);
}

bool load_settings(const std::filesystem::path &path, std::error_code &ec) {
  OmegaCalibration::DeviceSettings out;
  return OmegaCalibration::DeviceSettings::load(out, path, ec);
}

std::string read_all(const std::filesystem::path &path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
} // namespace

TEST(save_writes_schema_key_to_both_files) {
  TempDir dir("schema_write");
  std::error_code ec;
  CHECK(OmegaCalibration::save(support::sample_center(), support::sample_range(),
                               dir.file("omega_calibration.cfg"), ec));
  CHECK(OmegaCalibration::DeviceSettings::save({}, dir.file("omega_settings.cfg"), ec));
  CHECK(read_all(dir.file("omega_calibration.cfg")).find("schema=1\n") != std::string::npos);
  CHECK(read_all(dir.file("omega_settings.cfg")).find("schema=1\n") != std::string::npos);
}

TEST(calibration_without_schema_key_loads_as_schema_1) {
  TempDir dir("schema_cal_none");
  const auto path = dir.file("omega_calibration.cfg");
  { std::ofstream(path) << kCalibrationBody; }
  std::error_code ec;
  CHECK(load_calibration(path, ec));
  CHECK(!ec);
}

TEST(calibration_with_schema_1_loads) {
  TempDir dir("schema_cal_1");
  const auto path = dir.file("omega_calibration.cfg");
  { std::ofstream(path) << "schema=1\n" << kCalibrationBody; }
  std::error_code ec;
  CHECK(load_calibration(path, ec));
  CHECK(!ec);
}

TEST(calibration_with_newer_schema_is_rejected) {
  TempDir dir("schema_cal_2");
  const auto path = dir.file("omega_calibration.cfg");
  { std::ofstream(path) << "schema=2\n" << kCalibrationBody; }
  std::error_code ec;
  CHECK(!load_calibration(path, ec));
  CHECK(support::is_error(ec, OmegaCalibration::Error::unsupported_schema));
}

TEST(calibration_with_malformed_schema_is_rejected) {
  TempDir dir("schema_cal_bad");
  const auto path = dir.file("omega_calibration.cfg");
  { std::ofstream(path) << "schema=0\n" << kCalibrationBody; }
  std::error_code ec;
  CHECK(!load_calibration(path, ec));
  CHECK(support::is_error(ec, OmegaCalibration::Error::file_parse_failed));
}

TEST(settings_without_schema_key_loads_as_schema_1) {
  TempDir dir("schema_set_none");
  const auto path = dir.file("omega_settings.cfg");
  { std::ofstream(path) << "nudge_hold_time_ms=250\ninput_mode=1\n"; }
  OmegaCalibration::DeviceSettings out;
  std::error_code ec;
  CHECK(OmegaCalibration::DeviceSettings::load(out, path, ec));
  CHECK(!ec);
  CHECK_EQ(out.nudge_hold_time.count(), 250);
  CHECK(out.input_mode == OmegaCalibration::InputMode::XAC);
}

TEST(settings_with_schema_1_loads) {
  TempDir dir("schema_set_1");
  const auto path = dir.file("omega_settings.cfg");
  { std::ofstream(path) << "schema=1\nnudge_hold_time_ms=250\n"; }
  std::error_code ec;
  CHECK(load_settings(path, ec));
  CHECK(!ec);
}

TEST(settings_with_newer_schema_is_rejected) {
  TempDir dir("schema_set_2");
  const auto path = dir.file("omega_settings.cfg");
  { std::ofstream(path) << "schema=2\ninput_mode=1\n"; }
  std::error_code ec;
  CHECK(!load_settings(path, ec));
  CHECK(support::is_error(ec, OmegaCalibration::Error::unsupported_schema));
}

TEST(store_falls_back_to_defaults_on_newer_schema) {
  TempDir dir("schema_store");
  const auto cal = dir.file("omega_calibration.cfg");
  const auto set = dir.file("omega_settings.cfg");
  { std::ofstream(cal) << "schema=2\n" << kCalibrationBody; }
  { std::ofstream(set) << "schema=2\ninput_mode=1\nnudge_hold_time_ms=250\n"; }
  espp::OmegaStore store({.calibration_path = cal, .settings_path = set});
  CHECK(store.settings().input_mode == OmegaCalibration::InputMode::MOUSE);
  CHECK_EQ(store.settings().nudge_hold_time.count(), 100);
  CHECK_NEAR(store.center().center_x, OmegaCalibration::CenterResult{}.center_x, 1e-9);
}

namespace {
std::string slurp(const std::filesystem::path &path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
} // namespace

TEST(failed_write_leaves_previous_file_and_no_temp) {
  TempDir dir("atomic_write_fail");
  const auto path = dir.file("omega_settings.cfg");
  std::error_code ec;
  OmegaCalibration::DeviceSettings settings;
  settings.input_mode = OmegaCalibration::InputMode::XAC;
  CHECK(OmegaCalibration::DeviceSettings::save(settings, path, ec));
  const std::string before = slurp(path);
  CHECK(!before.empty());

  // A writer that fails partway, as a full filesystem would.
  CHECK(!OmegaCalibration::write_file_atomically(path, ec, [](std::ostream &out) {
    out << "schema=1\npartial";
    out.setstate(std::ios::badbit);
  }));
  CHECK(support::is_error(ec, OmegaCalibration::Error::file_write_failed));
  CHECK_EQ(slurp(path), before);
  CHECK(!std::filesystem::exists(path.string() + ".tmp"));
}

TEST(failed_rename_leaves_no_temp_and_reports_error) {
  TempDir dir("atomic_rename_fail");
  // The destination is a non-empty directory, so the rename cannot succeed.
  const auto path = dir.file("omega_calibration.cfg");
  std::filesystem::create_directories(path);
  { std::ofstream(path / "keep") << "x"; }
  std::error_code ec;

  CHECK(!OmegaCalibration::save(support::sample_center(), support::sample_range(), path, ec));
  CHECK(static_cast<bool>(ec));
  CHECK(std::filesystem::exists(path / "keep"));
  CHECK(!std::filesystem::exists(path.string() + ".tmp"));
}

TEST(unopenable_temp_leaves_previous_file_intact) {
  TempDir dir("atomic_open_fail");
  const auto path = dir.file("omega_calibration.cfg");
  std::error_code ec;
  CHECK(OmegaCalibration::save(support::sample_center(), support::sample_range(), path, ec));
  const std::string before = slurp(path);

  // A directory squatting on the temp name makes the temp file unopenable.
  std::filesystem::create_directories(path.string() + ".tmp");
  CHECK(!OmegaCalibration::save(support::sample_center(), support::sample_range(), path, ec));
  CHECK(support::is_error(ec, OmegaCalibration::Error::file_open_failed));
  CHECK_EQ(slurp(path), before);
}

TEST(save_replaces_existing_file_and_leaves_no_temp) {
  TempDir dir("atomic_replace");
  const auto path = dir.file("omega_settings.cfg");
  std::error_code ec;
  OmegaCalibration::DeviceSettings first;
  first.nudge_enabled = false;
  CHECK(OmegaCalibration::DeviceSettings::save(first, path, ec));
  OmegaCalibration::DeviceSettings second;
  second.nudge_enabled = true;
  second.input_mode = OmegaCalibration::InputMode::XAC;
  CHECK(OmegaCalibration::DeviceSettings::save(second, path, ec));
  CHECK(!std::filesystem::exists(path.string() + ".tmp"));

  OmegaCalibration::DeviceSettings loaded;
  CHECK(OmegaCalibration::DeviceSettings::load(loaded, path, ec));
  CHECK(loaded.nudge_enabled);
  CHECK(loaded.input_mode == OmegaCalibration::InputMode::XAC);
}
