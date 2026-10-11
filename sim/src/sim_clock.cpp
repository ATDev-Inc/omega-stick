#include "sim_clock.hpp"

namespace sim {

namespace {
std::atomic<Clock *> g_clock{nullptr};
RealClock &fallback() {
  static RealClock clock(1.0);
  return clock;
}
} // namespace

Clock &current_clock() {
  Clock *c = g_clock.load();
  return c ? *c : fallback();
}

void set_current_clock(Clock *clock) { g_clock = clock; }

} // namespace sim
