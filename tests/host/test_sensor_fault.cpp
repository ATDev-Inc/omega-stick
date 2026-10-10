// SensorFaultMonitor: when a sensor fault trips, when it clears, and what is
// latched for status reporting. See D21 in docs/decisions.md.

#include "omega_calibration.hpp"
#include "test_harness.hpp"

using Monitor = espp::OmegaCalibration::SensorFaultMonitor;
using Fault = Monitor::Fault;

namespace {

// A healthy sensor at rest: successful reads whose value wobbles by a count.
bool feed_noisy(Monitor &m, int samples, float base = 100.0f) {
  bool faulted = false;
  for (int i = 0; i < samples; i++) {
    const float wobble = (i % 2 == 0) ? 0.0f : 1.0f;
    faulted = m.update(true, base + wobble, base - wobble);
  }
  return faulted;
}

} // namespace

TEST(fault_monitor_starts_healthy) {
  Monitor m;
  CHECK(!m.faulted());
  CHECK(m.fault() == Fault::NONE);
  CHECK(m.first_fault() == Fault::NONE);
  CHECK_EQ(m.fault_count(), 0u);
  CHECK(!feed_noisy(m, 1000));
}

TEST(fault_monitor_ignores_a_single_failed_read) {
  Monitor m({.trip_after_failed_reads = 3});
  feed_noisy(m, 5);
  CHECK(!m.update(false, 0, 0));
  CHECK(!m.update(true, 100, 100));
  CHECK(!m.update(false, 0, 0));
  CHECK(!m.update(true, 101, 99));
  CHECK(!m.faulted());
  CHECK(m.first_fault() == Fault::NONE);
}

TEST(fault_monitor_trips_on_consecutive_failed_reads) {
  Monitor m({.trip_after_failed_reads = 3});
  feed_noisy(m, 5);
  CHECK(!m.update(false, 0, 0));
  CHECK(!m.update(false, 0, 0));
  CHECK(m.update(false, 0, 0));
  CHECK(m.fault() == Fault::READ_FAILED);
  CHECK(m.first_fault() == Fault::READ_FAILED);
  CHECK_EQ(m.fault_count(), 1u);
  // Still faulted while the reads keep failing, and the count does not grow.
  CHECK(m.update(false, 0, 0));
  CHECK_EQ(m.fault_count(), 1u);
}

TEST(fault_monitor_recovers_after_good_reads_only) {
  Monitor m({.trip_after_failed_reads = 3, .recover_after_good_reads = 10});
  for (int i = 0; i < 3; i++) {
    m.update(false, 0, 0);
  }
  CHECK(m.faulted());
  // Nine good reads are not enough.
  CHECK(feed_noisy(m, 9));
  // A failure in the middle restarts the count.
  CHECK(m.update(false, 0, 0));
  CHECK(feed_noisy(m, 9));
  CHECK(m.faulted());
  CHECK(!feed_noisy(m, 1));
  CHECK(m.fault() == Fault::NONE);
  // The latch survives recovery.
  CHECK(m.first_fault() == Fault::READ_FAILED);
  CHECK_EQ(m.fault_count(), 1u);
}

TEST(fault_monitor_trips_when_reading_never_changes) {
  Monitor m({.stuck_after_samples = 50, .recover_after_good_reads = 10});
  feed_noisy(m, 5);
  // 50 identical samples after the first one: streak reaches 50 on the 51st.
  for (int i = 0; i < 50; i++) {
    CHECK(!m.update(true, 42.0f, -7.0f));
  }
  CHECK(m.update(true, 42.0f, -7.0f));
  CHECK(m.fault() == Fault::STUCK);
  CHECK(m.first_fault() == Fault::STUCK);
  CHECK_EQ(m.fault_count(), 1u);
}

TEST(fault_monitor_stuck_needs_both_axes_identical) {
  Monitor m({.stuck_after_samples = 20});
  for (int i = 0; i < 200; i++) {
    // X frozen, Y wobbling: the conversion engine is clearly running.
    CHECK(!m.update(true, 42.0f, (i % 2) ? 1.0f : 0.0f));
  }
  CHECK(!m.faulted());
}

TEST(fault_monitor_stuck_recovers_only_after_reading_changes) {
  Monitor m({.stuck_after_samples = 20, .recover_after_good_reads = 10});
  for (int i = 0; i < 25; i++) {
    m.update(true, 42.0f, -7.0f);
  }
  CHECK(m.fault() == Fault::STUCK);
  // More identical samples keep it faulted no matter how many.
  for (int i = 0; i < 100; i++) {
    CHECK(m.update(true, 42.0f, -7.0f));
  }
  // Once the value moves, recovery still waits for the good-read count.
  CHECK(feed_noisy(m, 9));
  CHECK(!feed_noisy(m, 1));
  CHECK(m.fault() == Fault::NONE);
  CHECK_EQ(m.fault_count(), 1u);
}

TEST(fault_monitor_stuck_check_can_be_disabled) {
  Monitor m({.stuck_after_samples = 0});
  for (int i = 0; i < 5000; i++) {
    CHECK(!m.update(true, 42.0f, -7.0f));
  }
  CHECK(!m.faulted());
}

TEST(fault_monitor_counts_each_distinct_trip) {
  Monitor m(
      {.trip_after_failed_reads = 2, .stuck_after_samples = 10, .recover_after_good_reads = 2});
  m.update(false, 0, 0);
  m.update(false, 0, 0);
  CHECK(m.fault() == Fault::READ_FAILED);
  feed_noisy(m, 2);
  CHECK(!m.faulted());
  for (int i = 0; i < 12; i++) {
    m.update(true, 1.0f, 1.0f);
  }
  CHECK(m.fault() == Fault::STUCK);
  CHECK_EQ(m.fault_count(), 2u);
  CHECK(m.first_fault() == Fault::READ_FAILED);
}

TEST(fault_monitor_reset_forgets_everything) {
  Monitor m({.trip_after_failed_reads = 2});
  m.update(false, 0, 0);
  m.update(false, 0, 0);
  CHECK(m.faulted());
  m.reset();
  CHECK(!m.faulted());
  CHECK(m.first_fault() == Fault::NONE);
  CHECK_EQ(m.fault_count(), 0u);
  // The streak was cleared too: one more failure is not a trip.
  CHECK(!m.update(false, 0, 0));
}

TEST(fault_monitor_names_faults) {
  CHECK(std::string(Monitor::to_string(Fault::NONE)) == "none");
  CHECK(std::string(Monitor::to_string(Fault::READ_FAILED)) == "read failed");
  CHECK(std::string(Monitor::to_string(Fault::STUCK)) == "reading stuck");
}
