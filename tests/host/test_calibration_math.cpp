// OmegaCalibration measurement math: center_calibration, range_calibration,
// finalize_center_deadzone, the manual offset helpers, MedianFilter3 and the
// error category. The sensor is a deterministic fake ReadFunction and the
// settle/sample sleeps are replaced by a recorder, so the suite stays fast.
//
// Expected values come from the algorithm as documented in
// omega_calibration.hpp (percentile rules, margins, clamps), not from running
// the code and pasting its output.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include "omega_calibration.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using Cal = espp::OmegaCalibration;
using std::chrono::milliseconds;

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// Deterministic noise: a fixed LCG (Numerical Recipes constants) mapped to [-1, 1).
struct Lcg {
  std::uint32_t state;
  explicit Lcg(std::uint32_t seed)
      : state(seed) {}
  float next() {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
  }
};

/// Records what the calibration routine asked for instead of doing it.
struct Recorder {
  std::vector<milliseconds> sleeps;
  std::vector<std::string> messages;

  milliseconds total_sleep() const {
    milliseconds sum{0};
    for (const auto &s : sleeps) {
      sum += s;
    }
    return sum;
  }
  bool said(const std::string &text) const {
    for (const auto &m : messages) {
      if (m == text) {
        return true;
      }
    }
    return false;
  }
};

Cal::Config make_config(Recorder &rec, Cal::ReadFunction read) {
  return Cal::Config{.read = std::move(read),
                     .message = [&rec](const std::string &m) { rec.messages.push_back(m); },
                     .sleep = [&rec](milliseconds d) { rec.sleeps.push_back(d); },
                     .verbosity = espp::Logger::Verbosity::NONE};
}

using Point = std::pair<float, float>;

/// Replays a list of points, repeating the last one once exhausted.
Cal::ReadFunction replay(std::vector<Point> points) {
  return [points = std::move(points), i = std::size_t{0}](float *x, float *y) mutable {
    const Point &p = points[std::min(i, points.size() - 1)];
    ++i;
    *x = p.first;
    *y = p.second;
    return true;
  };
}

/// Center phase: uniform noise of +-amplitude around (cx, cy). Both
/// center_calibration phases (center, then noise) read from this.
struct NoisyCenter {
  float cx, cy, amplitude;
  Lcg rng{12345u};
  void operator()(float *x, float *y) {
    *x = cx + amplitude * rng.next();
    *y = cy + amplitude * rng.next();
  }
};

/// A sweep with one point per angular sector, at the sector's mid angle, with
/// radius radii[k]. Every point is repeated three times so the 3-sample median
/// filter returns each point unchanged. Mid-sector angles stay well away from
/// the sector boundaries.
std::vector<Point> sector_sweep(float cx, float cy, const std::vector<float> &radii) {
  std::vector<Point> pts;
  for (std::size_t k = 0; k < radii.size(); ++k) {
    const float angle = (static_cast<float>(k) + 0.5f) * 2.0f * kPi / 32.0f;
    const Point p{cx + radii[k] * std::cos(angle), cy + radii[k] * std::sin(angle)};
    for (int rep = 0; rep < 3; ++rep) {
      pts.push_back(p);
    }
  }
  return pts;
}

std::vector<float> constant_radii(int sectors, float r) {
  return std::vector<float>(static_cast<std::size_t>(sectors), r);
}

Cal::CenterResult center_at(float cx, float cy) {
  Cal::CenterResult c;
  c.center_x = cx;
  c.center_y = cy;
  return c;
}

/// Distance from a sector mid-angle to the nearest axis is half a sector
/// (pi / 32), so with a constant sector radius r every axis extreme is
/// r * cos(pi / 32).
float axis_extreme(float r) { return r * std::cos(kPi / 32.0f); }

} // namespace

// ---- MedianFilter3 ---------------------------------------------------------

TEST(median3_passes_the_first_three_samples_unfiltered) {
  Cal::MedianFilter3 f;
  CHECK_NEAR(f.update(1.0f), 1.0f, 0.0);
  CHECK_NEAR(f.update(100.0f), 100.0f, 0.0); // a spike this early cannot be rejected
  CHECK_NEAR(f.update(2.0f), 2.0f, 0.0);
}

TEST(median3_rejects_a_single_sample_spike) {
  Cal::MedianFilter3 f;
  f.update(5.0f);
  f.update(5.0f);
  f.update(5.0f);
  CHECK_NEAR(f.update(1000.0f), 5.0f, 0.0); // window {1000, 5, 5}
  CHECK_NEAR(f.update(5.0f), 5.0f, 0.0);    // window {1000, 5, 5} again
  CHECK_NEAR(f.update(5.0f), 5.0f, 0.0);    // window {5, 5, 5}
}

TEST(median3_passes_a_sustained_step_one_sample_late) {
  Cal::MedianFilter3 f;
  f.update(0.0f);
  f.update(0.0f);
  f.update(0.0f);
  CHECK_NEAR(f.update(9.0f), 0.0f, 0.0); // window {9, 0, 0}
  CHECK_NEAR(f.update(9.0f), 9.0f, 0.0); // window {9, 9, 0}
  CHECK_NEAR(f.update(9.0f), 9.0f, 0.0);
}

TEST(median3_passes_a_monotonic_ramp_delayed_by_one_sample) {
  Cal::MedianFilter3 f;
  f.update(1.0f);
  f.update(2.0f);
  f.update(3.0f);
  CHECK_NEAR(f.update(4.0f), 3.0f, 0.0); // median of {4, 2, 3}
  CHECK_NEAR(f.update(5.0f), 4.0f, 0.0); // median of {4, 5, 3}
}

TEST(median3_reset_restarts_the_passthrough_phase) {
  Cal::MedianFilter3 f;
  for (int i = 0; i < 6; ++i) {
    f.update(5.0f);
  }
  f.reset();
  CHECK_NEAR(f.update(77.0f), 77.0f, 0.0); // unfiltered again
}

// ---- error category --------------------------------------------------------

TEST(error_category_messages) {
  using E = Cal::Error;
  const auto msg = [](E e) { return Cal::make_error_code(e).message(); };
  CHECK_EQ(msg(E::read_failure), std::string("sensor read failed"));
  CHECK_EQ(msg(E::no_center_samples), std::string("no valid center samples"));
  CHECK_EQ(msg(E::no_center_noise_samples), std::string("no valid center-noise samples"));
  CHECK_EQ(msg(E::no_range_samples),
           std::string("insufficient angular coverage during range calibration"));
  CHECK_EQ(msg(E::file_open_failed), std::string("failed to open file"));
  CHECK_EQ(msg(E::file_write_failed), std::string("failed to write file"));
  CHECK_EQ(msg(E::file_parse_failed), std::string("failed to parse file"));
  CHECK_EQ(msg(E::file_missing_field), std::string("file is missing a required field"));
  CHECK_EQ(msg(E::unsupported_schema),
           std::string("file was written by newer firmware (unsupported schema)"));
  CHECK_EQ(msg(static_cast<E>(99)), std::string("unknown error"));
  CHECK_EQ(std::string(Cal::make_error_code(E::read_failure).category().name()),
           std::string("OmegaCalibration"));
}

// ---- center_calibration ----------------------------------------------------

TEST(center_calibration_finds_the_center_of_a_noisy_rest_position) {
  constexpr float noise = 0.2f;
  Recorder rec;
  NoisyCenter src{10.0f, -5.0f, noise};
  Cal cal(make_config(rec, [&src](float *x, float *y) {
    src(x, y);
    return true;
  }));

  Cal::CenterResult out;
  std::error_code ec;
  CHECK(cal.center_calibration(out, ec));
  CHECK(!ec);
  // Mean of 200 median-filtered uniform samples: sigma is about 0.01, so a
  // quarter of the noise amplitude is a very safe bound.
  CHECK_NEAR(out.center_x, 10.0f, noise * 0.25);
  CHECK_NEAR(out.center_y, -5.0f, noise * 0.25);
  // The deadzone is only finalized once the usable radius is known.
  CHECK_NEAR(out.deadzone, 0.0f, 0.0);
}

TEST(center_calibration_noise_radius_tracks_the_scatter) {
  constexpr float noise = 0.2f;
  Recorder rec;
  NoisyCenter src{10.0f, -5.0f, noise};
  Cal cal(make_config(rec, [&src](float *x, float *y) {
    src(x, y);
    return true;
  }));

  Cal::CenterResult out;
  std::error_code ec;
  CHECK(cal.center_calibration(out, ec));
  // The 99th-percentile radius can never exceed the corner of the noise square
  // (plus the small center error), and for median-of-3 uniform noise about 5%
  // of samples exceed 0.8 * amplitude on one axis alone, so the 99th
  // percentile of the radius is above that.
  CHECK(out.noise_radius > 0.8f * noise);
  CHECK(out.noise_radius < std::sqrt(2.0f) * noise + 0.05f);
}

TEST(center_calibration_of_a_motionless_stick_has_no_noise) {
  Recorder rec;
  Cal cal(make_config(rec, replay({{10.0f, -5.0f}})));
  Cal::CenterResult out;
  std::error_code ec;
  CHECK(cal.center_calibration(out, ec));
  CHECK_NEAR(out.center_x, 10.0f, 1e-5);
  CHECK_NEAR(out.center_y, -5.0f, 1e-5);
  CHECK_NEAR(out.noise_radius, 0.0f, 1e-5);
}

TEST(center_calibration_noise_radius_uses_the_99th_percentile_index) {
  // Phase 1 (center) is constant at (10, -5). Phase 2 ramps the offset from the
  // center along a 3-4-5 direction: sample i sits at radius m_i = 0.01 * i
  // (0 .. 1.99). A median of three on a monotonic ramp returns the middle
  // sample, so after the three pass-through samples the filtered radius for
  // sample i is m_{i-1}. The sorted list therefore has m_{k-1} at index k
  // (k >= 3), and the documented index is floor((200 - 1) * 0.99) = 197,
  // giving m_196 = 1.96. Using 200 * 0.99 = 198 would give 1.97 instead.
  Recorder rec;
  int n = 0;
  Cal cal(make_config(rec, [&n](float *x, float *y) {
    const int i = n++;
    if (i < Cal::CENTER_SAMPLES) {
      *x = 10.0f;
      *y = -5.0f;
    } else {
      const float m = 0.01f * static_cast<float>(i - Cal::CENTER_SAMPLES);
      *x = 10.0f + 0.6f * m;
      *y = -5.0f + 0.8f * m;
    }
    return true;
  }));

  Cal::CenterResult out;
  std::error_code ec;
  CHECK(cal.center_calibration(out, ec));
  CHECK_NEAR(out.center_x, 10.0f, 1e-5);
  CHECK_NEAR(out.noise_radius, 1.96f, 0.004);
}

TEST(center_calibration_skips_failed_reads) {
  constexpr float noise = 0.2f;
  Recorder rec;
  NoisyCenter src{10.0f, -5.0f, noise};
  int calls = 0;
  Cal cal(make_config(rec, [&](float *x, float *y) {
    if (++calls % 5 == 0) {
      return false; // every fifth read glitches
    }
    src(x, y);
    return true;
  }));

  Cal::CenterResult out;
  std::error_code ec;
  CHECK(cal.center_calibration(out, ec));
  CHECK_NEAR(out.center_x, 10.0f, noise * 0.25);
  CHECK_NEAR(out.center_y, -5.0f, noise * 0.25);
  // A failed read still waits one sample period before retrying.
  CHECK_EQ(rec.sleeps.size(), std::size_t{2 + 2 * Cal::CENTER_SAMPLES});
}

TEST(center_calibration_fails_when_no_center_sample_reads) {
  Recorder rec;
  Cal cal(make_config(rec, [](float *, float *) { return false; }));
  Cal::CenterResult out = center_at(99.0f, 99.0f);
  std::error_code ec;
  CHECK(!cal.center_calibration(out, ec));
  CHECK(support::is_error(ec, Cal::Error::no_center_samples));
  CHECK_NEAR(out.center_x, 99.0f, 0.0); // untouched on failure
}

TEST(center_calibration_fails_when_the_noise_phase_cannot_read) {
  Recorder rec;
  int n = 0;
  Cal cal(make_config(rec, [&n](float *x, float *y) {
    if (n++ >= Cal::CENTER_SAMPLES) {
      return false; // the center phase succeeds, the noise phase never does
    }
    *x = 1.0f;
    *y = 2.0f;
    return true;
  }));
  Cal::CenterResult out = center_at(99.0f, 99.0f);
  std::error_code ec;
  CHECK(!cal.center_calibration(out, ec));
  CHECK(support::is_error(ec, Cal::Error::no_center_noise_samples));
  CHECK_NEAR(out.center_x, 99.0f, 0.0);
}

TEST(center_calibration_clears_a_stale_error_code_on_success) {
  Recorder rec;
  Cal cal(make_config(rec, replay({{1.0f, 1.0f}})));
  Cal::CenterResult out;
  std::error_code ec = Cal::make_error_code(Cal::Error::read_failure);
  CHECK(cal.center_calibration(out, ec));
  CHECK(!ec);
}

TEST(center_calibration_sleep_schedule_matches_the_documented_duration) {
  // Documented: two SETTLE_DELAY pauses plus 2 * CENTER_SAMPLES * SAMPLE_DELAY.
  Recorder rec;
  Cal cal(make_config(rec, replay({{1.0f, 1.0f}})));
  Cal::CenterResult out;
  std::error_code ec;
  CHECK(cal.center_calibration(out, ec));
  CHECK_EQ(rec.sleeps.front(), milliseconds(5000));
  CHECK_EQ(rec.total_sleep(), milliseconds(2 * 5000 + 2 * 200 * 10));
  CHECK(rec.said("Measuring center..."));
}

// ---- range_calibration -----------------------------------------------------

TEST(range_calibration_of_a_full_circle_reports_the_radius) {
  constexpr float R = 30.0f;
  Recorder rec;
  Cal cal(make_config(rec, replay(sector_sweep(10.0f, -5.0f, constant_radii(32, R)))));

  Cal::RangeResult range;
  std::error_code ec;
  CHECK(cal.range_calibration(center_at(10.0f, -5.0f), range, ec));
  CHECK(!ec);

  const float edge = axis_extreme(R);
  CHECK_NEAR(range.max_x, edge, 1e-3);
  CHECK_NEAR(range.min_x, -edge, 1e-3);
  CHECK_NEAR(range.max_y, edge, 1e-3);
  CHECK_NEAR(range.min_y, -edge, 1e-3);
  // The usable radius is the weakest axis extreme.
  CHECK_NEAR(range.usable_radius, edge, 1e-3);
  CHECK_NEAR(range.reliable_max_radius, R, 1e-3);
  // reliable (30) is larger than usable (29.86): the ratio clamps to 1 and the
  // range deadzone is therefore 0.
  CHECK_NEAR(range.normalized_max_radius, 1.0f, 0.0);
  CHECK_NEAR(range.deadzone, 0.0f, 0.0);
  CHECK_EQ(range.sector_max_radius.size(), std::size_t{32});
  for (float r : range.sector_max_radius) {
    CHECK_NEAR(r, R, 1e-3);
  }
}

TEST(range_calibration_reliable_radius_is_the_tenth_percentile_of_sectors) {
  // Four weak sectors (3..6) at radius 28, the other 28 at radius 30. The
  // sorted covered-sector maxima are [28 x4, 30 x28]; the documented index is
  // floor((32 - 1) * 0.10) = 3, which is the last of the weak sectors: 28.
  // The axis extremes come from sectors 0/31, 7/8, 15/16 and 23/24, all radius 30.
  std::vector<float> radii = constant_radii(32, 30.0f);
  for (std::size_t k = 3; k <= 6; ++k) {
    radii[k] = 28.0f;
  }
  Recorder rec;
  Cal cal(make_config(rec, replay(sector_sweep(0.0f, 0.0f, radii))));

  Cal::RangeResult range;
  std::error_code ec;
  CHECK(cal.range_calibration(center_at(0.0f, 0.0f), range, ec));

  const float usable = axis_extreme(30.0f);
  CHECK_NEAR(range.usable_radius, usable, 1e-3);
  CHECK_NEAR(range.reliable_max_radius, 28.0f, 1e-3);
  CHECK_NEAR(range.normalized_max_radius, 28.0f / usable, 1e-4);
  CHECK_NEAR(range.deadzone, 1.0f - 28.0f / usable, 1e-4); // about 0.062, below the clamp
}

TEST(range_calibration_one_weak_direction_does_not_move_the_reliable_radius) {
  // The point of using the 10th percentile: a single dead direction (sector 5
  // at radius 5) is index 0 of the sorted list, and index 3 is still 30.
  std::vector<float> radii = constant_radii(32, 30.0f);
  radii[5] = 5.0f;
  Recorder rec;
  Cal cal(make_config(rec, replay(sector_sweep(0.0f, 0.0f, radii))));

  Cal::RangeResult range;
  std::error_code ec;
  CHECK(cal.range_calibration(center_at(0.0f, 0.0f), range, ec));
  CHECK_NEAR(range.reliable_max_radius, 30.0f, 1e-3);
  CHECK_NEAR(range.deadzone, 0.0f, 0.0);
}

TEST(range_calibration_deadzone_is_clamped_to_the_maximum) {
  // Sectors 0..3 only reach 15, so reliable = 15 and 1 - 15 / 29.86 = 0.50,
  // far above MAX_RANGE_DEADZONE.
  std::vector<float> radii = constant_radii(32, 30.0f);
  for (std::size_t k = 0; k <= 3; ++k) {
    radii[k] = 15.0f;
  }
  Recorder rec;
  Cal cal(make_config(rec, replay(sector_sweep(0.0f, 0.0f, radii))));

  Cal::RangeResult range;
  std::error_code ec;
  CHECK(cal.range_calibration(center_at(0.0f, 0.0f), range, ec));
  CHECK_NEAR(range.reliable_max_radius, 15.0f, 1e-3);
  CHECK(range.normalized_max_radius < 0.6f);
  CHECK_NEAR(range.deadzone, Cal::MAX_RANGE_DEADZONE, 0.0);
}

TEST(range_calibration_measures_relative_to_the_given_center) {
  // The same circle around two different centers gives the same result.
  Recorder rec;
  Cal a(make_config(rec, replay(sector_sweep(0.0f, 0.0f, constant_radii(32, 20.0f)))));
  Cal b(make_config(rec, replay(sector_sweep(-7.0f, 13.0f, constant_radii(32, 20.0f)))));
  Cal::RangeResult ra, rb;
  std::error_code ec;
  CHECK(a.range_calibration(center_at(0.0f, 0.0f), ra, ec));
  CHECK(b.range_calibration(center_at(-7.0f, 13.0f), rb, ec));
  CHECK_NEAR(ra.usable_radius, rb.usable_radius, 1e-3);
  CHECK_NEAR(ra.max_x, rb.max_x, 1e-3);
  CHECK_NEAR(ra.min_y, rb.min_y, 1e-3);
}

TEST(range_calibration_fails_when_no_sample_reads) {
  Recorder rec;
  Cal cal(make_config(rec, [](float *, float *) { return false; }));
  Cal::RangeResult range;
  range.usable_radius = 99.0f;
  std::error_code ec;
  CHECK(!cal.range_calibration(center_at(0.0f, 0.0f), range, ec));
  CHECK(support::is_error(ec, Cal::Error::no_range_samples));
  CHECK_NEAR(range.usable_radius, 99.0f, 0.0);
}

TEST(range_calibration_accepts_exactly_half_the_sectors) {
  // MIN_COVERED_SECTORS = 16: sectors 0..15 is the upper half circle.
  Recorder rec;
  Cal cal(make_config(rec, replay(sector_sweep(0.0f, 0.0f, constant_radii(16, 30.0f)))));
  Cal::RangeResult range;
  std::error_code ec;
  CHECK(cal.range_calibration(center_at(0.0f, 0.0f), range, ec));
}

TEST(range_calibration_rejects_insufficient_sector_coverage) {
  Recorder rec;
  Cal cal(make_config(rec, replay(sector_sweep(0.0f, 0.0f, constant_radii(8, 30.0f)))));
  Cal::RangeResult range;
  std::error_code ec;
  CHECK(!cal.range_calibration(center_at(0.0f, 0.0f), range, ec));
  CHECK(support::is_error(ec, Cal::Error::no_range_samples));
}

TEST(range_calibration_sleep_schedule_matches_the_documented_duration) {
  Recorder rec;
  Cal cal(make_config(rec, replay(sector_sweep(0.0f, 0.0f, constant_radii(32, 30.0f)))));
  Cal::RangeResult range;
  std::error_code ec;
  CHECK(cal.range_calibration(center_at(0.0f, 0.0f), range, ec));
  // Two SETTLE_DELAY lead-in pauses, then one SAMPLE_DELAY per sample.
  CHECK_EQ(rec.total_sleep(), milliseconds(2 * 5000 + 200 * 10));
  const auto start = [&] {
    for (std::size_t i = 0; i < rec.messages.size(); ++i) {
      if (rec.messages[i] == "START!") {
        return i;
      }
    }
    return rec.messages.size();
  }();
  CHECK(start + 2 == rec.messages.size()); // START!, then STOP!
  CHECK(rec.said("STOP!"));
}

// ---- finalize_center_deadzone ----------------------------------------------

TEST(finalize_center_deadzone_applies_the_margin_over_the_usable_radius) {
  Recorder rec;
  Cal cal(make_config(rec, replay({{0.0f, 0.0f}})));
  Cal::CenterResult center;
  center.noise_radius = 0.75f;
  Cal::RangeResult range;
  range.usable_radius = 37.0f;
  cal.finalize_center_deadzone(center, range);
  // deadzone * usable_radius is the noise radius widened by CENTER_MARGIN.
  CHECK_NEAR(center.deadzone * range.usable_radius, 0.75f * Cal::CENTER_MARGIN, 1e-4);
  CHECK_NEAR(center.deadzone, 0.75f * 1.5f / 37.0f, 1e-6);
}

TEST(finalize_center_deadzone_clamps_to_the_maximum) {
  Recorder rec;
  Cal cal(make_config(rec, replay({{0.0f, 0.0f}})));
  Cal::CenterResult center;
  center.noise_radius = 100.0f;
  Cal::RangeResult range;
  range.usable_radius = 10.0f; // 100 * 1.5 / 10 = 15
  cal.finalize_center_deadzone(center, range);
  CHECK_NEAR(center.deadzone, Cal::MAX_CENTER_DEADZONE, 0.0);
}

TEST(finalize_center_deadzone_with_no_usable_radius_is_zero) {
  Recorder rec;
  Cal cal(make_config(rec, replay({{0.0f, 0.0f}})));
  Cal::CenterResult center;
  center.noise_radius = 5.0f;
  center.deadzone = 0.3f;
  Cal::RangeResult range; // usable_radius defaults to 0
  cal.finalize_center_deadzone(center, range);
  CHECK_NEAR(center.deadzone, 0.0f, 0.0);
}

TEST(finalize_center_deadzone_is_idempotent) {
  Recorder rec;
  Cal cal(make_config(rec, replay({{0.0f, 0.0f}})));
  Cal::CenterResult center;
  center.noise_radius = 0.4f;
  Cal::RangeResult range;
  range.usable_radius = 20.0f;
  cal.finalize_center_deadzone(center, range);
  const float first = center.deadzone;
  cal.finalize_center_deadzone(center, range);
  CHECK_NEAR(center.deadzone, first, 0.0);
}

// ---- manual offsets --------------------------------------------------------

TEST(manual_offset_set_and_adjust_shift_the_effective_center_only) {
  Cal::CenterResult c = center_at(10.0f, -5.0f);
  CHECK_NEAR(c.effective_center_x(), 10.0f, 0.0);
  CHECK_NEAR(c.effective_center_y(), -5.0f, 0.0);

  Cal::set_manual_offset(c, 0.5f, -0.25f);
  CHECK_NEAR(c.effective_center_x(), 10.5f, 1e-6);
  CHECK_NEAR(c.effective_center_y(), -5.25f, 1e-6);
  CHECK_NEAR(c.center_x, 10.0f, 0.0); // the measured center is untouched

  Cal::adjust_manual_offset(c, 0.5f, 0.25f);
  CHECK_NEAR(c.manual_offset_x, 1.0f, 1e-6);
  CHECK_NEAR(c.manual_offset_y, 0.0f, 1e-6);
  CHECK_NEAR(c.effective_center_x(), 11.0f, 1e-6);

  Cal::set_manual_offset(c, 0.0f, 0.0f); // set replaces, it does not accumulate
  CHECK_NEAR(c.effective_center_x(), 10.0f, 0.0);
}

// ---- calibrate() end to end ------------------------------------------------

namespace {

/// 400 reads of noise around (cx, cy) for the center step, then the sweep.
Cal::ReadFunction center_then_sweep(float cx, float cy, float amplitude, std::vector<Point> sweep) {
  return [src = NoisyCenter{cx, cy, amplitude}, sweep = std::move(sweep), n = 0](float *x,
                                                                                 float *y) mutable {
    const int i = n++;
    if (i < 2 * Cal::CENTER_SAMPLES) {
      src(x, y);
    } else {
      const std::size_t j = static_cast<std::size_t>(i - 2 * Cal::CENTER_SAMPLES);
      const Point &p = sweep[std::min(j, sweep.size() - 1)];
      *x = p.first;
      *y = p.second;
    }
    return true;
  };
}

} // namespace

TEST(calibrate_runs_center_then_range_then_finalizes_the_deadzone) {
  constexpr float noise = 0.2f;
  Recorder rec;
  Cal cal(
      make_config(rec, center_then_sweep(10.0f, -5.0f, noise,
                                         sector_sweep(10.0f, -5.0f, constant_radii(32, 30.0f)))));
  Cal::CenterResult center;
  Cal::RangeResult range;
  std::error_code ec;
  CHECK(cal.calibrate(center, range, ec));

  CHECK_NEAR(center.center_x, 10.0f, noise * 0.25);
  CHECK_NEAR(center.center_y, -5.0f, noise * 0.25);
  // The sweep is a circle around the true center, so the measured center
  // error leaves the usable radius within that error of the ideal.
  CHECK_NEAR(range.usable_radius, axis_extreme(30.0f), noise * 0.25 + 1e-3);
  CHECK(center.noise_radius > 0.0f);
  CHECK_NEAR(center.deadzone, center.noise_radius * Cal::CENTER_MARGIN / range.usable_radius, 1e-6);
  CHECK(center.deadzone < Cal::MAX_CENTER_DEADZONE);
}

TEST(calibrate_clamps_the_center_deadzone_when_the_noise_is_huge) {
  constexpr float noise = 50.0f; // bigger than the stick's whole travel
  Recorder rec;
  Cal cal(make_config(rec, center_then_sweep(0.0f, 0.0f, noise,
                                             sector_sweep(0.0f, 0.0f, constant_radii(32, 30.0f)))));
  Cal::CenterResult center;
  Cal::RangeResult range;
  std::error_code ec;
  CHECK(cal.calibrate(center, range, ec));
  CHECK(center.noise_radius > 0.8f * noise);
  CHECK_NEAR(center.deadzone, Cal::MAX_CENTER_DEADZONE, 0.0);
}

TEST(calibrate_keeps_the_manual_offset_across_recalibration) {
  Recorder rec;
  Cal cal(make_config(rec, center_then_sweep(0.0f, 0.0f, 0.1f,
                                             sector_sweep(0.0f, 0.0f, constant_radii(32, 30.0f)))));
  Cal::CenterResult center;
  Cal::set_manual_offset(center, 1.5f, -2.5f);
  Cal::RangeResult range;
  std::error_code ec;
  CHECK(cal.calibrate(center, range, ec));
  CHECK_NEAR(center.manual_offset_x, 1.5f, 0.0);
  CHECK_NEAR(center.manual_offset_y, -2.5f, 0.0);
}

TEST(calibrate_stops_at_the_first_failing_step) {
  Recorder rec;
  int reads = 0;
  Cal cal(make_config(rec, [&reads](float *, float *) {
    ++reads;
    return false;
  }));
  Cal::CenterResult center;
  Cal::RangeResult range;
  range.usable_radius = 99.0f;
  std::error_code ec;
  CHECK(!cal.calibrate(center, range, ec));
  CHECK(support::is_error(ec, Cal::Error::no_center_samples));
  CHECK_EQ(reads, Cal::CENTER_SAMPLES); // range calibration never ran
  CHECK_NEAR(range.usable_radius, 99.0f, 0.0);
}
