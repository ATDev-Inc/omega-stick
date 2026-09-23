#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include "file_system.hpp"
#include "joystick.hpp"
#include "logger.hpp"

namespace espp {

/// Runs center + range calibration for a 2-axis analog stick (e.g. a
/// TMAG5273-based joystick) and produces the parameters needed to build an
/// espp::Joystick.
class OmegaCalibration {
public:
  // --------------------------------------------------------------------------
  // Tunables
  // --------------------------------------------------------------------------

  static constexpr int CENTER_SAMPLES = 500;
  static constexpr int RANGE_SAMPLES = 500;

  static constexpr int RANGE_SECTORS = 32;

  // Center calibration: 99% of observed center movement must fall inside the
  // deadzone.
  static constexpr float CENTER_PERCENTILE = 0.99f;

  // Add 10% margin to measured center noise.
  static constexpr float CENTER_MARGIN = 1.50f;

  // Don't allow a calibration accident to create a huge deadzone.
  static constexpr float MAX_CENTER_DEADZONE = 0.5f;

  // Use the 10th percentile of reachable sector radii as the "reliable" edge,
  // so one bad direction can't wreck the whole calibration and one unusually
  // good direction can't make us assume every direction reaches that far.
  static constexpr float RANGE_PERCENTILE = 0.10f;

  static constexpr float MAX_RANGE_DEADZONE = 0.10f;

  // Require coverage of at least half the angular sectors before trusting
  // the range calibration.
  static constexpr int MIN_COVERED_SECTORS = RANGE_SECTORS / 2;

  static constexpr auto SETTLE_DELAY = std::chrono::seconds(5);
  static constexpr auto SAMPLE_DELAY = std::chrono::milliseconds(10);

  // --------------------------------------------------------------------------
  // Types
  // --------------------------------------------------------------------------

  /// Rejects isolated single-sample outliers (an I2C glitch, a moment of EMI)
  /// from a noisy stream without touching genuine sustained values. Used
  /// during calibration sampling, and available for the runtime read stream
  /// too. A real stick motion changes smoothly across several consecutive
  /// samples; a glitch is a single spike, so the median of the last three
  /// readings passes real motion through unchanged while dropping the spike.
  class MedianFilter3 {
  public:
    float update(float value) {
      history_[index_] = value;
      index_ = (index_ + 1) % 3;
      if (count_ < 3) {
        ++count_;
        return value; // not enough history yet; pass through unfiltered
      }
      const float a = history_[0];
      const float b = history_[1];
      const float c = history_[2];
      // Median of three, without needing a full sort.
      return std::max(std::min(a, b), std::min(std::max(a, b), c));
    }

    void reset() {
      count_ = 0;
      index_ = 0;
    }

  private:
    float history_[3]{0.0f, 0.0f, 0.0f};
    int index_{0};
    int count_{0};
  };

  /// Reads the raw (uncalibrated) magnetic x/y for the joystick. Returns
  /// false if the read failed (e.g. I2C error) so the sample can be skipped.
  using ReadFunction = std::function<bool(float *x, float *y)>;

  /// Called with human-readable progress/instruction messages
  /// (e.g. "Release the joystick completely."). Defaults to using the
  /// logger at INFO level.
  using MessageFunction = std::function<void(const std::string &)>;

  struct Config {
    ReadFunction read;                 ///< Required.
    MessageFunction message = nullptr; ///< Optional; defaults to logger_.
    espp::Logger::Verbosity verbosity{espp::Logger::Verbosity::INFO};
  };

  struct CenterResult {
    float center_x{0.0f};
    float center_y{0.0f};
    float noise_radius{0.0f}; ///< 99th-percentile radius of center noise.
    float deadzone{0.0f};     ///< Normalized [0, MAX_CENTER_DEADZONE].

    // Manual per-axis trim added on top of the measured center. Intended
    // for compensating mechanical alignment issues an automatic
    // calibration sweep can't characterize by itself -- an off-center
    // magnet, a sensor that isn't perfectly perpendicular to the stick's
    // true axes, and so on -- without needing to touch the hardware.
    // Independent of, and unaffected by, recalibration or drift
    // correction; set via set_manual_offset() / adjust_manual_offset().
    // In the same raw sensor units as the calibration itself. Defaults to
    // zero (no adjustment).
    float manual_offset_x{0.0f};
    float manual_offset_y{0.0f};

    /// The center actually used when building or updating a Joystick: the
    /// measured center plus whatever manual trim has been set.
    float effective_center_x() const { return center_x + manual_offset_x; }
    float effective_center_y() const { return center_y + manual_offset_y; }
  };

  struct RangeResult {
    float min_x{0.0f}, max_x{0.0f};
    float min_y{0.0f}, max_y{0.0f};
    float usable_radius{0.0f};         ///< Conservative (weakest-axis) radius.
    float reliable_max_radius{0.0f};   ///< 10th-percentile sector radius.
    float normalized_max_radius{0.0f}; ///< reliable_max_radius / usable_radius.
    float deadzone{0.0f};              ///< Normalized [0, MAX_RANGE_DEADZONE].
    std::vector<float> sector_max_radius;
  };

  /// Errors this calibration routine can report.
  enum class Error {
    read_failure = 1,
    no_center_samples,
    no_center_noise_samples,
    no_range_samples,
    file_open_failed,
    file_write_failed,
    file_parse_failed,
    file_missing_field,
  };

  static const std::error_category &error_category() {
    static const struct Category : std::error_category {
      const char *name() const noexcept override { return "OmegaCalibration"; }
      std::string message(int ev) const override {
        switch (static_cast<Error>(ev)) {
        case Error::read_failure:
          return "sensor read failed";
        case Error::no_center_samples:
          return "no valid center samples";
        case Error::no_center_noise_samples:
          return "no valid center-noise samples";
        case Error::no_range_samples:
          return "insufficient angular coverage during range calibration";
        case Error::file_open_failed:
          return "failed to open calibration file";
        case Error::file_write_failed:
          return "failed to write calibration file";
        case Error::file_parse_failed:
          return "failed to parse calibration file";
        case Error::file_missing_field:
          return "calibration file is missing a required field";
        default:
          return "unknown error";
        }
      }
    } category;
    return category;
  }

  static std::error_code make_error_code(Error e) {
    return {static_cast<int>(e), error_category()};
  }

  // --------------------------------------------------------------------------
  // Construction
  // --------------------------------------------------------------------------

  explicit OmegaCalibration(const Config &config)
      : read_(config.read)
      , message_(config.message)
      , logger_({.tag = "OmegaCalibration", .level = config.verbosity}) {}

  // --------------------------------------------------------------------------
  // Calibration steps
  // --------------------------------------------------------------------------

  /// Measures the resting center position and center noise, and derives a
  /// center deadzone. Blocks for roughly 2 * CENTER_SAMPLES * SAMPLE_DELAY
  /// plus two SETTLE_DELAY pauses. Returns false (with ec set) on failure.
  bool center_calibration(CenterResult &out, std::error_code &ec) {
    ec.clear();

    message("Release the joystick completely.");
    message("Do not touch or move it.");
    std::this_thread::sleep_for(SETTLE_DELAY);

    // ------------------------------------------------------------------
    // Determine center
    // ------------------------------------------------------------------
    float sum_x = 0.0f;
    float sum_y = 0.0f;
    int valid_samples = 0;
    MedianFilter3 x_filter, y_filter;

    message("Measuring center...");

    for (int i = 0; i < CENTER_SAMPLES; i++) {
      float x, y;
      if (!read_(&x, &y)) {
        std::this_thread::sleep_for(SAMPLE_DELAY);
        continue;
      }
      x = x_filter.update(x);
      y = y_filter.update(y);
      sum_x += x;
      sum_y += y;
      valid_samples++;
      std::this_thread::sleep_for(SAMPLE_DELAY);
    }

    if (valid_samples == 0) {
      logger_.error("No valid center samples!");
      ec = make_error_code(Error::no_center_samples);
      return false;
    }

    const float center_x = sum_x / static_cast<float>(valid_samples);
    const float center_y = sum_y / static_cast<float>(valid_samples);

    logger_.info("Center:");
    logger_.info("  X = {:.6f}", center_x);
    logger_.info("  Y = {:.6f}", center_y);

    // ------------------------------------------------------------------
    // Measure center noise
    // ------------------------------------------------------------------
    message("Measuring center movement/noise...");
    message("shake it a bit but do not touch the end effector.");
    std::this_thread::sleep_for(SETTLE_DELAY);

    std::vector<float> center_radii;
    center_radii.reserve(CENTER_SAMPLES);
    MedianFilter3 noise_x_filter, noise_y_filter;

    for (int i = 0; i < CENTER_SAMPLES; i++) {
      float x, y;
      if (!read_(&x, &y)) {
        std::this_thread::sleep_for(SAMPLE_DELAY);
        continue;
      }
      x = noise_x_filter.update(x);
      y = noise_y_filter.update(y);
      const float dx = x - center_x;
      const float dy = y - center_y;
      center_radii.push_back(std::sqrt(dx * dx + dy * dy));
      std::this_thread::sleep_for(SAMPLE_DELAY);
    }

    if (center_radii.empty()) {
      logger_.error("No valid center-noise samples!");
      ec = make_error_code(Error::no_center_noise_samples);
      return false;
    }

    std::sort(center_radii.begin(), center_radii.end());
    const size_t percentile_index =
        static_cast<size_t>((center_radii.size() - 1) * CENTER_PERCENTILE);
    const float noise_radius = center_radii[percentile_index];

    logger_.info("Center noise:");
    logger_.info("  99th percentile radius = {:.6f}", noise_radius);

    out.center_x = center_x;
    out.center_y = center_y;
    out.noise_radius = noise_radius;
    // Deadzone gets finalized (relative to usable_radius) once range
    // calibration has run; store the raw noise radius for now and let
    // finalize_center_deadzone() compute the normalized value.
    out.deadzone = 0.0f;
    return true;
  }

  /// Sweeps the stick through its full range of motion to find axis extrema
  /// and a reliable reachable radius per angular sector, then derives a
  /// range deadzone. Requires a completed CenterResult. Blocks for roughly
  /// RANGE_SAMPLES * SAMPLE_DELAY plus a ~10s lead-in.
  bool range_calibration(const CenterResult &center, RangeResult &out, std::error_code &ec) {
    ec.clear();

    message("Move the joystick slowly through a COMPLETE circle.");
    message("Reach the physical maximum in every direction.");
    message("Try to reach all edges of the joystick.");
    std::this_thread::sleep_for(SETTLE_DELAY);
    std::this_thread::sleep_for(SETTLE_DELAY);
    message("START!");

    float min_x = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float min_y = std::numeric_limits<float>::max();
    float max_y = std::numeric_limits<float>::lowest();

    std::vector<float> sector_max_radius(RANGE_SECTORS, 0.0f);
    int valid_samples = 0;
    MedianFilter3 sweep_x_filter, sweep_y_filter;

    for (int i = 0; i < RANGE_SAMPLES; i++) {
      float x, y;
      if (!read_(&x, &y)) {
        std::this_thread::sleep_for(SAMPLE_DELAY);
        continue;
      }

      x = sweep_x_filter.update(x);
      y = sweep_y_filter.update(y);

      const float dx = x - center.center_x;
      const float dy = y - center.center_y;

      min_x = std::min(min_x, dx);
      max_x = std::max(max_x, dx);
      min_y = std::min(min_y, dy);
      max_y = std::max(max_y, dy);

      const float radius = std::sqrt(dx * dx + dy * dy);

      float angle = std::atan2(dy, dx);
      if (angle < 0.0f) {
        angle += 2.0f * static_cast<float>(M_PI);
      }

      int sector = static_cast<int>(
          angle / (2.0f * static_cast<float>(M_PI) / static_cast<float>(RANGE_SECTORS)));
      sector = std::clamp(sector, 0, RANGE_SECTORS - 1);

      sector_max_radius[sector] = std::max(sector_max_radius[sector], radius);

      valid_samples++;
      std::this_thread::sleep_for(SAMPLE_DELAY);
    }

    message("STOP!");

    if (valid_samples == 0) {
      logger_.error("No valid range samples!");
      ec = make_error_code(Error::no_range_samples);
      return false;
    }

    // ------------------------------------------------------------------
    // Usable (conservative) axis radius
    // ------------------------------------------------------------------
    const float positive_x = std::abs(max_x);
    const float negative_x = std::abs(min_x);
    const float positive_y = std::abs(max_y);
    const float negative_y = std::abs(min_y);

    const float usable_radius = std::min({positive_x, negative_x, positive_y, negative_y});

    logger_.info("Axis range relative to center:");
    logger_.info("  X: {:.6f} to +{:.6f}", min_x, max_x);
    logger_.info("  Y: {:.6f} to +{:.6f}", min_y, max_y);
    logger_.info("Usable axis radius: {:.6f}", usable_radius);

    // ------------------------------------------------------------------
    // Reliable reachable radius (10th percentile across covered sectors)
    // ------------------------------------------------------------------
    std::vector<float> valid_sector_maxima;
    for (float radius : sector_max_radius) {
      if (radius > 0.0f) {
        valid_sector_maxima.push_back(radius);
      }
    }

    std::sort(valid_sector_maxima.begin(), valid_sector_maxima.end());
    const size_t percentile_index =
        static_cast<size_t>((valid_sector_maxima.size() - 1) * RANGE_PERCENTILE);
    const float reliable_max_radius = valid_sector_maxima[percentile_index];

    float normalized_max_radius = 0.0f;
    if (usable_radius > 0.0f) {
      normalized_max_radius = reliable_max_radius / usable_radius;
    }
    normalized_max_radius = std::clamp(normalized_max_radius, 0.0f, 1.0f);

    float range_deadzone = 1.0f - normalized_max_radius;
    range_deadzone = std::clamp(range_deadzone, 0.0f, MAX_RANGE_DEADZONE);

    logger_.info("Angular sector maxima:");
    for (int i = 0; i < RANGE_SECTORS; i++) {
      logger_.info("  Sector {:2d}: {:.6f}", i, sector_max_radius[i]);
    }

    out.min_x = min_x;
    out.max_x = max_x;
    out.min_y = min_y;
    out.max_y = max_y;
    out.usable_radius = usable_radius;
    out.reliable_max_radius = reliable_max_radius;
    out.normalized_max_radius = normalized_max_radius;
    out.deadzone = range_deadzone;
    out.sector_max_radius = std::move(sector_max_radius);
    return true;
  }

  /// Sets the manual per-axis trim outright, e.g. to dial out mechanical
  /// misalignment (an off-center magnet, a sensor that's slightly rotated
  /// relative to the stick's true axes) that shows up as one direction
  /// feeling "off" even after a clean calibration sweep. offset_x/offset_y
  /// are in the same raw sensor units as the calibration itself, and are
  /// ADDED to the measured center -- a positive offset_x shifts the
  /// effective center in the positive-x direction. This is independent of
  /// center_calibration()/range_calibration() and untouched by
  /// DriftCompensator, so it survives recalibration. Call
  /// apply_calibration() afterward to push the change into a live
  /// Joystick, and save() if it should persist across reboots.
  static void set_manual_offset(CenterResult &center, float offset_x, float offset_y) {
    center.manual_offset_x = offset_x;
    center.manual_offset_y = offset_y;
  }

  /// Nudges the current manual offset by a delta rather than setting it
  /// outright -- convenient for an interactive "press left/right to trim"
  /// style adjustment, or a console command that steps the trim by a fixed
  /// increment each call.
  static void adjust_manual_offset(CenterResult &center, float delta_x, float delta_y) {
    center.manual_offset_x += delta_x;
    center.manual_offset_y += delta_y;
  }

  /// Normalizes a CenterResult's noise radius into a deadzone fraction of
  /// usable_radius. Call this after range_calibration() has produced a
  /// RangeResult, then use the updated CenterResult when building the
  /// Joystick.
  void finalize_center_deadzone(CenterResult &center, const RangeResult &range) const {
    float deadzone = 0.0f;
    if (range.usable_radius > 0.0f) {
      deadzone = (center.noise_radius * CENTER_MARGIN) / range.usable_radius;
    }
    center.deadzone = std::clamp(deadzone, 0.0f, MAX_CENTER_DEADZONE);
  }

  /// Convenience: runs center calibration, then range calibration, then
  /// finalizes the center deadzone. Returns false (with ec set) at whichever
  /// step fails first; whatever was completed is still written to center/range.
  bool calibrate(CenterResult &center, RangeResult &range, std::error_code &ec) {
    if (!center_calibration(center, ec)) {
      return false;
    }
    if (!range_calibration(center, range, ec)) {
      return false;
    }
    finalize_center_deadzone(center, range);
    return true;
  }

  /// Corrects slow center drift (thermal offset creep, mechanical settling)
  /// that accumulates while the device runs, which a one-time boot
  /// calibration can never fully account for. Call update() every loop tick
  /// with the joystick's current RAW reading and its current MAPPED output
  /// magnitude (e.g. std::hypot(js.x(), js.y())). When the mapped output
  /// stays near zero for a sustained period, that's taken as "the stick is
  /// genuinely at rest," and the stored center is nudged a small amount
  /// toward the raw reading -- slowly and rate-limited, so it can never be
  /// mistaken for responding to real input.
  ///
  /// On a nudge, update() returns true; the caller should then push the
  /// updated CenterResult into the live Joystick via apply_calibration(),
  /// and may periodically persist it via save() (not on every nudge --
  /// that would wear the flash for no benefit; e.g. once after N nudges or
  /// on a timer).
  class DriftCompensator {
  public:
    struct Config {
      float idle_magnitude_threshold{0.05f};
      std::chrono::milliseconds idle_dwell{std::chrono::seconds(2)};
      float blend_rate{0.02f};
      std::chrono::milliseconds min_nudge_interval{std::chrono::milliseconds(500)};
    };

    // 1. Default constructor uses Config's built-in default values
    DriftCompensator()
        : config_({}) {}

    // 2. Explicit constructor handles a custom configuration pass-in
    explicit DriftCompensator(const Config &config)
        : config_(config) {}

    bool update(float raw_x, float raw_y, float mapped_magnitude, CenterResult &center) {
      const auto now = std::chrono::steady_clock::now();

      if (mapped_magnitude > config_.idle_magnitude_threshold) {
        idle_since_.reset();
        return false;
      }
      if (!idle_since_) {
        idle_since_ = now;
      }
      if (now - *idle_since_ < config_.idle_dwell) {
        return false;
      }
      if (last_nudge_ && now - *last_nudge_ < config_.min_nudge_interval) {
        return false;
      }

      center.center_x += (raw_x - center.center_x) * config_.blend_rate;
      center.center_y += (raw_y - center.center_y) * config_.blend_rate;
      last_nudge_ = now;
      return true;
    }

    /// Resets idle tracking, e.g. after a manual recalibration.
    void reset() {
      idle_since_.reset();
      last_nudge_.reset();
    }

  private:
    Config config_;
    std::optional<std::chrono::steady_clock::time_point> idle_since_;
    std::optional<std::chrono::steady_clock::time_point> last_nudge_;
  };

  /// Applies completed calibration results to an already-constructed
  /// espp::Joystick -- e.g. after a fresh interactive recalibration
  /// (following clear() + calibrate()) -- without needing to destroy and
  /// rebuild the Joystick (and everything downstream of it, like the USB/HID
  /// setup).
  ///
  /// This goes through Joystick::set_calibration() rather than rebuilding a
  /// Joystick::Config from scratch, because set_calibration() (like
  /// set_type()) zeroes the per-axis FloatRangeMapper deadbands whenever the
  /// joystick's type is CIRCULAR -- which is exactly what espp::Joystick's
  /// own class documentation recommends ("it's recommended to set the
  /// individual x/y calibration deadzones to be 0 and to only use the
  /// deadzone_radius field"). Joystick's plain constructor does NOT perform
  /// this zeroing, so prefer this function (or at least call it once right
  /// after construction) instead of relying on the constructor alone.
  ///
  /// @note Does not change the joystick's type or its get_values function.
  ///       The joystick must already have been constructed with
  ///       Type::CIRCULAR (e.g. via make_joystick_config()) for the
  ///       deadband-zeroing in set_calibration() to actually trigger.
  static void apply_calibration(espp::Joystick &js, const CenterResult &center,
                                const RangeResult &range) {
    js.set_calibration(
        espp::FloatRangeMapper::Config{.center = center.effective_center_x(),
                                       .minimum = center.effective_center_x() + range.min_x,
                                       .maximum = center.effective_center_x() + range.max_x},
        espp::FloatRangeMapper::Config{.center = center.effective_center_y(),
                                       .minimum = center.effective_center_y() + range.min_y,
                                       .maximum = center.effective_center_y() + range.max_y},
        center.deadzone, range.deadzone);
  }

  /// Builds the espp::Joystick config from completed calibration results.
  /// The caller supplies get_values since it typically closes over a sensor
  /// object (e.g. a TMAG5273) that this class doesn't own.
  ///
  /// @note Use this for the initial construction of the Joystick only. To
  ///       push a new calibration onto an already-constructed Joystick (e.g.
  ///       after recalibrating at runtime), use apply_calibration() instead
  ///       -- it goes through Joystick::set_calibration(), which correctly
  ///       zeroes the per-axis deadbands for CIRCULAR joysticks, unlike
  ///       constructing a fresh Joystick::Config and assigning it directly.
  static espp::Joystick::Config
  make_joystick_config(const CenterResult &center, const RangeResult &range,
                       std::function<bool(float *, float *)> get_values) {
    return espp::Joystick::Config{
        .x_calibration = {.center = center.effective_center_x(),
                          .minimum = center.effective_center_x() + range.min_x,
                          .maximum = center.effective_center_x() + range.max_x},
        .y_calibration = {.center = center.effective_center_y(),
                          .minimum = center.effective_center_y() + range.min_y,
                          .maximum = center.effective_center_y() + range.max_y},
        .type = espp::Joystick::Type::CIRCULAR,
        .center_deadzone_radius = center.deadzone,
        .range_deadzone = range.deadzone,
        .get_values = std::move(get_values),
    };
  }

  /// Convenience: builds a path for the calibration file under espp's
  /// mounted file system root (e.g. "/littlefs/omega_calibration.cfg").
  /// Requires that espp::FileSystem's partition/label is already set up
  /// correctly (see FileSystem::get_root_path()).
  static std::filesystem::path default_path(const std::string &filename = "omega_calibration.cfg") {
    return espp::FileSystem::get().get_root_path() / filename;
  }

  /// Saves completed calibration results to a simple "key=value" text file
  /// at the given path (e.g. under espp::FileSystem::get().get_root_path()).
  /// Overwrites any existing file at that path.
  static bool save(const CenterResult &center, const RangeResult &range,
                   const std::filesystem::path &path, std::error_code &ec) {
    ec.clear();

    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
      ec = make_error_code(Error::file_open_failed);
      return false;
    }

    out << "center_x=" << center.center_x << "\n";
    out << "center_y=" << center.center_y << "\n";
    out << "center_noise_radius=" << center.noise_radius << "\n";
    out << "center_deadzone=" << center.deadzone << "\n";
    out << "center_manual_offset_x=" << center.manual_offset_x << "\n";
    out << "center_manual_offset_y=" << center.manual_offset_y << "\n";
    out << "range_min_x=" << range.min_x << "\n";
    out << "range_max_x=" << range.max_x << "\n";
    out << "range_min_y=" << range.min_y << "\n";
    out << "range_max_y=" << range.max_y << "\n";
    out << "range_usable_radius=" << range.usable_radius << "\n";
    out << "range_reliable_max_radius=" << range.reliable_max_radius << "\n";
    out << "range_normalized_max_radius=" << range.normalized_max_radius << "\n";
    out << "range_deadzone=" << range.deadzone << "\n";
    out.flush();

    if (!out) {
      ec = make_error_code(Error::file_write_failed);
      return false;
    }
    return true;
  }

  /// Loads calibration results previously written by save(). Only the
  /// fields needed to reconstruct a Joystick config are required; the
  /// diagnostic fields (noise_radius, usable_radius, etc.) are restored
  /// when present but default to 0 otherwise. Returns false (with ec set)
  /// if the file can't be opened, can't be parsed, or is missing a
  /// required field.
  static bool load(CenterResult &center, RangeResult &range, const std::filesystem::path &path,
                   std::error_code &ec) {
    ec.clear();

    std::ifstream in(path);
    if (!in.is_open()) {
      ec = make_error_code(Error::file_open_failed);
      return false;
    }

    std::unordered_map<std::string, float> values;
    std::string line;
    while (std::getline(in, line)) {
      const auto eq = line.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      const std::string key = line.substr(0, eq);
      const std::string value_str = line.substr(eq + 1);

      char *parse_end = nullptr;
      const float value = std::strtof(value_str.c_str(), &parse_end);
      if (parse_end == value_str.c_str()) {
        // No characters were consumed -> not a valid float.
        ec = make_error_code(Error::file_parse_failed);
        return false;
      }
      values[key] = value;
    }

    static const std::vector<std::string> required = {
        "center_x",    "center_y",    "center_deadzone", "range_min_x",
        "range_max_x", "range_min_y", "range_max_y",     "range_deadzone",
    };
    for (const auto &key : required) {
      if (values.find(key) == values.end()) {
        ec = make_error_code(Error::file_missing_field);
        return false;
      }
    }

    center.center_x = values["center_x"];
    center.center_y = values["center_y"];
    center.noise_radius =
        values.count("center_noise_radius") ? values["center_noise_radius"] : 0.0f;
    center.deadzone = values["center_deadzone"];
    center.manual_offset_x =
        values.count("center_manual_offset_x") ? values["center_manual_offset_x"] : 0.0f;
    center.manual_offset_y =
        values.count("center_manual_offset_y") ? values["center_manual_offset_y"] : 0.0f;

    range.min_x = values["range_min_x"];
    range.max_x = values["range_max_x"];
    range.min_y = values["range_min_y"];
    range.max_y = values["range_max_y"];
    range.usable_radius =
        values.count("range_usable_radius") ? values["range_usable_radius"] : 0.0f;
    range.reliable_max_radius =
        values.count("range_reliable_max_radius") ? values["range_reliable_max_radius"] : 0.0f;
    range.normalized_max_radius =
        values.count("range_normalized_max_radius") ? values["range_normalized_max_radius"] : 0.0f;
    range.deadzone = values["range_deadzone"];
    range.sector_max_radius.clear(); // not persisted; diagnostic only

    return true;
  }

  /// Deletes the persisted calibration file at the given path, forcing a
  /// fresh interactive calibration next time load() is tried (e.g. from a
  /// "hold button at boot to recalibrate" check, or a debug/console
  /// command). It is not an error for the file to already be missing —
  /// this returns true in that case too. Returns false (with ec set) only
  /// if the file exists but couldn't actually be removed.
  static bool clear(const std::filesystem::path &path, std::error_code &ec) {
    ec.clear();

    std::error_code exists_ec;
    if (!std::filesystem::exists(path, exists_ec)) {
      // Nothing to clear.
      return true;
    }

    if (!espp::FileSystem::get().remove(path, ec)) {
      if (!ec) {
        // FileSystem::remove() didn't set a specific error; report a
        // generic failure rather than silently claiming success.
        ec = make_error_code(Error::file_write_failed);
      }
      return false;
    }
    return true;
  }

protected:
  void message(const std::string &text) const {
    if (message_) {
      message_(text);
    } else {
      logger_.info(text);
    }
  }

  ReadFunction read_;
  MessageFunction message_;
  mutable espp::Logger logger_;
};

} // namespace espp

namespace std {
template <> struct is_error_code_enum<espp::OmegaCalibration::Error> : true_type {};
} // namespace std