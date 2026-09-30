#pragma once

// Shared helpers for the host tests: a fresh temp directory per test and a
// way to build / decode calibration-protocol frames without hardware.

#include <atomic>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "file_system.hpp"
#include "omega_calibration.hpp"
#include "stream_frame.hpp"

namespace support {

/// A unique, empty directory under the stubbed FileSystem root. Removed on
/// destruction.
struct TempDir {
  std::filesystem::path path;

  explicit TempDir(const std::string &label) {
    static std::atomic<int> counter{0};
    path = espp::FileSystem::get().get_root_path() /
           (label + "_" + std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path file(const char *name) const { return path / name; }
};

inline espp::OmegaCalibration::CenterResult sample_center() {
  espp::OmegaCalibration::CenterResult c;
  c.center_x = 12.5f;
  c.center_y = -3.25f;
  c.noise_radius = 0.75f;
  c.deadzone = 0.05f;
  c.manual_offset_x = 0.5f;
  c.manual_offset_y = -0.25f;
  return c;
}

inline espp::OmegaCalibration::RangeResult sample_range() {
  espp::OmegaCalibration::RangeResult r;
  r.min_x = -40.0f;
  r.max_x = 38.0f;
  r.min_y = -41.0f;
  r.max_y = 39.5f;
  r.usable_radius = 37.0f;
  r.reliable_max_radius = 36.0f;
  r.normalized_max_radius = 0.97f;
  r.deadzone = 0.02f;
  r.sector_max_radius = {1.0f, 2.0f};
  return r;
}

inline void put_f32(std::vector<uint8_t> &out, float value) {
  uint8_t bytes[4];
  std::memcpy(bytes, &value, sizeof(bytes));
  out.insert(out.end(), bytes, bytes + 4);
}

inline float get_f32(std::span<const uint8_t> data, size_t offset) {
  float value;
  std::memcpy(&value, data.data() + offset, sizeof(value));
  return value;
}

/// Decodes every complete frame in a byte buffer (e.g. everything a
/// service's send callback emitted).
inline std::vector<espp::stream_frame::Frame> decode(std::span<const uint8_t> bytes) {
  espp::stream_frame::StreamParser parser;
  return parser.feed(bytes);
}

/// Decodes exactly one frame from a buffer, or nullopt if there is not
/// exactly one.
inline std::optional<espp::stream_frame::Frame> decode_one(std::span<const uint8_t> bytes) {
  auto frames = decode(bytes);
  if (frames.size() != 1) {
    return std::nullopt;
  }
  return frames.front();
}

/// True when @p ec carries the given OmegaCalibration::Error. (The header
/// specialises std::is_error_code_enum for Error but only provides a static
/// member make_error_code, so `ec == Error::x` does not compile.)
inline bool is_error(const std::error_code &ec, espp::OmegaCalibration::Error e) {
  return ec == espp::OmegaCalibration::make_error_code(e);
}

/// The UTF-8 message that follows the u32 code in an ERROR payload.
inline std::string error_message(const espp::stream_frame::Frame &frame) {
  if (frame.payload.size() < 4) {
    return {};
  }
  return std::string(frame.payload.begin() + 4, frame.payload.end());
}

} // namespace support
