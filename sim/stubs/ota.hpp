#pragma once

// Simulator stand-in for espp/ota: the same public surface CalibrationService and
// FirmwareHealthCheck use, backed by the two-slot fake in sim_flash.hpp instead
// of the ESP-IDF OTA APIs.
//
//  - begin() is refused while the running image is pending verification, as
//    esp_ota_begin() refuses it, and otherwise erases the slot that is not
//    running; write() leaves it "partial" once the app descriptor has arrived,
//    so an aborted or expired upload cannot be reverted to.
//  - finish() validates the uploaded image the way esp_ota_end() does (image
//    magic and app descriptor), keeps its descriptor in the slot that is not
//    running and makes that slot the boot slot.
//  - restart() asks the simulator to restart (it cannot unwind; it runs on the
//    thread CalibrationService detaches after the reply flush delay).
//  - mark_app_invalid_and_rollback() never returns on success, like the real one:
//    it throws sim::RestartRequest, which the simulated firmware's top level
//    turns into the restart.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "logger.hpp"
#include "sim_flash.hpp"

namespace sim {

/// Thrown by calls that restart the device on the real hardware and never return.
struct RestartRequest {
  std::string reason;
};

} // namespace sim

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
    if (active_) {
      ec = std::make_error_code(std::errc::device_or_resource_busy);
      return false;
    }
    if (sim::flash().pending_verify()) {
      // esp_ota_begin() refuses with ESP_ERR_OTA_ROLLBACK_INVALID_STATE, which
      // the real espp::Ota reports as io_error.
      ec = std::make_error_code(std::errc::io_error);
      return false;
    }
    sim::flash().begin_upload();
    active_ = true;
    image_size_ = image_size;
    image_.clear();
    desc_seen_ = false;
    return true;
  }

  bool write(std::span<const uint8_t> data, std::error_code &ec) {
    if (!active_) {
      ec = std::make_error_code(std::errc::operation_not_permitted);
      return false;
    }
    image_.insert(image_.end(), data.begin(), data.end());
    esp_app_desc_t desc{};
    if (!desc_seen_ && sim::parse_image_desc(image_, desc)) {
      // The descriptor is now in the slot: it reads as a (partial) image.
      desc_seen_ = true;
      sim::flash().upload_descriptor(desc);
    }
    return true;
  }

  bool finish(std::error_code &ec) {
    if (!active_) {
      ec = std::make_error_code(std::errc::operation_not_permitted);
      return false;
    }
    active_ = false;
    esp_app_desc_t desc{};
    if (!sim::parse_image_desc(image_, desc)) {
      ec = std::make_error_code(std::errc::bad_message); // image validation failed
      return false;
    }
    sim::flash().commit_image(desc);
    image_.clear();
    return true;
  }

  bool abort(std::error_code &) {
    active_ = false;
    image_.clear();
    return true;
  }

  /// Restart into the boot slot. Returns immediately in the simulator; the
  /// host performs the restart.
  void restart() { sim::flash().request_restart(); }

  bool is_pending_verify() const { return sim::flash().pending_verify(); }

  bool mark_app_valid(std::error_code &) {
    sim::flash().mark_valid();
    return true;
  }

  bool mark_app_invalid_and_rollback(std::error_code &ec) {
    if (!sim::flash().mark_invalid_and_rollback()) {
      ec = std::make_error_code(std::errc::no_such_device);
      return false;
    }
    throw sim::RestartRequest{sim::flash().take_restart_reason()};
  }

  std::string running_partition_label() const { return sim::flash().running_label(); }
  std::string boot_partition_label() const { return sim::flash().boot_label(); }
  bool session_active() const { return active_; }
  size_t bytes_written() const { return image_.size(); }
  size_t image_size() const { return image_size_; }

private:
  Config config_;
  std::atomic<bool> active_{false};
  size_t image_size_{0};
  std::vector<uint8_t> image_;
  bool desc_seen_{false};
};

} // namespace espp
