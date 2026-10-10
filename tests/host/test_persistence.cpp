// OmegaCalibration::save/load and DeviceSettings::save/load round-trips.

#include <fstream>

#include "file_system.hpp"
#include "omega_calibration.hpp"
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
  std::error_code ec;
  CHECK(OmegaCalibration::DeviceSettings::load(out, path, ec));
  CHECK(out.input_mode == OmegaCalibration::InputMode::XAC);
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
  CHECK_EQ(js.calibration_updates(), 1);
  CHECK_NEAR(js.x_calibration().center, center.effective_center_x(), 1e-6);
  CHECK_NEAR(js.x_calibration().minimum, center.effective_center_x() + range.min_x, 1e-6);
  CHECK_NEAR(js.x_calibration().maximum, center.effective_center_x() + range.max_x, 1e-6);
  CHECK_NEAR(js.y_calibration().center, center.effective_center_y(), 1e-6);
  CHECK_NEAR(js.center_deadzone_radius(), 0.3f, 1e-6);
  CHECK_NEAR(js.range_deadzone(), range.deadzone, 1e-6);
}
