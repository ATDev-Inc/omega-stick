#pragma once

// The simulated sensor: a field model (stick position -> mT at the TMAG5273) and a
// fake I2C bus that presents it through the chip's register map, so the real
// espp::TMAG5273 driver reads it exactly as it reads the part.
//
// Axes: the stick is x right, y up, both -1..1. The firmware's own mapping makes a
// NEGATIVE joystick y mean "forward"/"up" (negative mouse dy), so the model puts
// physical up on the negative field-y direction: pushing the stick up moves the
// cursor up.

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "tmag5273.hpp"

namespace sim {

enum class Fault { Ok, NoResponse, StuckCounter, WeakMagnet };

const char *fault_name(Fault f);
std::optional<Fault> parse_fault(const std::string &name);

struct StickPos {
  double x{0}, y{0};
};

struct Field {
  double bx{0}, by{0}, bz{0}; ///< mT
};

class SensorModel {
public:
  /// Field produced by a full-deflection stick, in mT.
  static constexpr double kMagnetMT = 40.0;
  /// Resting field offsets (the "true center"), in mT.
  static constexpr double kCenterXmT = 2.4;
  static constexpr double kCenterYmT = -1.9;
  static constexpr double kZmT = 20.0;
  static constexpr double kGainX = 1.0;
  static constexpr double kGainY = 0.92;
  static constexpr double kWeakFactor = 0.15;

  void set_fault(Fault f);
  Fault fault() const;
  /// Center drift in stick units per minute, from time t_ns on.
  void set_drift(double rate_per_min, int64_t t_ns);
  void set_noise(double sigma);
  double noise() const;

  /// The field for a stick position at time t_ns (one new conversion).
  Field field(StickPos pos, int64_t t_ns);

  /// The resting field the calibration should find at time t_ns (includes drift).
  std::pair<double, double> true_center_mT(int64_t t_ns) const;
  /// mT per unit of stick travel on each axis (what a perfect calibration reports).
  static double mT_per_unit_x() { return kMagnetMT * kGainX; }
  static double mT_per_unit_y() { return kMagnetMT * kGainY; }

private:
  double drift_units_locked(int64_t t_ns) const;
  double uniform_locked(); ///< [0, 1)
  double gaussian_locked();

  mutable std::mutex mu_;
  Fault fault_{Fault::Ok};
  double drift_rate_{0};
  double drift_base_{0};
  int64_t drift_t0_{0};
  double sigma_{0.002};
  uint64_t rng_{0x9E3779B97F4A7C15ull};
};

/// A TMAG5273 on a fake I2C bus. Not movable: the callbacks capture `this`.
class FakeTmagBus {
public:
  using Provider = std::function<std::pair<StickPos, int64_t>()>; ///< stick position, time (ns)

  FakeTmagBus(SensorModel &model, Provider provider);
  FakeTmagBus(const FakeTmagBus &) = delete;
  FakeTmagBus &operator=(const FakeTmagBus &) = delete;

  /// Driver configuration wired to this bus, the same as main.cpp's.
  espp::TMAG5273::Config config(uint8_t address);

  /// The last field a read produced (for telemetry), in mT.
  Field last_field() const;

private:
  bool read(uint8_t reg, uint8_t *data, size_t len);
  void convert();

  SensorModel &model_;
  Provider provider_;
  std::array<uint8_t, 256> regs_{};
  uint8_t counter_{0};
  mutable std::mutex mu_;
  Field last_{};
};

} // namespace sim
