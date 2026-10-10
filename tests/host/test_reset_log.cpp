// ResetLog: boot counter and reset-reason ring, persisted across boots.
// See D16 in docs/decisions.md.

#include <fstream>
#include <string>

#include "reset_log.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using espp::ResetLog;
using support::TempDir;

TEST(reset_log_starts_empty_when_file_is_missing) {
  TempDir dir("reset_log_missing");
  ResetLog log({.path = dir.file("omega_resets.log")});
  std::error_code ec;
  CHECK(log.load(ec));
  CHECK(!ec);
  CHECK_EQ(log.boot_count(), 0u);
  CHECK(log.entries().empty());
}

TEST(reset_log_record_bumps_boot_count_and_round_trips) {
  TempDir dir("reset_log_roundtrip");
  const auto path = dir.file("omega_resets.log");
  std::error_code ec;
  {
    ResetLog log({.path = path});
    CHECK(log.load(ec));
    log.record(1, "power-on", false);
    CHECK_EQ(log.boot_count(), 1u);
    CHECK(log.save(ec));
    CHECK(!ec);
  }
  {
    ResetLog log({.path = path});
    CHECK(log.load(ec));
    CHECK_EQ(log.boot_count(), 1u);
    CHECK_EQ(log.entries().size(), std::size_t{1});
    CHECK_EQ(log.entries()[0].boot, 1u);
    CHECK_EQ(log.entries()[0].reason, 1);
    CHECK(!log.entries()[0].abnormal);
    CHECK(log.entries()[0].name == "power-on");

    log.record(9, "brownout", true);
    CHECK_EQ(log.boot_count(), 2u);
    CHECK(log.save(ec));
  }
  {
    ResetLog log({.path = path});
    CHECK(log.load(ec));
    CHECK_EQ(log.boot_count(), 2u);
    CHECK_EQ(log.entries().size(), std::size_t{2});
    CHECK_EQ(log.entries()[1].boot, 2u);
    CHECK_EQ(log.entries()[1].reason, 9);
    CHECK(log.entries()[1].abnormal);
    CHECK(log.entries()[1].name == "brownout");
    CHECK_EQ(log.abnormal_count(), std::size_t{1});
  }
}

TEST(reset_log_keeps_only_the_newest_entries) {
  TempDir dir("reset_log_ring");
  ResetLog log({.path = dir.file("omega_resets.log"), .max_entries = 3});
  std::error_code ec;
  CHECK(log.load(ec));
  for (int i = 1; i <= 5; i++) {
    log.record(i, "r" + std::to_string(i), false);
  }
  CHECK_EQ(log.boot_count(), 5u);
  CHECK_EQ(log.entries().size(), std::size_t{3});
  CHECK_EQ(log.entries().front().boot, 3u);
  CHECK_EQ(log.entries().back().boot, 5u);
  CHECK(log.entries().back().name == "r5");
}

TEST(reset_log_trims_on_load_when_limit_shrinks) {
  TempDir dir("reset_log_shrink");
  const auto path = dir.file("omega_resets.log");
  std::error_code ec;
  {
    ResetLog log({.path = path, .max_entries = 8});
    for (int i = 0; i < 6; i++) {
      log.record(1, "power-on", false);
    }
    CHECK(log.save(ec));
  }
  ResetLog log({.path = path, .max_entries = 2});
  CHECK(log.load(ec));
  CHECK_EQ(log.boot_count(), 6u);
  CHECK_EQ(log.entries().size(), std::size_t{2});
  CHECK_EQ(log.entries().front().boot, 5u);
}

TEST(reset_log_skips_damaged_lines_but_keeps_the_rest) {
  TempDir dir("reset_log_damaged");
  const auto path = dir.file("omega_resets.log");
  {
    std::ofstream out(path);
    out << "boot_count=7\r\n"
        << "entry=5,4,1,panic\n"
        << "garbage without an equals sign\n"
        << "entry=not,a,number,at all\n"
        << "entry=6,4\n"
        << "unknown_key=whatever\n"
        << "entry=7,11,0,usb, with a comma in the name\n";
  }
  ResetLog log({.path = path});
  std::error_code ec;
  CHECK(log.load(ec));
  CHECK_EQ(log.boot_count(), 7u);
  CHECK_EQ(log.entries().size(), std::size_t{2});
  CHECK_EQ(log.entries()[0].boot, 5u);
  CHECK(log.entries()[0].abnormal);
  CHECK_EQ(log.entries()[1].reason, 11);
  CHECK(log.entries()[1].name == "usb, with a comma in the name");
  CHECK_EQ(log.abnormal_count(), std::size_t{1});
}

TEST(reset_log_ignores_a_bad_boot_count_line) {
  TempDir dir("reset_log_bad_count");
  const auto path = dir.file("omega_resets.log");
  {
    std::ofstream out(path);
    out << "boot_count=99999999999\n" // does not fit in 32 bits
        << "entry=1,1,0,power-on\n";
  }
  ResetLog log({.path = path});
  std::error_code ec;
  CHECK(log.load(ec));
  CHECK_EQ(log.boot_count(), 0u);
  CHECK_EQ(log.entries().size(), std::size_t{1});
}

TEST(reset_log_clear_removes_the_file_and_tolerates_absence) {
  TempDir dir("reset_log_clear");
  const auto path = dir.file("omega_resets.log");
  ResetLog log({.path = path});
  std::error_code ec;
  log.record(1, "power-on", false);
  CHECK(log.save(ec));
  CHECK(std::filesystem::exists(path));
  CHECK(log.clear(ec));
  CHECK(!std::filesystem::exists(path));
  CHECK(log.clear(ec));
  CHECK(!ec);
}
