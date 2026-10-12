#include "sensor.hpp"

#include <algorithm>
#include <cmath>

namespace sim {

namespace {
constexpr double kDriftDirX = 0.8; // drift heads this way, in stick units
constexpr double kDriftDirY = 0.6;
constexpr double kLsbMT = 266.0 / 32768.0; // XY/Z HIGH range
} // namespace

const char *fault_name(Fault f) {
  switch (f) {
  case Fault::NoResponse:
    return "no_response";
  case Fault::StuckCounter:
    return "stuck_counter";
  case Fault::WeakMagnet:
    return "weak_magnet";
  case Fault::Ok:
  default:
    return "ok";
  }
}

std::optional<Fault> parse_fault(const std::string &name) {
  for (const Fault f : {Fault::Ok, Fault::NoResponse, Fault::StuckCounter, Fault::WeakMagnet}) {
    if (name == fault_name(f)) {
      return f;
    }
  }
  return std::nullopt;
}

void SensorModel::set_fault(Fault f) {
  std::lock_guard<std::mutex> lock(mu_);
  fault_ = f;
}

Fault SensorModel::fault() const {
  std::lock_guard<std::mutex> lock(mu_);
  return fault_;
}

void SensorModel::set_drift(double rate_per_min, int64_t t_ns) {
  std::lock_guard<std::mutex> lock(mu_);
  drift_base_ = drift_units_locked(t_ns);
  drift_rate_ = rate_per_min;
  drift_t0_ = t_ns;
}

void SensorModel::set_noise(double sigma) {
  std::lock_guard<std::mutex> lock(mu_);
  sigma_ = sigma;
}

double SensorModel::noise() const {
  std::lock_guard<std::mutex> lock(mu_);
  return sigma_;
}

double SensorModel::drift_units_locked(int64_t t_ns) const {
  return drift_base_ + drift_rate_ * (static_cast<double>(t_ns - drift_t0_) / 60e9);
}

double SensorModel::uniform_locked() {
  // xorshift64*: the same sequence on every platform, unlike std::normal_distribution.
  rng_ ^= rng_ >> 12;
  rng_ ^= rng_ << 25;
  rng_ ^= rng_ >> 27;
  const uint64_t v = rng_ * 0x2545F4914F6CDD1Dull;
  return static_cast<double>(v >> 11) / 9007199254740992.0;
}

double SensorModel::gaussian_locked() {
  const double u1 = std::max(uniform_locked(), 1e-12);
  const double u2 = uniform_locked();
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * 3.14159265358979323846 * u2);
}

Field SensorModel::field(const StickPos &pos, int64_t t_ns) {
  std::lock_guard<std::mutex> lock(mu_);
  const double strength = fault_ == Fault::WeakMagnet ? kWeakFactor : 1.0;
  const double drift = drift_units_locked(t_ns);
  // One LSB of dither even with noise off: a real part never returns bit-identical
  // samples, and the firmware calls identical samples a stuck sensor.
  const double dither_x = (uniform_locked() - 0.5) * 2.0 * kLsbMT;
  const double dither_y = (uniform_locked() - 0.5) * 2.0 * kLsbMT;
  const double nx = sigma_ > 0 ? gaussian_locked() * sigma_ * kMagnetMT : 0.0;
  const double ny = sigma_ > 0 ? gaussian_locked() * sigma_ * kMagnetMT : 0.0;
  Field f;
  f.bx = kCenterXmT + strength * kGainX * kMagnetMT * pos.x + drift * kDriftDirX * kMagnetMT + nx +
         dither_x;
  // Physical up (+y) is the negative field direction; see the file comment.
  f.by = kCenterYmT - strength * kGainY * kMagnetMT * pos.y + drift * kDriftDirY * kMagnetMT + ny +
         dither_y;
  f.bz = kZmT;
  return f;
}

std::pair<double, double> SensorModel::true_center_mT(int64_t t_ns) const {
  std::lock_guard<std::mutex> lock(mu_);
  const double drift = drift_units_locked(t_ns);
  return {kCenterXmT + drift * kDriftDirX * kMagnetMT, kCenterYmT + drift * kDriftDirY * kMagnetMT};
}

// ---- fake bus -------------------------------------------------------------------

FakeTmagBus::FakeTmagBus(SensorModel &model, Provider provider, FieldHook on_field)
    : model_(model)
    , provider_(std::move(provider))
    , on_field_(std::move(on_field)) {
  regs_[espp::TMAG5273::DEVICE_ID] = 0x01;
}

espp::TMAG5273::Config FakeTmagBus::config(uint8_t address) {
  espp::TMAG5273::Config cfg;
  cfg.device_address = address;
  cfg.verbosity = espp::Logger::Verbosity::ERROR;
  // The same settings main.cpp uses.
  cfg.channels = espp::TMAG5273::MagneticChannels::XYZ;
  cfg.xy_range = espp::TMAG5273::MagneticRange::HIGH;
  cfg.z_range = espp::TMAG5273::MagneticRange::HIGH;
  cfg.operating_mode = espp::TMAG5273::OperatingMode::CONTINUOUS;
  cfg.conversion_average = espp::TMAG5273::ConversionAverage::AVG_32;
  cfg.low_noise = false;
  cfg.auto_init = false;
  cfg.write = [this](uint8_t, const uint8_t *data, size_t len) {
    if (model_.fault() == Fault::NoResponse || len < 2) {
      return false;
    }
    // [register, value...] with auto-increment, like the real part.
    for (size_t i = 1; i < len; ++i) {
      const size_t r = static_cast<size_t>(data[0]) + i - 1;
      if (r < regs_.size()) {
        regs_[r] = data[i];
      }
    }
    return true;
  };
  cfg.read_register = [this](uint8_t, uint8_t reg, uint8_t *data, size_t len) {
    return read(reg, data, len);
  };
  return cfg;
}

bool FakeTmagBus::read(uint8_t reg, uint8_t *data, size_t len) {
  if (model_.fault() == Fault::NoResponse || static_cast<size_t>(reg) + len > regs_.size()) {
    return false;
  }
  const uint8_t first = espp::TMAG5273::X_MSB_RESULT;
  if (reg <= first && static_cast<size_t>(reg) + len > first) {
    convert();
  }
  for (size_t i = 0; i < len; ++i) {
    data[i] = regs_[static_cast<size_t>(reg) + i];
  }
  return true;
}

void FakeTmagBus::convert() {
  if (model_.fault() == Fault::StuckCounter) {
    return; // the conversion engine stopped: the result registers keep their last contents
  }
  const auto [pos, t_ns] = provider_();
  const Field f = model_.field(pos, t_ns);
  {
    std::lock_guard<std::mutex> lock(mu_);
    last_ = f;
  }
  if (on_field_) {
    on_field_(f);
  }
  auto raw = [](double mT) {
    const double counts = std::round(mT / 266.0 * 32768.0);
    return static_cast<uint16_t>(static_cast<int16_t>(std::clamp(counts, -32768.0, 32767.0)));
  };
  const uint16_t x = raw(f.bx), y = raw(f.by), z = raw(f.bz);
  regs_[espp::TMAG5273::X_MSB_RESULT + 0] = static_cast<uint8_t>(x >> 8);
  regs_[espp::TMAG5273::X_MSB_RESULT + 1] = static_cast<uint8_t>(x);
  regs_[espp::TMAG5273::X_MSB_RESULT + 2] = static_cast<uint8_t>(y >> 8);
  regs_[espp::TMAG5273::X_MSB_RESULT + 3] = static_cast<uint8_t>(y);
  regs_[espp::TMAG5273::X_MSB_RESULT + 4] = static_cast<uint8_t>(z >> 8);
  regs_[espp::TMAG5273::X_MSB_RESULT + 5] = static_cast<uint8_t>(z);
  counter_ = static_cast<uint8_t>((counter_ + 1) & espp::TMAG5273::STATUS_SET_COUNT_MASK);
  regs_[espp::TMAG5273::CONV_STATUS] = static_cast<uint8_t>(
      (counter_ << espp::TMAG5273::STATUS_SET_COUNT_SHIFT) | espp::TMAG5273::STATUS_RESULT_READY);
}

Field FakeTmagBus::last_field() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_;
}

} // namespace sim
