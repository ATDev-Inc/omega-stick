// libFuzzer harness for the calibration wire path: the espp stream_frame
// parser, the Dispatcher routing/discovery, and every CalibrationService
// request handler (payload length checks, float validation, store writes).
//
// Each input is fed twice: whole, then split into chunks whose sizes are
// derived from the input bytes themselves, so both the one-shot and the
// incremental-parser paths are exercised. Replies are discarded; the point is
// that no input may crash, trip ASan/UBSan, hang, or grow memory without
// bound. See tests/fuzz/README.md.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include "calibration_service.hpp"
#include "dispatcher.hpp"
#include "omega_store.hpp"

namespace {

struct Harness {
  std::filesystem::path dir;
  espp::OmegaStore store;
  espp::CalibrationService service;
  espp::Dispatcher dispatcher;

  static std::filesystem::path make_dir() {
    std::error_code ec;
    auto d = std::filesystem::temp_directory_path(ec) / "omega_frame_fuzz";
    std::filesystem::create_directories(d, ec);
    return d;
  }

  Harness()
      : dir(make_dir())
      , store({.calibration_path = dir / "omega_calibration.cfg",
               .settings_path = dir / "omega_settings.cfg"})
      , service({.store = store,
                 .send = [](std::span<const uint8_t>) { return true; },
                 .start_calibration = [](std::error_code &) { return true; },
                 .log_level = espp::Logger::Verbosity::NONE}) {
    espp::OmegaCalibration::CenterResult center;
    center.center_x = 12.5f;
    center.center_y = -3.25f;
    espp::OmegaCalibration::RangeResult range;
    range.min_x = -40.0f;
    range.max_x = 38.0f;
    range.min_y = -41.0f;
    range.max_y = 39.5f;
    store.set_calibration(center, range);
    store.set_input_mode(espp::OmegaStore::InputMode::MOUSE);
    dispatcher.register_module(service);
    dispatcher.set_device_info("omega-stick", "fuzz");
    dispatcher.serve_discovery([](std::span<const uint8_t>) {});
  }
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static Harness h;
  const std::span<const uint8_t> input(data, size);

  // Whole buffer, straight to the service and through the dispatcher.
  h.service.reset_parser();
  h.service.feed(input);
  h.dispatcher.feed(input);

  // Same bytes in input-derived chunks (1..16 bytes) to stress resync across
  // feed() boundaries.
  h.service.reset_parser();
  size_t pos = 0;
  while (pos < size) {
    const size_t chunk = std::min<size_t>(size - pos, 1 + (data[pos] & 0x0F));
    h.service.feed(input.subspan(pos, chunk));
    h.dispatcher.feed(input.subspan(pos, chunk));
    pos += chunk;
  }
  return 0;
}
