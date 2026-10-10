#pragma once

// Host-test stand-in for espp/ota. Same public surface the CalibrationService
// uses, but backed by plain fields so tests can drive and inspect it.
// restart() only records that it was called (the real one never returns).

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "logger.hpp"

namespace espp {

class Ota {
public:
  struct Config {
    bool reject_same_version{false};
    espp::Logger::Verbosity log_level{espp::Logger::Verbosity::WARN};
  };

  explicit Ota(const Config &config)
      : config_(config) {}

  bool begin(size_t image_size, std::error_code &ec) {
    if (fail_next_begin) {
      fail_next_begin = false;
      ec = std::make_error_code(std::errc::io_error);
      return false;
    }
    if (active_) {
      ec = std::make_error_code(std::errc::device_or_resource_busy);
      return false;
    }
    active_ = true;
    image_size_ = image_size;
    image.clear();
    ++begin_calls;
    return true;
  }

  bool write(std::span<const uint8_t> data, std::error_code &ec) {
    if (!active_) {
      ec = std::make_error_code(std::errc::operation_not_permitted);
      return false;
    }
    if (fail_next_write) {
      fail_next_write = false;
      active_ = false; // the real engine aborts the session on a failed write
      ec = std::make_error_code(std::errc::io_error);
      return false;
    }
    image.insert(image.end(), data.begin(), data.end());
    return true;
  }

  bool finish(std::error_code &ec) {
    if (!active_) {
      ec = std::make_error_code(std::errc::operation_not_permitted);
      return false;
    }
    active_ = false;
    if (fail_next_finish) {
      fail_next_finish = false;
      ec = std::make_error_code(std::errc::bad_message);
      return false;
    }
    ++finish_calls;
    return true;
  }

  bool abort(std::error_code &) {
    active_ = false;
    ++abort_calls;
    return true;
  }

  void restart() { restart_called = true; }

  bool is_pending_verify() const { return pending_verify; }

  bool mark_app_valid(std::error_code &ec) {
    if (fail_mark_valid) {
      ec = std::make_error_code(std::errc::io_error);
      return false;
    }
    pending_verify = false;
    ++mark_valid_calls;
    return true;
  }

  bool mark_app_invalid_and_rollback(std::error_code &) {
    ++rollback_calls;
    return true;
  }

  const std::string &running_partition_label() const { return running_label; }
  const std::string &boot_partition_label() const { return boot_label; }
  bool session_active() const { return active_; }
  size_t bytes_written() const { return image.size(); }
  size_t image_size() const { return image_size_; }

  // ---- test-only controls ----
  bool pending_verify{false};
  std::string running_label{"ota_0"};
  std::string boot_label{"ota_1"};
  std::vector<uint8_t> image;
  bool fail_next_begin{false};
  bool fail_next_write{false};
  bool fail_next_finish{false};
  bool fail_mark_valid{false};
  std::atomic<bool> restart_called{false};
  int begin_calls{0};
  int finish_calls{0};
  int abort_calls{0};
  int mark_valid_calls{0};
  int rollback_calls{0};

private:
  Config config_;
  bool active_{false};
  size_t image_size_{0};
};

} // namespace espp
