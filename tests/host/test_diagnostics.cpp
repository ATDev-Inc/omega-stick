// CalibrationService GET_DIAGNOSTICS: the reply layout, the error when the
// device publishes nothing, and the fit-to-frame rule for long reset lists.

#include <string>
#include <vector>

#include "calibration_service.hpp"
#include "file_system.hpp"
#include "omega_store.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using espp::CalibrationService;
using espp::OmegaStore;
namespace stream = espp::stream_frame;
using support::TempDir;

namespace {

using Msg = CalibrationService::Msg;
using Diag = CalibrationService::DiagnosticsData;

struct Fixture {
  TempDir dir;
  OmegaStore store;
  std::vector<uint8_t> sent;
  CalibrationService service;

  explicit Fixture(const char *label, CalibrationService::diagnostics_fn diagnostics)
      : dir(label)
      , store({.calibration_path = dir.file("omega_calibration.cfg"),
               .settings_path = dir.file("omega_settings.cfg")})
      , service({.store = store,
                 .send =
                     [this](std::span<const uint8_t> f) {
                       sent.insert(sent.end(), f.begin(), f.end());
                       return true;
                     },
                 .diagnostics = std::move(diagnostics)}) {}

  stream::Frame request(Msg type) {
    sent.clear();
    CHECK(service.handle_frame(static_cast<uint8_t>(type), {}));
    auto reply = support::decode_one(sent);
    CHECK(reply.has_value());
    CHECK(reply->is_reply());
    return *reply;
  }
};

bool is(const stream::Frame &f, Msg type) { return f.type == static_cast<uint8_t>(type); }

Diag sample() {
  Diag d;
  d.sensor_fault = 2;
  d.sensor_first_fault = 1;
  d.sensor_fault_count = 3;
  d.boot_count = 42;
  d.resets.push_back({.boot = 40, .reason = 9, .abnormal = true, .name = "brownout"});
  d.resets.push_back({.boot = 41, .reason = 1, .abnormal = false, .name = "power-on"});
  d.resets.push_back({.boot = 42, .reason = 3, .abnormal = false, .name = "software"});
  return d;
}

} // namespace

TEST(diagnostics_unsupported_without_callback) {
  Fixture fx("diag_none", nullptr);
  const auto reply = fx.request(Msg::GetDiagnostics);
  CHECK(is(reply, Msg::Error));
}

TEST(diagnostics_reply_follows_documented_layout) {
  Fixture fx("diag_layout", [] { return sample(); });
  const auto reply = fx.request(Msg::GetDiagnostics);
  CHECK(is(reply, Msg::Diagnostics));
  const auto &p = reply.payload;
  const std::span<const uint8_t> s(p);
  CHECK_EQ(p[0], 2);
  CHECK_EQ(p[1], 1);
  CHECK_EQ(stream::get_u32(s.subspan(2)), 3u);
  CHECK_EQ(stream::get_u32(s.subspan(6)), 42u);
  CHECK_EQ(p[10], 3); // reset_count
  // First entry: boot 40, reason 9, abnormal, "brownout".
  CHECK_EQ(stream::get_u32(s.subspan(11)), 40u);
  CHECK_EQ(p[15], 9);
  CHECK_EQ(p[16], 1);
  CHECK_EQ(p[17], 8);
  CHECK(std::string(p.begin() + 18, p.begin() + 26) == "brownout");
  // Second entry starts right after.
  CHECK_EQ(stream::get_u32(s.subspan(26)), 41u);
  CHECK_EQ(p[30], 1);
  CHECK_EQ(p[31], 0);
  CHECK_EQ(p[32], 8);
  // Then u8 calibration_state (0 = usable by default). Total: 11 header + 3
  // entries of (7 + 8) + 1.
  CHECK_EQ(p.back(), CalibrationService::CALIBRATION_USABLE);
  CHECK_EQ(p.size(), std::size_t{11 + 3 * 15 + 1});
}

TEST(diagnostics_ends_with_the_calibration_state) {
  // D26: 1 = no usable calibration, the output is held neutral. It is the last
  // byte, after the reset list, so a console that stops after the list still
  // reads every older field.
  Fixture fx("diag_cal_state", [] {
    Diag d = sample();
    d.calibration_state = CalibrationService::CALIBRATION_NEEDED;
    return d;
  });
  const auto reply = fx.request(Msg::GetDiagnostics);
  CHECK(is(reply, Msg::Diagnostics));
  const auto &p = reply.payload;
  CHECK_EQ(p.size(), std::size_t{11 + 3 * 15 + 1});
  CHECK_EQ(p[11 + 3 * 15], 1);
  CHECK_EQ(p[10], 3); // the reset list in front of it is unchanged
}

TEST(diagnostics_calibration_state_with_no_resets) {
  Fixture fx("diag_cal_state_empty", [] {
    Diag d;
    d.calibration_state = CalibrationService::CALIBRATION_NEEDED;
    return d;
  });
  const auto reply = fx.request(Msg::GetDiagnostics);
  const auto &p = reply.payload;
  CHECK_EQ(p.size(), std::size_t{12});
  CHECK_EQ(p[10], 0);
  CHECK_EQ(p[11], 1);
}

TEST(diagnostics_passes_the_not_observed_code_through_unchanged) {
  // 3 = the polling loop has not seen the sensor yet (D22). The service does not
  // interpret the byte; a console that does not know 3 sees a non-zero value.
  Fixture fx("diag_not_observed", [] {
    Diag d = sample();
    d.sensor_fault = 3;
    d.sensor_first_fault = 0;
    d.sensor_fault_count = 0;
    return d;
  });
  const auto reply = fx.request(Msg::GetDiagnostics);
  CHECK(is(reply, Msg::Diagnostics));
  CHECK_EQ(reply.payload[0], 3);
  CHECK_EQ(reply.payload[1], 0);
}

TEST(diagnostics_keeps_newest_resets_when_the_list_cannot_fit) {
  Fixture fx("diag_fit", [] {
    Diag d;
    d.boot_count = 1000;
    for (uint32_t i = 0; i < 400; i++) {
      d.resets.push_back(
          {.boot = i, .reason = 1, .abnormal = false, .name = std::string(100, 'n')});
    }
    return d;
  });
  const auto reply = fx.request(Msg::GetDiagnostics);
  CHECK(is(reply, Msg::Diagnostics));
  const auto &p = reply.payload;
  CHECK(p.size() <= stream::kMaxPayloadSize);
  const unsigned count = p[10];
  CHECK(count > 0);
  CHECK(count < 400);
  // The first entry emitted is the oldest that fits, and the last is boot 399.
  const std::span<const uint8_t> s(p);
  CHECK_EQ(stream::get_u32(s.subspan(11)), 400u - count);
  const size_t last = 11 + (count - 1) * (7 + 100);
  CHECK_EQ(stream::get_u32(s.subspan(last)), 399u);
  // The calibration state still fits after the clipped list.
  CHECK_EQ(p.size(), last + 7 + 100 + 1);
}

TEST(diagnostics_clips_long_names) {
  Fixture fx("diag_clip", [] {
    Diag d;
    d.resets.push_back({.boot = 1, .reason = 1, .abnormal = false, .name = std::string(300, 'x')});
    return d;
  });
  const auto reply = fx.request(Msg::GetDiagnostics);
  const auto &p = reply.payload;
  CHECK_EQ(p[10], 1);
  CHECK_EQ(p[17], 255);
  CHECK_EQ(p.size(), std::size_t{11 + 7 + 255 + 1});
}
