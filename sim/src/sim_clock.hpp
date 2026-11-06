#pragma once

// The simulator's notion of time. Interactive mode follows the wall clock
// (times --speed); headless mode keeps a virtual clock that only moves when the
// firmware logic sleeps, so a minute of device time takes milliseconds.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace sim {

class Clock {
public:
  virtual ~Clock() = default;
  /// Simulated nanoseconds since the simulator started.
  virtual int64_t now_ns() const = 0;
  /// Lets `d` of simulated time pass.
  virtual void sleep_for(std::chrono::nanoseconds d) = 0;
  virtual bool is_virtual() const = 0;
};

class RealClock final : public Clock {
public:
  explicit RealClock(double speed)
      : speed_(speed > 0.0 ? speed : 1.0)
      , start_(std::chrono::steady_clock::now()) {}

  int64_t now_ns() const override {
    const auto elapsed = std::chrono::steady_clock::now() - start_;
    return kStartOffsetNs +
           static_cast<int64_t>(
               static_cast<double>(
                   std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) *
               speed_);
  }
  /// Sleeps until the deadline: a coarse sleep, then a yield-spin for the last
  /// stretch, so 10 ms loop periods stay 10 ms even where the OS timer is coarse.
  void sleep_for(std::chrono::nanoseconds d) override {
    using steady = std::chrono::steady_clock;
    const auto deadline =
        steady::now() +
        std::chrono::nanoseconds(static_cast<int64_t>(static_cast<double>(d.count()) / speed_));
    const auto coarse = deadline - std::chrono::milliseconds(2);
    if (steady::now() < coarse) {
      std::this_thread::sleep_until(coarse);
    }
    while (steady::now() < deadline) {
      std::this_thread::yield();
    }
  }
  bool is_virtual() const override { return false; }

private:
  // Start away from zero so no time_point is ever the "unset" default one.
  static constexpr int64_t kStartOffsetNs = 1'000'000'000;
  double speed_;
  std::chrono::steady_clock::time_point start_;
};

class VirtualClock final : public Clock {
public:
  int64_t now_ns() const override { return now_.load(); }
  void sleep_for(std::chrono::nanoseconds d) override { now_ += d.count(); }
  bool is_virtual() const override { return true; }

private:
  std::atomic<int64_t> now_{1'000'000'000};
};

/// Asks the OS for 1 ms timer resolution (a no-op off Windows).
void enable_precise_timers();

/// The clock the firmware code runs on. Set once at startup.
Clock &current_clock();
void set_current_clock(Clock *clock);

/// A std-style clock over current_clock(), for FirmwareHealthCheck<Clock>.
struct SimSteady {
  using rep = int64_t;
  using period = std::nano;
  using duration = std::chrono::nanoseconds;
  using time_point = std::chrono::time_point<SimSteady>;
  static constexpr bool is_steady = true;
  static time_point now() noexcept { return time_point(duration(current_clock().now_ns())); }
};

/// The same instant as the std::chrono::steady_clock time_point the control loop uses.
inline std::chrono::steady_clock::time_point steady_at(int64_t ns) {
  return std::chrono::steady_clock::time_point(
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::nanoseconds(ns)));
}

} // namespace sim
