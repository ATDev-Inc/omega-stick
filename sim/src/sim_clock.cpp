#include "sim_clock.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <timeapi.h> // after windows.h
// clang-format on
#endif

namespace sim {

namespace {
std::atomic<Clock *> g_clock{nullptr};
RealClock &fallback() {
  static RealClock clock(1.0);
  return clock;
}
} // namespace

void enable_precise_timers() {
#if defined(_WIN32)
  timeBeginPeriod(1);
#endif
}

Clock &current_clock() {
  if (Clock *c = g_clock.load()) {
    return *c;
  }
  return fallback();
}

void set_current_clock(Clock *clock) { g_clock = clock; }

} // namespace sim
