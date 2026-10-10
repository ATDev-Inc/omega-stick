#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "file_system.hpp"

namespace espp {

/// A boot counter and a ring of the last few reset reasons, persisted to a
/// small text file so a field failure leaves a trace even when it was not a
/// panic (a core dump covers those). Brownouts, watchdog resets and USB
/// resets otherwise vanish at the next boot. See D16 in docs/decisions.md.
///
/// The reason is stored as the integer the platform reported plus the name
/// the caller gives it, so this header needs nothing from ESP-IDF and the
/// host tests can drive it. The caller also says whether the reason counts
/// as abnormal (panic, watchdog, brownout), which lets a later health check
/// ask "has this image been crashing?" without decoding reasons itself.
///
/// Usage at boot:
///
///   espp::ResetLog log({.path = espp::ResetLog::default_path()});
///   log.load(ec);                                  // tolerant of a missing file
///   log.record(reason, name, abnormal);            // bumps the boot counter
///   log.save(ec);
///
/// File format, one entry per line, newest last:
///
///   boot_count=42
///   entry=40,9,1,brownout
///   entry=41,1,0,power-on
///
/// Unknown or damaged lines are skipped on load, so an older or hand-edited
/// file still loads and a corrupt one degrades to an empty log.
class ResetLog {
public:
  struct Entry {
    uint32_t boot{0}; ///< Boot number this reset started (1 = first boot ever)
    int reason{0};    ///< Platform reset-reason code, verbatim
    bool abnormal{false};
    std::string name; ///< Human-readable reason, as the caller named it
  };

  struct Config {
    std::filesystem::path path;
    std::size_t max_entries{16};
  };

  explicit ResetLog(const Config &config)
      : config_(config) {}

  /// Lives next to the calibration and settings files on the same
  /// filesystem; see OmegaCalibration::default_path() for the rationale.
  static std::filesystem::path default_path(const std::string &filename = "omega_resets.log") {
    return espp::FileSystem::get().get_root_path() / filename;
  }

  /// Reads the file. A missing file is not an error: the log just starts
  /// empty. Returns false only when the file exists and cannot be opened.
  bool load(std::error_code &ec) {
    ec.clear();
    boot_count_ = 0;
    entries_.clear();

    std::error_code exists_ec;
    if (!std::filesystem::exists(config_.path, exists_ec)) {
      return true;
    }
    std::ifstream in(config_.path);
    if (!in) {
      ec = std::make_error_code(std::errc::io_error);
      return false;
    }

    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      const auto eq = line.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      const std::string_view key(line.data(), eq);
      const std::string_view value(line.data() + eq + 1, line.size() - eq - 1);
      if (key == "boot_count") {
        uint32_t parsed = 0;
        if (parse_u32(value, parsed)) {
          boot_count_ = parsed;
        }
      } else if (key == "entry") {
        Entry entry;
        if (parse_entry(value, entry)) {
          entries_.push_back(std::move(entry));
        }
      }
    }
    trim();
    return true;
  }

  /// Appends this boot's reset reason and bumps the boot counter.
  void record(int reason, std::string_view name, bool abnormal) {
    boot_count_++;
    entries_.push_back(Entry{
        .boot = boot_count_, .reason = reason, .abnormal = abnormal, .name = std::string(name)});
    trim();
  }

  bool save(std::error_code &ec) const {
    ec.clear();
    std::ofstream out(config_.path, std::ios::out | std::ios::trunc);
    if (!out) {
      ec = std::make_error_code(std::errc::io_error);
      return false;
    }
    out << "boot_count=" << boot_count_ << '\n';
    for (const auto &entry : entries_) {
      // The name is last so a comma in it cannot break the fixed fields.
      out << "entry=" << entry.boot << ',' << entry.reason << ',' << (entry.abnormal ? 1 : 0) << ','
          << entry.name << '\n';
    }
    out.flush();
    if (!out) {
      ec = std::make_error_code(std::errc::io_error);
      return false;
    }
    return true;
  }

  uint32_t boot_count() const { return boot_count_; }

  /// Oldest first, newest last.
  const std::vector<Entry> &entries() const { return entries_; }

  /// How many of the retained resets the caller flagged as abnormal.
  std::size_t abnormal_count() const {
    std::size_t count = 0;
    for (const auto &entry : entries_) {
      if (entry.abnormal) {
        count++;
      }
    }
    return count;
  }

  /// Removes the file. Missing is fine.
  bool clear(std::error_code &ec) const {
    ec.clear();
    std::error_code remove_ec;
    std::filesystem::remove(config_.path, remove_ec);
    if (remove_ec) {
      ec = remove_ec;
      return false;
    }
    return true;
  }

private:
  static bool parse_u32(std::string_view text, uint32_t &out) {
    if (text.empty()) {
      return false;
    }
    uint64_t value = 0;
    for (const char c : text) {
      if (c < '0' || c > '9') {
        return false;
      }
      value = value * 10 + static_cast<uint64_t>(c - '0');
      if (value > UINT32_MAX) {
        return false;
      }
    }
    out = static_cast<uint32_t>(value);
    return true;
  }

  static bool parse_int(std::string_view text, int &out) {
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
      negative = true;
      text.remove_prefix(1);
    }
    uint32_t magnitude = 0;
    if (!parse_u32(text, magnitude) || magnitude > static_cast<uint32_t>(INT32_MAX)) {
      return false;
    }
    out = negative ? -static_cast<int>(magnitude) : static_cast<int>(magnitude);
    return true;
  }

  static bool parse_entry(std::string_view value, Entry &entry) {
    // boot,reason,abnormal,name
    const auto c1 = value.find(',');
    if (c1 == std::string_view::npos) {
      return false;
    }
    const auto c2 = value.find(',', c1 + 1);
    if (c2 == std::string_view::npos) {
      return false;
    }
    const auto c3 = value.find(',', c2 + 1);
    if (c3 == std::string_view::npos) {
      return false;
    }
    uint32_t abnormal = 0;
    if (!parse_u32(value.substr(0, c1), entry.boot) ||
        !parse_int(value.substr(c1 + 1, c2 - c1 - 1), entry.reason) ||
        !parse_u32(value.substr(c2 + 1, c3 - c2 - 1), abnormal)) {
      return false;
    }
    entry.abnormal = abnormal != 0;
    entry.name = std::string(value.substr(c3 + 1));
    return true;
  }

  void trim() {
    if (config_.max_entries == 0) {
      entries_.clear();
      return;
    }
    if (entries_.size() > config_.max_entries) {
      entries_.erase(entries_.begin(),
                     entries_.begin() +
                         static_cast<std::ptrdiff_t>(entries_.size() - config_.max_entries));
    }
  }

  Config config_;
  uint32_t boot_count_{0};
  std::vector<Entry> entries_;
};

} // namespace espp
