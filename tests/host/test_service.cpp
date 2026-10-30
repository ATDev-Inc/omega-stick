// CalibrationService: request decoding, replies, store side effects, and the
// framing path through the real espp stream_frame parser/builder.

#include <atomic>
#include <chrono>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "calibration_service.hpp"
#include "file_system.hpp"
#include "omega_store.hpp"
#include "ota.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using espp::CalibrationService;
using espp::OmegaStore;
namespace stream = espp::stream_frame;
using support::TempDir;

namespace {

using Msg = CalibrationService::Msg;

/// A store + service pair whose replies are captured for decoding.
struct Fixture {
  TempDir dir;
  OmegaStore::Config store_cfg;
  OmegaStore store;
  std::vector<uint8_t> sent;
  int send_calls{0};
  CalibrationService service;

  explicit Fixture(const char *label, OmegaStore::Config extra = {})
      : dir(label)
      , store_cfg(with_paths(std::move(extra)))
      , store(store_cfg)
      , service({.store = store, .send = [this](std::span<const uint8_t> f) {
                   ++send_calls;
                   sent.insert(sent.end(), f.begin(), f.end());
                   return true;
                 }}) {}

  OmegaStore::Config with_paths(OmegaStore::Config c) const {
    c.calibration_path = dir.file("omega_calibration.cfg");
    c.settings_path = dir.file("omega_settings.cfg");
    return c;
  }

  /// Handles one request and returns the single decoded reply frame.
  stream::Frame request(Msg type, std::vector<uint8_t> payload = {}) {
    sent.clear();
    CHECK(service.handle_frame(static_cast<uint8_t>(type), payload));
    auto reply = support::decode_one(sent);
    CHECK(reply.has_value());
    CHECK(reply->is_reply());
    CHECK_EQ(reply->module, CalibrationService::kModule);
    return *reply;
  }
};

bool is(const stream::Frame &f, Msg type) { return f.type == static_cast<uint8_t>(type); }

} // namespace

TEST(service_get_state_encodes_snapshot) {
  Fixture fx("svc_state");
  fx.store.set_calibration(support::sample_center(), support::sample_range());
  fx.store.set_deadzone_override(std::nullopt, 0.09f);
  fx.store.set_nudge_settings(0.45f, 0.75f, std::chrono::milliseconds(120));
  fx.store.set_input_mode(OmegaStore::InputMode::XAC);

  const auto reply = fx.request(Msg::GetState);
  CHECK(is(reply, Msg::State));
  const auto &p = reply.payload;
  CHECK_EQ(p.size(), CalibrationService::kStatePayloadSize);

  const auto c = support::sample_center();
  const auto r = support::sample_range();
  CHECK_NEAR(support::get_f32(p, 0), c.center_x, 1e-6);
  CHECK_NEAR(support::get_f32(p, 4), c.center_y, 1e-6);
  CHECK_NEAR(support::get_f32(p, 16), c.manual_offset_x, 1e-6);
  CHECK_NEAR(support::get_f32(p, 20), c.manual_offset_y, 1e-6);
  CHECK_NEAR(support::get_f32(p, 24), r.min_x, 1e-6);
  CHECK_NEAR(support::get_f32(p, 52), r.deadzone, 1e-6);
  CHECK_EQ(p[56], 0); // no center override
  CHECK_EQ(p[61], 1); // range override present
  CHECK_NEAR(support::get_f32(p, 62), 0.09f, 1e-6);
  CHECK_NEAR(support::get_f32(p, 66), 0.45f, 1e-6);
  CHECK_NEAR(support::get_f32(p, 70), 0.75f, 1e-6);
  CHECK_EQ(stream::get_u32(std::span<const uint8_t>(p).subspan(74)), 120u);
  CHECK_EQ(p[78], 1); // XAC
  CHECK_EQ(p[79], 1); // nudge clicks on (the default)
}

TEST(service_state_reports_nudge_enabled_as_the_last_byte) {
  Fixture fx("svc_state_nudge");
  fx.store.set_nudge_enabled(false);
  auto reply = fx.request(Msg::GetState);
  CHECK(is(reply, Msg::State));
  CHECK_EQ(reply.payload.size(), size_t{80});
  CHECK_EQ(reply.payload[79], 0);

  fx.store.set_nudge_enabled(true);
  reply = fx.request(Msg::GetState);
  CHECK_EQ(reply.payload[79], 1);
}

TEST(service_set_nudge_enabled_toggles_the_switch_and_keeps_tuning) {
  Fixture fx("svc_nudge_enabled");
  fx.store.set_nudge_settings(0.45f, 0.75f, std::chrono::milliseconds(120));

  auto reply = fx.request(Msg::SetNudgeEnabled, {0});
  CHECK(is(reply, Msg::Ok));
  CHECK_EQ(stream::get_u32(reply.payload), 0u);
  auto s = fx.store.settings();
  CHECK(!s.nudge_enabled);
  CHECK_NEAR(s.nudge_distance_threshold, 0.45f, 1e-6);
  CHECK_NEAR(s.nudge_exit_threshold, 0.75f, 1e-6);
  CHECK_EQ(s.nudge_hold_time.count(), 120);

  CHECK(is(fx.request(Msg::SetNudgeEnabled, {1}), Msg::Ok));
  CHECK(fx.store.settings().nudge_enabled);
}

TEST(service_set_nudge_enabled_rejects_other_values_and_lengths) {
  Fixture fx("svc_nudge_enabled_bad");
  for (const auto &payload : std::vector<std::vector<uint8_t>>{{}, {2}, {0xFF}, {0, 0}, {1, 1}}) {
    const auto reply = fx.request(Msg::SetNudgeEnabled, payload);
    CHECK(is(reply, Msg::Error));
    CHECK_EQ(stream::get_u32(reply.payload), static_cast<uint32_t>(std::errc::invalid_argument));
    CHECK(support::error_message(reply).find("SET_NUDGE_ENABLED") != std::string::npos);
  }
  // The rejected requests left the default in place.
  CHECK(fx.store.settings().nudge_enabled);
}

TEST(service_set_manual_offset_updates_store_and_replies_ok) {
  int changed = 0;
  Fixture fx("svc_offset", {.on_calibration_changed = [&]() { ++changed; }});
  std::vector<uint8_t> payload;
  support::put_f32(payload, 0.75f);
  support::put_f32(payload, -1.25f);

  const auto reply = fx.request(Msg::SetManualOffset, payload);
  CHECK(is(reply, Msg::Ok));
  CHECK_EQ(stream::get_u32(reply.payload), 0u);
  CHECK_EQ(changed, 1);
  CHECK_NEAR(fx.store.center().manual_offset_x, 0.75f, 1e-6);
  CHECK_NEAR(fx.store.center().manual_offset_y, -1.25f, 1e-6);
}

TEST(service_rejects_wrong_sized_payloads) {
  Fixture fx("svc_badsize");
  auto reply = fx.request(Msg::SetManualOffset, {1, 2, 3});
  CHECK(is(reply, Msg::Error));
  CHECK(support::error_message(reply).find("SET_MANUAL_OFFSET") != std::string::npos);

  reply = fx.request(Msg::SetDeadzoneOverride, {0});
  CHECK(is(reply, Msg::Error));

  reply = fx.request(Msg::SetNudgeSettings, {0});
  CHECK(is(reply, Msg::Error));

  reply = fx.request(Msg::SetInputMode, {});
  CHECK(is(reply, Msg::Error));

  reply = fx.request(Msg::StartCalibration, {1});
  CHECK(is(reply, Msg::Error));
}

TEST(service_set_deadzone_override_honours_flags) {
  Fixture fx("svc_dz");
  std::vector<uint8_t> payload{0x02};
  support::put_f32(payload, 0.5f);  // center (ignored, flag clear)
  support::put_f32(payload, 0.06f); // range
  CHECK(is(fx.request(Msg::SetDeadzoneOverride, payload), Msg::Ok));
  const auto s = fx.store.settings();
  CHECK(!s.manual_center_deadzone.has_value());
  CHECK_NEAR(s.manual_range_deadzone.value_or(0), 0.06f, 1e-6);
}

TEST(service_rejects_non_finite_manual_offset) {
  int changed = 0;
  Fixture fx("svc_offset_nan", {.on_calibration_changed = [&]() { ++changed; }});
  const float before_x = fx.store.center().manual_offset_x;
  for (const float bad :
       {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    std::vector<uint8_t> payload;
    support::put_f32(payload, 0.25f);
    support::put_f32(payload, bad);
    const auto reply = fx.request(Msg::SetManualOffset, payload);
    CHECK(is(reply, Msg::Error));
    CHECK(support::error_message(reply).find("finite") != std::string::npos);
  }
  CHECK_EQ(changed, 0);
  CHECK_NEAR(fx.store.center().manual_offset_x, before_x, 1e-6);
}

TEST(service_rejects_invalid_deadzone_override_values) {
  Fixture fx("svc_dz_bad");
  for (const float bad :
       {-0.01f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    std::vector<uint8_t> payload{0x01};
    support::put_f32(payload, bad); // center, flag set
    support::put_f32(payload, 0.0f);
    CHECK(is(fx.request(Msg::SetDeadzoneOverride, payload), Msg::Error));
  }
  CHECK(!fx.store.settings().manual_center_deadzone.has_value());

  // Above the calibration maximums (center 0.5, range 0.1) is refused too,
  // and the stored override is left untouched.
  {
    std::vector<uint8_t> payload{0x01};
    support::put_f32(payload, 2.0f);
    support::put_f32(payload, 0.0f);
    const auto reply = fx.request(Msg::SetDeadzoneOverride, payload);
    CHECK(is(reply, Msg::Error));
    CHECK(support::error_message(reply).find("center deadzone") != std::string::npos);
  }
  {
    std::vector<uint8_t> payload{0x02};
    support::put_f32(payload, 0.0f);
    support::put_f32(payload, 0.11f);
    const auto reply = fx.request(Msg::SetDeadzoneOverride, payload);
    CHECK(is(reply, Msg::Error));
    CHECK(support::error_message(reply).find("range deadzone") != std::string::npos);
  }
  CHECK(!fx.store.settings().manual_center_deadzone.has_value());
  CHECK(!fx.store.settings().manual_range_deadzone.has_value());

  // A bad value under a CLEAR flag is ignored, as the protocol documents.
  std::vector<uint8_t> payload{0x02};
  support::put_f32(payload, std::numeric_limits<float>::quiet_NaN());
  support::put_f32(payload, 0.05f);
  CHECK(is(fx.request(Msg::SetDeadzoneOverride, payload), Msg::Ok));
  CHECK_NEAR(fx.store.settings().manual_range_deadzone.value_or(0), 0.05f, 1e-6);
}

TEST(service_set_nudge_settings_validates_ranges) {
  Fixture fx("svc_nudge");
  auto build = [](float enter, float exit, uint32_t ms) {
    std::vector<uint8_t> p;
    support::put_f32(p, enter);
    support::put_f32(p, exit);
    stream::put_u32(p, ms);
    return p;
  };
  CHECK(is(fx.request(Msg::SetNudgeSettings, build(0.0f, 0.5f, 100)), Msg::Error));
  CHECK(is(fx.request(Msg::SetNudgeSettings, build(1.5f, 0.5f, 100)), Msg::Error));
  CHECK(is(fx.request(Msg::SetNudgeSettings, build(0.5f, 1.5f, 100)), Msg::Error));
  CHECK(is(fx.request(Msg::SetNudgeSettings, build(0.35f, 0.65f, 150)), Msg::Ok));
  const auto s = fx.store.settings();
  CHECK_NEAR(s.nudge_distance_threshold, 0.35f, 1e-6);
  CHECK_NEAR(s.nudge_exit_threshold, 0.65f, 1e-6);
  CHECK_EQ(s.nudge_hold_time.count(), 150);
}

TEST(service_set_input_mode_switches_store_and_notifies) {
  std::vector<OmegaStore::InputMode> seen;
  Fixture fx("svc_mode", {.on_mode_changed = [&](OmegaStore::InputMode m) { seen.push_back(m); }});

  CHECK(is(fx.request(Msg::SetInputMode, {2}), Msg::Error));
  CHECK(seen.empty());

  CHECK(is(fx.request(Msg::SetInputMode, {1}), Msg::Ok));
  CHECK_EQ(seen.size(), size_t{1});
  CHECK(seen[0] == OmegaStore::InputMode::XAC);
  CHECK(fx.store.settings().input_mode == OmegaStore::InputMode::XAC);
}

// main.cpp's on_mode_changed calls esp_restart(), so the OK must already be sent
// when it runs, or the console reports a timeout for a switch that worked.
TEST(service_ok_reply_is_sent_before_mode_change_callback_runs) {
  int sends_seen_from_callback = -1;
  const Fixture *self = nullptr;
  Fixture fx("svc_mode_order", {.on_mode_changed = [&](OmegaStore::InputMode) {
               sends_seen_from_callback = self->send_calls;
             }});
  self = &fx;

  CHECK(is(fx.request(Msg::SetInputMode, {1}), Msg::Ok));
  CHECK_EQ(sends_seen_from_callback, 1);
}

// A reply the transport refused never reached the host, so the device must
// not act on the request: no save, no restart, and the host can retry.
TEST(service_dropped_reply_does_not_apply_the_mode_change) {
  TempDir dir("svc_mode_dropped");
  int mode_changes = 0;
  OmegaStore::Config cfg;
  cfg.calibration_path = dir.file("omega_calibration.cfg");
  cfg.settings_path = dir.file("omega_settings.cfg");
  cfg.on_mode_changed = [&](OmegaStore::InputMode) { ++mode_changes; };
  OmegaStore store(cfg);
  bool accept = false; // the transport refuses until this flips
  std::vector<uint8_t> sent;
  CalibrationService svc({.store = store, .send = [&](std::span<const uint8_t> f) {
                            if (!accept)
                              return false;
                            sent.assign(f.begin(), f.end());
                            return true;
                          }});
  const std::vector<uint8_t> xac{1};
  CHECK(svc.handle_frame(static_cast<uint8_t>(Msg::SetInputMode), xac));
  CHECK_EQ(mode_changes, 0);
  CHECK(store.settings().input_mode == OmegaStore::InputMode::MOUSE);
  // The dropped action is gone: an unrelated request whose reply does go
  // out must not trigger it...
  accept = true;
  CHECK(svc.handle_frame(static_cast<uint8_t>(Msg::GetState), {}));
  CHECK_EQ(mode_changes, 0);
  // ...and a retry on the same service works normally, with the OK saying
  // that a switch is happening.
  CHECK(svc.handle_frame(static_cast<uint8_t>(Msg::SetInputMode), xac));
  CHECK_EQ(mode_changes, 1);
  const auto ok = support::decode_one(sent);
  CHECK(ok && is(*ok, Msg::Ok));
  CHECK_EQ(stream::get_u32(std::span<const uint8_t>(ok->payload)), 1u);
}

TEST(service_set_input_mode_to_the_active_mode_is_ok_without_a_callback) {
  int mode_changes = 0;
  Fixture fx("svc_mode_same", {.on_mode_changed = [&](OmegaStore::InputMode) { ++mode_changes; }});
  const auto same = fx.request(Msg::SetInputMode, {0}); // MOUSE is the default
  CHECK(is(same, Msg::Ok));
  CHECK_EQ(stream::get_u32(std::span<const uint8_t>(same.payload)), 0u); // 0: nothing changed
  CHECK_EQ(mode_changes, 0);
  const auto switched = fx.request(Msg::SetInputMode, {1});
  CHECK(is(switched, Msg::Ok));
  CHECK_EQ(stream::get_u32(std::span<const uint8_t>(switched.payload)), 1u); // 1: switching
  CHECK_EQ(mode_changes, 1);
}

TEST(service_save_to_flash_writes_files_and_reports_failure) {
  Fixture fx("svc_save");
  fx.store.set_calibration(support::sample_center(), support::sample_range());
  CHECK(is(fx.request(Msg::SaveToFlash), Msg::Ok));
  CHECK(std::filesystem::exists(fx.store_cfg.calibration_path));
  CHECK(std::filesystem::exists(fx.store_cfg.settings_path));

  Fixture bad("svc_save_bad");
  // Point the calibration file into a directory that does not exist.
  OmegaStore broken({.calibration_path = bad.dir.path / "missing" / "cal.cfg",
                     .settings_path = bad.dir.file("s.cfg")});
  std::vector<uint8_t> sent;
  CalibrationService svc({.store = broken, .send = [&](std::span<const uint8_t> f) {
                            sent.assign(f.begin(), f.end());
                            return true;
                          }});
  CHECK(svc.handle_frame(static_cast<uint8_t>(Msg::SaveToFlash), {}));
  const auto reply = support::decode_one(sent);
  CHECK(reply && is(*reply, Msg::Error));
  CHECK(support::error_message(*reply).find("SAVE_TO_FLASH failed") != std::string::npos);
}

TEST(service_start_calibration_requires_hook_and_relays_its_verdict) {
  Fixture fx("svc_cal_nohook");
  auto reply = fx.request(Msg::StartCalibration);
  CHECK(is(reply, Msg::Error));
  CHECK_EQ(stream::get_u32(reply.payload),
           static_cast<uint32_t>(std::errc::function_not_supported));

  TempDir dir("svc_cal_hook");
  OmegaStore store({.calibration_path = dir.file("c.cfg"), .settings_path = dir.file("s.cfg")});
  std::vector<uint8_t> sent;
  bool accept = true;
  int kicks = 0;
  CalibrationService svc({.store = store,
                          .send =
                              [&](std::span<const uint8_t> f) {
                                sent.assign(f.begin(), f.end());
                                return true;
                              },
                          .start_calibration =
                              [&](std::error_code &ec) {
                                ++kicks;
                                if (!accept) {
                                  ec = std::make_error_code(std::errc::device_or_resource_busy);
                                }
                                return accept;
                              }});

  CHECK(svc.handle_frame(static_cast<uint8_t>(Msg::StartCalibration), {}));
  auto f = support::decode_one(sent);
  CHECK(f && is(*f, Msg::Ok));
  CHECK_EQ(kicks, 1);

  accept = false;
  CHECK(svc.handle_frame(static_cast<uint8_t>(Msg::StartCalibration), {}));
  f = support::decode_one(sent);
  CHECK(f && is(*f, Msg::Error));
  CHECK(support::error_message(*f).find("could not be started") != std::string::npos);
}

TEST(service_notify_calibration_result_is_unsolicited_frame) {
  Fixture fx("svc_notify");
  fx.sent.clear();
  fx.service.notify_calibration_result(true);
  auto f = support::decode_one(fx.sent);
  CHECK(f && is(*f, Msg::CalibrationResult));
  CHECK_EQ(f->payload.size(), size_t{1});
  CHECK_EQ(f->payload[0], 1);

  fx.sent.clear();
  fx.service.notify_calibration_result(false, std::make_error_code(std::errc::timed_out));
  f = support::decode_one(fx.sent);
  CHECK(f && is(*f, Msg::CalibrationResult));
  CHECK_EQ(f->payload[0], 0);
  CHECK_EQ(stream::get_u32(std::span<const uint8_t>(f->payload).subspan(1)),
           static_cast<uint32_t>(std::errc::timed_out));
  CHECK(f->payload.size() > 5); // message text follows
}

TEST(service_ignores_foreign_frames) {
  Fixture fx("svc_ignore");
  fx.sent.clear();
  CHECK(!fx.service.handle_frame(0x10, {})); // not a calibration type
  CHECK(fx.sent.empty());

  stream::Frame other;
  other.module = CalibrationService::kModule + 1;
  other.type = static_cast<uint8_t>(Msg::GetState);
  fx.service.handle(other);
  CHECK(fx.sent.empty());

  stream::Frame echoed;
  echoed.module = CalibrationService::kModule;
  echoed.type = static_cast<uint8_t>(Msg::GetState);
  echoed.flags = stream::make_flags(true); // a reply must not be re-answered
  fx.service.handle(echoed);
  CHECK(fx.sent.empty());
}

TEST(service_feed_parses_wire_bytes_across_chunks) {
  Fixture fx("svc_feed");
  fx.store.set_calibration(support::sample_center(), support::sample_range());
  const auto wire = stream::build_frame(false, CalibrationService::kModule,
                                        static_cast<uint8_t>(Msg::GetState), {});
  // Deliver a byte at a time to exercise the incremental parser.
  fx.sent.clear();
  for (size_t i = 0; i < wire.size(); ++i) {
    fx.service.feed(std::span<const uint8_t>(wire.data() + i, 1));
  }
  auto reply = support::decode_one(fx.sent);
  CHECK(reply && is(*reply, Msg::State));
  CHECK_EQ(reply->payload.size(), CalibrationService::kStatePayloadSize);

  // Garbage in front of a valid frame is skipped by resynchronisation.
  fx.sent.clear();
  std::vector<uint8_t> noisy{0xAA, 0x55, 0x00};
  noisy.insert(noisy.end(), wire.begin(), wire.end());
  fx.service.feed(noisy);
  reply = support::decode_one(fx.sent);
  CHECK(reply && is(*reply, Msg::State));
}

// ---- OTA / firmware info / restart ------------------------------------------

namespace {

/// Like Fixture, but with an espp::Ota fake and restart callback optionally wired.
struct OtaFixture {
  TempDir dir;
  OmegaStore::Config store_cfg;
  OmegaStore store;
  espp::Ota ota;
  std::vector<uint8_t> sent;
  std::vector<uint8_t> sent2;
  std::atomic<bool> restart_ran{false};
  // Config::output_hold edges, counted across both services as main.cpp does.
  std::atomic<int> output_holds{0};
  bool held() const { return output_holds.load() > 0; }
  CalibrationService service;
  CalibrationService service2; // second transport sharing the same Ota

  explicit OtaFixture(const char *label, bool wire_ota = true, bool wire_restart = false)
      : dir(label)
      , store_cfg(with_paths())
      , store(store_cfg)
      , ota(espp::Ota::Config{})
      , service(make_config(wire_ota, wire_restart, sent))
      , service2(make_config(wire_ota, wire_restart, sent2)) {
    esp_ota_fake().reset();
  }

  OmegaStore::Config with_paths() const {
    OmegaStore::Config c;
    c.calibration_path = dir.file("omega_calibration.cfg");
    c.settings_path = dir.file("omega_settings.cfg");
    return c;
  }

  CalibrationService::Config make_config(bool wire_ota, bool wire_restart,
                                         std::vector<uint8_t> &out) {
    CalibrationService::Config cfg{.store = store, .send = [&out](std::span<const uint8_t> f) {
                                     out.insert(out.end(), f.begin(), f.end());
                                     return true;
                                   }};
    if (wire_ota) {
      cfg.ota = &ota;
    }
    if (wire_restart) {
      cfg.restart = [this] { restart_ran = true; };
    }
    cfg.output_hold = [this](bool hold) { output_holds += hold ? 1 : -1; };
    return cfg;
  }

  stream::Frame request(Msg type, std::vector<uint8_t> payload = {}) {
    sent.clear();
    CHECK(service.handle_frame(static_cast<uint8_t>(type), payload));
    auto reply = support::decode_one(sent);
    CHECK(reply.has_value());
    CHECK(reply->is_reply());
    return *reply;
  }

  /// Same as request(), but over the second service.
  stream::Frame request2(Msg type, std::vector<uint8_t> payload = {}) {
    sent2.clear();
    CHECK(service2.handle_frame(static_cast<uint8_t>(type), payload));
    auto reply = support::decode_one(sent2);
    CHECK(reply.has_value());
    CHECK(reply->is_reply());
    return *reply;
  }
};

std::vector<uint8_t> u32le(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

std::vector<uint8_t> ota_data(uint32_t offset, const std::vector<uint8_t> &bytes) {
  auto p = u32le(offset);
  p.insert(p.end(), bytes.begin(), bytes.end());
  return p;
}

} // namespace

TEST(service_get_fw_info_reports_flags_and_strings) {
  OtaFixture fx("svc_fwinfo");
  fx.ota.pending_verify = true;
  fx.ota.running_label = "ota_1";
  fx.ota.boot_label = "ota_0";
  auto reply = fx.request(Msg::GetFwInfo);
  CHECK(is(reply, Msg::FwInfo));
  const auto &p = reply.payload;
  CHECK(p.size() >= 2);
  CHECK_EQ(p[0], 1);
  CHECK_EQ(p[1], 0);
  std::vector<std::string> strs;
  size_t pos = 2;
  while (pos < p.size()) {
    const size_t n = p[pos++];
    CHECK(pos + n <= p.size());
    strs.emplace_back(reinterpret_cast<const char *>(p.data() + pos), n);
    pos += n;
  }
  const std::vector<std::string> expected{
      "omega_stick", "v9.9.9-test", "Oct 10 2026", "12:34:56",    "v6.0.2",   "ota_1", "deadbeef",
      "ota_0",       "ota_1",       "0.0.9-prev",  "Jan  2 2026", "01:02:03", "valid"};
  CHECK_EQ(strs.size(), expected.size());
  for (size_t i = 0; i < expected.size() && i < strs.size(); ++i) {
    CHECK(strs[i] == expected[i]);
  }

  // The flags follow the fake: not pending, and an active session shows up.
  fx.ota.pending_verify = false;
  CHECK(is(fx.request(Msg::OtaBegin, u32le(16)), Msg::Ok));
  reply = fx.request(Msg::GetFwInfo);
  CHECK(is(reply, Msg::FwInfo));
  CHECK_EQ(reply.payload[0], 0);
  CHECK_EQ(reply.payload[1], 1);
}

namespace {
std::vector<std::string> fw_info_strings(const stream::Frame &reply) {
  std::vector<std::string> strs;
  const auto &p = reply.payload;
  size_t pos = 2;
  while (pos < p.size()) {
    const size_t n = p[pos++];
    if (pos + n > p.size()) {
      break;
    }
    strs.emplace_back(reinterpret_cast<const char *>(p.data() + pos), n);
    pos += n;
  }
  return strs;
}
} // namespace

TEST(service_get_fw_info_previous_slot_states) {
  OtaFixture fx("svc_fwinfo_prev");
  auto &fake = esp_ota_fake();

  fake.other_has_image = false;
  auto strs = fw_info_strings(fx.request(Msg::GetFwInfo));
  CHECK_EQ(strs.size(), 13u);
  if (strs.size() == 13) {
    CHECK(strs[8] == "ota_1");
    CHECK(strs[9].empty());
    CHECK(strs[10].empty());
    CHECK(strs[11].empty());
    CHECK(strs[12] == "empty");
  }

  fake.other_has_image = true;
  fake.other_state = ESP_OTA_IMG_INVALID;
  strs = fw_info_strings(fx.request(Msg::GetFwInfo));
  CHECK_EQ(strs.size(), 13u);
  if (strs.size() == 13) {
    CHECK(strs[9] == "0.0.9-prev");
    CHECK(strs[12] == "invalid");
  }
}

// An aborted or expired upload leaves the start of an image in the other slot:
// its descriptor reads (so the version is still shown) but the image does not
// verify, so REVERT would fail. FW_INFO says "partial" instead of the stale
// otadata state.
TEST(service_get_fw_info_reports_a_partial_previous_slot) {
  OtaFixture fx("svc_fwinfo_partial");
  auto &fake = esp_ota_fake();
  fake.other_verifies = false;
  fake.other_state = ESP_OTA_IMG_UNDEFINED;
  auto strs = fw_info_strings(fx.request(Msg::GetFwInfo));
  CHECK_EQ(strs.size(), 13u);
  if (strs.size() == 13) {
    CHECK(strs[8] == "ota_1");
    CHECK(strs[9] == "0.0.9-prev");
    CHECK(strs[10] == "Jan  2 2026");
    CHECK(strs[11] == "01:02:03");
    CHECK(strs[12] == "partial");
  }
  CHECK_EQ(fake.verify_calls, 1);

  // A complete image is still reported by its otadata state.
  fake.other_verifies = true;
  fake.other_state = ESP_OTA_IMG_VALID;
  strs = fw_info_strings(fx.request(Msg::GetFwInfo));
  CHECK(strs.size() == 13u && strs[12] == "valid");

  // While an upload runs, the slot it writes is partial by definition; the
  // image is not read.
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  const int calls = fake.verify_calls;
  strs = fw_info_strings(fx.request(Msg::GetFwInfo));
  CHECK(strs.size() == 13u && strs[12] == "partial");
  CHECK_EQ(fake.verify_calls, calls);
}

TEST(service_revert_errors) {
  OtaFixture none("svc_revert_none", /*wire_ota=*/false);
  auto reply = none.request(Msg::Revert);
  CHECK(is(reply, Msg::Error));
  CHECK_EQ(stream::get_u32(reply.payload),
           static_cast<uint32_t>(std::errc::function_not_supported));

  OtaFixture fx("svc_revert_err");
  auto &fake = esp_ota_fake();

  reply = fx.request(Msg::Revert, {1});
  CHECK(is(reply, Msg::Error));
  CHECK_EQ(stream::get_u32(reply.payload), static_cast<uint32_t>(std::errc::invalid_argument));

  // An upload owned by the other service (sharing the engine) blocks REVERT.
  CHECK(is(fx.request2(Msg::OtaBegin, u32le(16)), Msg::Ok));
  reply = fx.request(Msg::Revert);
  CHECK(is(reply, Msg::Error));
  CHECK_EQ(stream::get_u32(reply.payload), static_cast<uint32_t>(std::errc::operation_in_progress));
  CHECK_EQ(fake.set_boot_calls, 0);
  CHECK(fx.ota.session_active());
  CHECK(is(fx.request2(Msg::OtaAbort), Msg::Ok));

  fake.other_has_image = false;
  reply = fx.request(Msg::Revert);
  CHECK(is(reply, Msg::Error));
  CHECK_EQ(stream::get_u32(reply.payload), static_cast<uint32_t>(std::errc::no_such_device));
  CHECK_EQ(fake.set_boot_calls, 0);

  fake.other_has_image = true;
  fake.set_boot_result = ESP_FAIL;
  reply = fx.request(Msg::Revert);
  CHECK(is(reply, Msg::Error));
  CHECK_EQ(stream::get_u32(reply.payload), static_cast<uint32_t>(std::errc::io_error));
  CHECK(support::error_message(reply).find(esp_err_to_name(ESP_FAIL)) != std::string::npos);
  CHECK_EQ(fake.set_boot_calls, 1);
  CHECK(!fx.ota.restart_called.load());
}

TEST(service_revert_sets_boot_partition_and_restarts) {
  OtaFixture fx("svc_revert_ok");
  const auto &fake = esp_ota_fake();
  CHECK(is(fx.request(Msg::Revert), Msg::Ok));
  CHECK_EQ(fake.set_boot_calls, 1);
  CHECK(fake.last_set_boot == &fake.other);
  // Restart comes from a detached thread ~750 ms after the reply.
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  CHECK(fx.ota.restart_called.load());
}

TEST(service_ota_messages_error_without_ota) {
  OtaFixture fx("svc_noota", /*wire_ota=*/false);
  const std::vector<std::pair<Msg, std::vector<uint8_t>>> cases{
      {Msg::GetFwInfo, {}}, {Msg::OtaBegin, u32le(100)}, {Msg::MarkValid, {}}};
  for (const auto &[type, payload] : cases) {
    auto reply = fx.request(type, payload);
    CHECK(is(reply, Msg::Error));
    CHECK_EQ(stream::get_u32(reply.payload),
             static_cast<uint32_t>(std::errc::function_not_supported));
  }
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {1, 2, 3})), Msg::Error));
  CHECK(is(fx.request(Msg::OtaFinish), Msg::Error));
  // Abort is harmless without an engine.
  CHECK(is(fx.request(Msg::OtaAbort), Msg::Ok));
}

TEST(service_ota_happy_path_streams_image_and_finishes) {
  OtaFixture fx("svc_ota_ok");
  std::vector<uint8_t> image(1000);
  for (size_t i = 0; i < image.size(); ++i) {
    image[i] = static_cast<uint8_t>(i * 7 + 3);
  }
  CHECK(is(fx.request(Msg::OtaBegin, u32le(static_cast<uint32_t>(image.size()))), Msg::Ok));
  CHECK(fx.ota.session_active());
  CHECK(fx.service.update_in_progress());
  CHECK_EQ(fx.ota.image_size(), image.size());

  uint32_t offset = 0;
  for (const uint32_t chunk : {400u, 400u, 200u}) {
    const std::vector<uint8_t> part(image.begin() + offset, image.begin() + offset + chunk);
    auto reply = fx.request(Msg::OtaData, ota_data(offset, part));
    CHECK(is(reply, Msg::Ok));
    offset += chunk;
    CHECK_EQ(stream::get_u32(reply.payload), offset);
  }
  CHECK(fx.ota.image == image);

  CHECK(is(fx.request(Msg::OtaFinish), Msg::Ok));
  CHECK(!fx.ota.session_active());
  CHECK_EQ(fx.ota.finish_calls, 1);

  // The service reboots from a detached thread ~750 ms after the reply.
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  CHECK(fx.ota.restart_called.load());
}

TEST(service_ota_data_rejects_bad_offset_and_missing_session) {
  OtaFixture fx("svc_ota_bad");

  // No OtaBegin yet.
  auto reply = fx.request(Msg::OtaData, ota_data(0, {1, 2, 3}));
  CHECK(is(reply, Msg::Error));
  CHECK_EQ(stream::get_u32(reply.payload),
           static_cast<uint32_t>(std::errc::operation_not_permitted));
  CHECK(is(fx.request(Msg::OtaFinish), Msg::Error));

  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {1, 2, 3, 4})), Msg::Ok));

  // Wrong offset: error, nothing written, session survives.
  reply = fx.request(Msg::OtaData, ota_data(100, {9, 9}));
  CHECK(is(reply, Msg::Error));
  CHECK(support::error_message(reply).find("offset") != std::string::npos);
  CHECK(fx.ota.session_active());
  CHECK_EQ(fx.ota.bytes_written(), 4u);

  // An offset with no data is rejected too.
  CHECK(is(fx.request(Msg::OtaData, u32le(4)), Msg::Error));

  // The right offset still works afterwards.
  reply = fx.request(Msg::OtaData, ota_data(4, {5, 6}));
  CHECK(is(reply, Msg::Ok));
  CHECK_EQ(stream::get_u32(reply.payload), 6u);
}

TEST(service_ota_write_failure_ends_session) {
  OtaFixture fx("svc_ota_wfail");
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  fx.ota.fail_next_write = true;
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {1, 2})), Msg::Error));
  CHECK(!fx.ota.session_active());
  // The service no longer owns a session.
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {1, 2})), Msg::Error));
}

TEST(service_ota_abort_ends_session) {
  OtaFixture fx("svc_ota_abort");
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  CHECK(fx.ota.session_active());
  CHECK(is(fx.request(Msg::OtaAbort), Msg::Ok));
  CHECK(!fx.ota.session_active());
  CHECK_EQ(fx.ota.abort_calls, 1);
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {1})), Msg::Error));
}

TEST(service_ota_begin_takes_over_own_stale_session) {
  // Disconnect, reconnect, retry: the host abandoned an upload midway and
  // starts over on the same link without a reboot.
  OtaFixture fx("svc_ota_retry");
  std::vector<uint8_t> image(300);
  for (size_t i = 0; i < image.size(); ++i) {
    image[i] = static_cast<uint8_t>(i * 5 + 1);
  }
  const auto size = static_cast<uint32_t>(image.size());
  CHECK(is(fx.request(Msg::OtaBegin, u32le(size)), Msg::Ok));
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {9, 9, 9, 9, 9})), Msg::Ok));
  CHECK_EQ(fx.ota.bytes_written(), 5u);

  CHECK(is(fx.request(Msg::OtaBegin, u32le(size)), Msg::Ok));
  CHECK_EQ(fx.ota.abort_calls, 1);
  CHECK(fx.ota.session_active());
  CHECK_EQ(fx.ota.bytes_written(), 0u);

  CHECK(is(fx.request(Msg::OtaData, ota_data(0, image)), Msg::Ok));
  CHECK(fx.ota.image == image);
  CHECK(is(fx.request(Msg::OtaFinish), Msg::Ok));
  CHECK(!fx.ota.session_active());
  CHECK_EQ(fx.ota.finish_calls, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
}

TEST(service_ota_begin_refused_while_other_service_owns_session) {
  OtaFixture fx("svc_ota_two");
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  CHECK(is(fx.request2(Msg::OtaBegin, u32le(64)), Msg::Error));
  CHECK_EQ(fx.ota.abort_calls, 0);
  CHECK(fx.ota.session_active());
  CHECK(is(fx.request2(Msg::OtaData, ota_data(0, {1})), Msg::Error));
  // The owner is unaffected.
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {1, 2})), Msg::Ok));
}

TEST(service_revert_aborts_own_abandoned_upload) {
  OtaFixture fx("svc_revert_abandoned");
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {1, 2, 3})), Msg::Ok));
  CHECK(is(fx.request(Msg::Revert), Msg::Ok));
  CHECK(!fx.ota.session_active());
  CHECK_EQ(fx.ota.abort_calls, 1);
  CHECK_EQ(esp_ota_fake().set_boot_calls, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  CHECK(fx.ota.restart_called.load());
}

TEST(service_expire_stale_update) {
  using std::chrono::milliseconds;
  OtaFixture fx("svc_expire");

  // Nothing owned: nothing to expire.
  CHECK(!fx.service.expire_stale_update(milliseconds(0)));

  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  // A non-owner never touches another service's session.
  CHECK(!fx.service2.expire_stale_update(milliseconds(0)));
  CHECK(fx.ota.session_active());
  CHECK_EQ(fx.ota.abort_calls, 0);

  // Not idle long enough yet.
  CHECK(!fx.service.expire_stale_update(std::chrono::hours(1)));
  CHECK(fx.ota.session_active());
  CHECK(!fx.service.expire_stale_update(CalibrationService::OTA_IDLE_TIMEOUT));
  CHECK(fx.ota.session_active());

  CHECK(fx.service.expire_stale_update(milliseconds(0)));
  CHECK(!fx.ota.session_active());
  CHECK_EQ(fx.ota.abort_calls, 1);
  CHECK(!fx.service.expire_stale_update(milliseconds(0)));

  // The link can start a fresh upload afterwards, and the other can too.
  CHECK(is(fx.request2(Msg::OtaBegin, u32le(64)), Msg::Ok));
}

TEST(service_mark_valid_clears_pending_verify) {
  OtaFixture fx("svc_markvalid");
  fx.ota.pending_verify = true;
  CHECK(is(fx.request(Msg::MarkValid), Msg::Ok));
  CHECK(!fx.ota.pending_verify);
  fx.ota.fail_mark_valid = true;
  CHECK(is(fx.request(Msg::MarkValid), Msg::Error));
}

// The console may confirm an image that is already valid (a retry, or a second
// console): that is a plain OK and leaves it valid.
TEST(service_mark_valid_on_a_confirmed_image_is_a_no_op_ok) {
  OtaFixture fx("svc_markvalid_noop");
  fx.ota.pending_verify = false;
  const auto reply = fx.request(Msg::MarkValid);
  CHECK(is(reply, Msg::Ok));
  CHECK_EQ(stream::get_u32(reply.payload), 0u);
  CHECK(!fx.ota.pending_verify);
  CHECK(!fx.held());
  CHECK_EQ(fx.ota.rollback_calls, 0);
}

// esp_ota_begin() refuses while the running image is pending, and espp::Ota
// would only report io_error; the service says why instead.
TEST(service_ota_begin_refused_while_the_running_image_is_pending) {
  OtaFixture fx("svc_ota_pending");
  fx.ota.pending_verify = true;
  const auto reply = fx.request(Msg::OtaBegin, u32le(64));
  CHECK(is(reply, Msg::Error));
  CHECK_EQ(stream::get_u32(reply.payload),
           static_cast<uint32_t>(std::errc::operation_not_permitted));
  CHECK(support::error_message(reply).find("not confirmed yet") != std::string::npos);
  CHECK(support::error_message(reply).find("MARK_VALID") != std::string::npos);
  CHECK_EQ(fx.ota.begin_calls, 0);
  CHECK(!fx.ota.session_active());
  CHECK(!fx.held());

  // Once confirmed, the upload can start.
  CHECK(is(fx.request(Msg::MarkValid), Msg::Ok));
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
}

TEST(service_restart_requires_callback_and_empty_payload) {
  OtaFixture none("svc_restart_none", /*wire_ota=*/false, /*wire_restart=*/false);
  CHECK(is(none.request(Msg::Restart), Msg::Error));

  OtaFixture fx("svc_restart", /*wire_ota=*/false, /*wire_restart=*/true);
  CHECK(is(fx.request(Msg::Restart, {1}), Msg::Error));
  CHECK(!fx.restart_ran.load());

  CHECK(is(fx.request(Msg::Restart), Msg::Ok));
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  CHECK(fx.restart_ran.load());
}

// The polling loops pass OTA_IDLE_TIMEOUT. Its value is what
// docs/firmware-updates.md documents, and the session survives right up to it.
TEST(service_expire_stale_update_at_the_real_idle_timeout) {
  using namespace std::chrono;
  CHECK(CalibrationService::OTA_IDLE_TIMEOUT == seconds(30));
  OtaFixture fx("svc_expire_timeout");
  const auto timeout = CalibrationService::OTA_IDLE_TIMEOUT;
  const auto begin_sent = steady_clock::now();
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));

  // Idle for just under the timeout (measured from before the request, so the
  // real last activity is no earlier than that): kept.
  CHECK(!fx.service.expire_stale_update(timeout, begin_sent + timeout - milliseconds(1)));
  CHECK(fx.ota.session_active());
  CHECK(fx.held());

  // OTA_DATA restarts the idle time.
  const auto data_sent = steady_clock::now();
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {1, 2})), Msg::Ok));
  const auto data_done = steady_clock::now();
  CHECK(!fx.service.expire_stale_update(timeout, data_sent + timeout - milliseconds(1)));
  CHECK(fx.ota.session_active());

  // At the timeout (measured from after the last request): aborted.
  CHECK(fx.service.expire_stale_update(timeout, data_done + timeout));
  CHECK(!fx.ota.session_active());
  CHECK_EQ(fx.ota.abort_calls, 1);
  CHECK(!fx.held());
}

// ---- output hold (D22 rule 1) ---------------------------------------------------

// begin() erases the slot, which takes seconds on the device: the hold must be
// up before it starts, and come down again if no session began.
TEST(service_output_hold_is_raised_before_ota_begin_erases) {
  OtaFixture fx("svc_hold_before_begin");
  bool held_in_begin = false;
  fx.ota.before_begin = [&] { held_in_begin = fx.held(); };
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  CHECK(held_in_begin);
  CHECK(fx.held());
  CHECK(is(fx.request(Msg::OtaAbort), Msg::Ok));
  CHECK(!fx.held());

  // A begin() that fails releases the hold it raised.
  held_in_begin = false;
  fx.ota.fail_next_begin = true;
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Error));
  CHECK(held_in_begin);
  CHECK(!fx.held());

  // After a takeover the hold was already up; a failed restart of the upload
  // ends the stale session and the hold with it.
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  fx.ota.fail_next_begin = true;
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Error));
  CHECK(!fx.ota.session_active());
  CHECK(!fx.held());
}

TEST(service_output_hold_follows_an_ota_session) {
  OtaFixture fx("svc_hold_session");
  CHECK(!fx.held());

  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  CHECK(fx.held());

  // Held through the data transfer.
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {1, 2, 3})), Msg::Ok));
  CHECK(fx.held());
  CHECK(is(fx.request(Msg::OtaData, ota_data(3, {4, 5})), Msg::Ok));
  CHECK(fx.held());

  CHECK(is(fx.request(Msg::OtaAbort), Msg::Ok));
  CHECK(!fx.held());
}

TEST(service_output_hold_is_released_when_the_session_is_torn_down) {
  OtaFixture fx("svc_hold_teardown");

  // Idle expiry.
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  CHECK(fx.held());
  CHECK(fx.service.expire_stale_update(std::chrono::milliseconds(0)));
  CHECK(!fx.held());

  // A write failure.
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  fx.ota.fail_next_write = true;
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {1, 2})), Msg::Error));
  CHECK(!fx.held());

  // The transport dropping.
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  CHECK(fx.held());
  fx.service.abort_update();
  CHECK(!fx.held());
}

TEST(service_output_hold_ignores_a_refused_ota_begin) {
  OtaFixture fx("svc_hold_refused");
  CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
  CHECK(fx.held());
  // The other service's OTA_BEGIN is refused and must not release the hold
  // the first service's session still needs.
  CHECK(is(fx.request2(Msg::OtaBegin, u32le(64)), Msg::Error));
  CHECK(fx.held());
  // Nor does a non-owner's expiry or abort.
  CHECK(!fx.service2.expire_stale_update(std::chrono::milliseconds(0)));
  CHECK(is(fx.request2(Msg::OtaAbort), Msg::Ok));
  CHECK(fx.held());
}

TEST(service_output_hold_is_counted_per_service) {
  OtaFixture fx("svc_hold_counted");
  CHECK(is(fx.request(Msg::OtaBegin, u32le(4)), Msg::Ok));
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {1, 2, 3, 4})), Msg::Ok));
  CHECK(is(fx.request(Msg::OtaFinish), Msg::Ok));
  // BEGIN and FINISH both ask for the hold; only the first edge is reported.
  CHECK(fx.output_holds.load() == 1);
  // While the first service waits to reboot, the second starts and abandons
  // an upload of its own. That must not end the first service's hold.
  CHECK(is(fx.request2(Msg::OtaBegin, u32le(64)), Msg::Ok));
  CHECK(fx.output_holds.load() == 2);
  CHECK(is(fx.request2(Msg::OtaAbort), Msg::Ok));
  CHECK(fx.output_holds.load() == 1);
  // Let the first service's deferred reboot run before the fixture goes.
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  CHECK(fx.ota.restart_called.load());
}

TEST(service_output_hold_stays_through_a_successful_finish) {
  OtaFixture fx("svc_hold_finish_ok");
  CHECK(is(fx.request(Msg::OtaBegin, u32le(4)), Msg::Ok));
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {1, 2, 3, 4})), Msg::Ok));
  CHECK(is(fx.request(Msg::OtaFinish), Msg::Ok));
  CHECK(!fx.ota.session_active());
  // The session is over but the reboot is pending: still neutral.
  CHECK(fx.held());
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  CHECK(fx.ota.restart_called.load());
  CHECK(fx.held());
}

TEST(service_output_hold_is_released_when_finish_fails) {
  OtaFixture fx("svc_hold_finish_fail");
  CHECK(is(fx.request(Msg::OtaBegin, u32le(4)), Msg::Ok));
  CHECK(is(fx.request(Msg::OtaData, ota_data(0, {1, 2, 3, 4})), Msg::Ok));
  CHECK(fx.held());
  fx.ota.fail_next_finish = true;
  CHECK(is(fx.request(Msg::OtaFinish), Msg::Error));
  CHECK(!fx.ota.restart_called.load());
  CHECK(!fx.held());
}

TEST(service_output_hold_is_raised_before_a_console_reboot) {
  {
    OtaFixture fx("svc_hold_restart", /*wire_ota=*/false, /*wire_restart=*/true);
    CHECK(is(fx.request(Msg::Restart, {1}), Msg::Error)); // refused: no hold
    CHECK(!fx.held());
    CHECK(is(fx.request(Msg::Restart), Msg::Ok));
    CHECK(fx.held());
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    CHECK(fx.restart_ran.load());
  }
  {
    OtaFixture fx("svc_hold_revert");
    CHECK(is(fx.request(Msg::Revert, {1}), Msg::Error));
    CHECK(!fx.held());
    CHECK(is(fx.request(Msg::Revert), Msg::Ok));
    CHECK(fx.held());
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    CHECK(fx.ota.restart_called.load());
  }
  {
    // REVERT over an abandoned upload that then cannot reboot: nothing stays held.
    OtaFixture fx("svc_hold_revert_fail");
    CHECK(is(fx.request(Msg::OtaBegin, u32le(64)), Msg::Ok));
    CHECK(fx.held());
    esp_ota_fake().other_has_image = false;
    CHECK(is(fx.request(Msg::Revert), Msg::Error));
    CHECK(!fx.held());
  }
}

TEST(service_output_hold_is_raised_before_an_input_mode_switch) {
  OtaFixture fx("svc_hold_mode", /*wire_ota=*/false);
  // Already the active mode: no switch, no reboot, no hold.
  CHECK(is(fx.request(Msg::SetInputMode, {0}), Msg::Ok));
  CHECK(!fx.held());
  CHECK(is(fx.request(Msg::SetInputMode, {1}), Msg::Ok));
  CHECK(fx.held());
}

// A reply the transport refused never reaches the host, so the deferred
// reboot is dropped and the output must not stay neutral for a reboot that
// is not coming.
TEST(service_output_hold_is_released_when_the_deferred_reboot_is_dropped) {
  TempDir dir("svc_hold_dropped");
  OmegaStore::Config cfg;
  cfg.calibration_path = dir.file("omega_calibration.cfg");
  cfg.settings_path = dir.file("omega_settings.cfg");
  OmegaStore store(cfg);
  bool held = false;
  int raised = 0;
  bool accept = false;
  CalibrationService svc({.store = store,
                          .send = [&](std::span<const uint8_t>) { return accept; },
                          .restart = [] {},
                          .output_hold =
                              [&](bool hold) {
                                held = hold;
                                raised += hold ? 1 : 0;
                              }});
  CHECK(svc.handle_frame(static_cast<uint8_t>(Msg::Restart), {}));
  CHECK_EQ(raised, 1); // raised for the reboot...
  CHECK(!held);        // ...and lowered again once the reply was refused
}

// A reply that was not queued drops its deferred reboot, but an upload this
// link is still running keeps the output held until the upload ends.
TEST(service_dropped_reboot_reply_keeps_the_hold_of_a_running_upload) {
  TempDir dir("svc_hold_dropped_upload");
  OmegaStore::Config cfg;
  cfg.calibration_path = dir.file("omega_calibration.cfg");
  cfg.settings_path = dir.file("omega_settings.cfg");
  OmegaStore store(cfg);
  espp::Ota ota(espp::Ota::Config{});
  esp_ota_fake().reset();
  int holds = 0;
  bool accept = true;
  std::atomic<bool> restarted{false};
  CalibrationService svc({.store = store,
                          .send = [&](std::span<const uint8_t>) { return accept; },
                          .restart = [&] { restarted = true; },
                          .ota = &ota,
                          .output_hold = [&](bool hold) { holds += hold ? 1 : -1; }});
  const auto size = u32le(64);
  CHECK(svc.handle_frame(static_cast<uint8_t>(Msg::OtaBegin), size));
  CHECK(ota.session_active());
  CHECK_EQ(holds, 1);

  accept = false; // the transport refuses the RESTART's OK
  CHECK(svc.handle_frame(static_cast<uint8_t>(Msg::Restart), {}));
  CHECK_EQ(holds, 1); // the upload still needs it
  CHECK(ota.session_active());

  // The upload ending releases it as usual.
  accept = true;
  CHECK(svc.expire_stale_update(std::chrono::milliseconds(0)));
  CHECK_EQ(holds, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  CHECK(!restarted.load()); // the dropped reboot never runs
}

// ---- visual calibration (protocol version 2) --------------------------------

namespace {

using Progress = CalibrationService::Progress;
using Stage = espp::OmegaCalibration::Stage;

/// A stage-6 report with a few sectors covered and every field distinct.
Progress sample_progress() {
  Progress p;
  p.stage = Stage::RANGE_SWEEP;
  p.raw_valid = true;
  p.center_valid = true;
  p.coverage_complete = false;
  p.finish_available = true;
  p.stage_elapsed_ms = 1250;
  p.stage_total_ms = 0;
  p.raw_x = 31.5f;
  p.raw_y = -2.25f;
  p.center_x = 12.5f;
  p.center_y = -3.25f;
  p.noise_radius = 0.75f;
  p.min_x = -4.0f;
  p.max_x = 19.0f;
  p.min_y = -0.5f;
  p.max_y = 8.0f;
  p.covered_mask = 0x0000000Fu;
  for (size_t i = 0; i < 4; ++i) {
    p.sector_max_radius[i] = 19.0f + static_cast<float>(i);
  }
  p.sector_max_radius[31] = 1.5f;
  return p;
}

} // namespace

TEST(service_advertises_protocol_version_2) {
  Fixture fx("svc_proto_v2");
  CHECK_EQ(CalibrationService::kProtocolVersion, uint16_t{2});
  CHECK_EQ(fx.service.module_info().protocol_version, uint16_t{2});
}

TEST(service_calibration_progress_has_the_documented_layout) {
  Fixture fx("svc_progress");
  fx.sent.clear();
  fx.service.notify_calibration_progress(sample_progress());
  const auto f = support::decode_one(fx.sent);
  CHECK(f.has_value());
  CHECK(is(*f, Msg::CalibrationProgress));
  CHECK(f->is_reply());
  CHECK_EQ(f->module, CalibrationService::kModule);
  const auto &p = f->payload;
  const std::span<const uint8_t> s(p);
  CHECK_EQ(p.size(), size_t{179});
  CHECK_EQ(p.size(), CalibrationService::kProgressPayloadSize);
  CHECK_EQ(p[0], 6);    // stage: range_sweep
  CHECK_EQ(p[1], 0x0B); // raw_valid | center_valid | finish_available
  CHECK_EQ(stream::get_u32(s.subspan(2)), 1250u);
  CHECK_EQ(stream::get_u32(s.subspan(6)), 0u);
  CHECK_NEAR(support::get_f32(p, 10), 31.5f, 0.0);  // raw_x
  CHECK_NEAR(support::get_f32(p, 14), -2.25f, 0.0); // raw_y
  CHECK_NEAR(support::get_f32(p, 18), 12.5f, 0.0);  // center_x
  CHECK_NEAR(support::get_f32(p, 22), -3.25f, 0.0); // center_y
  CHECK_NEAR(support::get_f32(p, 26), 0.75f, 0.0);  // noise_radius
  CHECK_NEAR(support::get_f32(p, 30), -4.0f, 0.0);  // min_x
  CHECK_NEAR(support::get_f32(p, 34), 19.0f, 0.0);  // max_x
  CHECK_NEAR(support::get_f32(p, 38), -0.5f, 0.0);  // min_y
  CHECK_NEAR(support::get_f32(p, 42), 8.0f, 0.0);   // max_y
  CHECK_EQ(stream::get_u32(s.subspan(46)), 0x0Fu);  // covered_mask
  CHECK_EQ(p[50], 32);                              // sector_count
  CHECK_NEAR(support::get_f32(p, 51), 19.0f, 0.0);  // sector 0
  CHECK_NEAR(support::get_f32(p, 51 + 3 * 4), 22.0f, 0.0);
  CHECK_NEAR(support::get_f32(p, 51 + 4 * 4), 0.0f, 0.0);
  CHECK_NEAR(support::get_f32(p, 51 + 31 * 4), 1.5f, 0.0); // the last 4 bytes

  // Each flag bit on its own.
  const auto byte_of = [](const Progress &r, size_t i) -> uint8_t {
    return CalibrationService::build_progress_payload(r)[i];
  };
  Progress q;
  q.coverage_complete = true;
  CHECK_EQ(byte_of(q, 1), 0x04);
  q = Progress{};
  q.center_valid = true;
  CHECK_EQ(byte_of(q, 1), 0x02);
  q = Progress{};
  CHECK_EQ(byte_of(q, 0), 0); // stage 0: live
  CHECK_EQ(byte_of(q, 1), 0x00);
}

TEST(service_progress_refused_by_the_transport_is_dropped_quietly) {
  TempDir dir("svc_progress_refused");
  OmegaStore store({.calibration_path = dir.file("c.cfg"), .settings_path = dir.file("s.cfg")});
  int calls = 0;
  CalibrationService svc({.store = store, .send = [&](std::span<const uint8_t>) {
                            ++calls;
                            return false;
                          }});
  svc.notify_calibration_progress(sample_progress()); // no retry, no throw
  CHECK_EQ(calls, 1);
}

TEST(service_finish_calibration_reply_rules) {
  {
    Fixture fx("svc_finish_nohook");
    const auto reply = fx.request(Msg::FinishCalibration);
    CHECK(is(reply, Msg::Error));
    CHECK_EQ(stream::get_u32(reply.payload),
             static_cast<uint32_t>(std::errc::function_not_supported));
  }
  TempDir dir("svc_finish");
  OmegaStore store({.calibration_path = dir.file("c.cfg"), .settings_path = dir.file("s.cfg")});
  std::vector<uint8_t> sent;
  bool sampling = false;
  int asked = 0;
  CalibrationService svc({.store = store,
                          .send =
                              [&](std::span<const uint8_t> f) {
                                sent.assign(f.begin(), f.end());
                                return true;
                              },
                          .finish_calibration =
                              [&] {
                                ++asked;
                                return sampling;
                              }});
  auto request = [&](std::vector<uint8_t> payload) {
    sent.clear();
    CHECK(svc.handle_frame(static_cast<uint8_t>(Msg::FinishCalibration), payload));
    auto f = support::decode_one(sent);
    CHECK(f.has_value());
    return *f;
  };

  auto f = request({});
  CHECK(is(f, Msg::Error));
  CHECK_EQ(stream::get_u32(f.payload), static_cast<uint32_t>(std::errc::operation_not_permitted));
  CHECK(support::error_message(f).find("no range sweep is running") != std::string::npos);

  sampling = true;
  f = request({});
  CHECK(is(f, Msg::Ok));
  CHECK_EQ(stream::get_u32(f.payload), 0u);
  CHECK_EQ(asked, 2);

  f = request({1}); // a payload is refused before the hook is asked
  CHECK(is(f, Msg::Error));
  CHECK_EQ(stream::get_u32(f.payload), static_cast<uint32_t>(std::errc::invalid_argument));
  CHECK_EQ(asked, 2);
}

TEST(service_position_stream_is_a_lease) {
  Fixture fx("svc_position_lease");
  using std::chrono::milliseconds;
  using steady = std::chrono::steady_clock;
  CHECK(!fx.service.position_stream_active());

  // Start: active until lease_ms after the request.
  auto before = steady::now();
  auto reply = fx.request(Msg::SetPositionStream, u32le(1000));
  auto after = steady::now();
  CHECK(is(reply, Msg::Ok));
  CHECK_EQ(stream::get_u32(reply.payload), 0u);
  CHECK(fx.service.position_stream_active(before + milliseconds(999)));
  CHECK(!fx.service.position_stream_active(after + milliseconds(1000)));

  // Renew: the new lease runs from the renewal.
  before = steady::now();
  CHECK(is(fx.request(Msg::SetPositionStream, u32le(3000)), Msg::Ok));
  CHECK(fx.service.position_stream_active(before + milliseconds(2999)));

  // Stop: 0 ends it at once.
  CHECK(is(fx.request(Msg::SetPositionStream, u32le(0)), Msg::Ok));
  CHECK(!fx.service.position_stream_active());
  CHECK(!fx.service.position_stream_active(steady::time_point{}));

  // Clamp: a lease longer than MAX_POSITION_STREAM_LEASE gets 10 s.
  before = steady::now();
  CHECK(is(fx.request(Msg::SetPositionStream, u32le(0xFFFFFFFFu)), Msg::Ok));
  after = steady::now();
  CHECK(fx.service.position_stream_active(before + milliseconds(9999)));
  CHECK(!fx.service.position_stream_active(after + CalibrationService::MAX_POSITION_STREAM_LEASE));

  // Wrong payload lengths are refused and leave the lease alone.
  for (const std::vector<uint8_t> &bad : {std::vector<uint8_t>{}, std::vector<uint8_t>{1, 2, 3},
                                          std::vector<uint8_t>{1, 2, 3, 4, 5}}) {
    reply = fx.request(Msg::SetPositionStream, bad);
    CHECK(is(reply, Msg::Error));
    CHECK_EQ(stream::get_u32(reply.payload), static_cast<uint32_t>(std::errc::invalid_argument));
  }
  CHECK(fx.service.position_stream_active(before + milliseconds(9999)));
}

TEST(service_notify_position_sends_only_while_the_lease_runs) {
  Fixture fx("svc_position_notify");
  using std::chrono::milliseconds;
  const auto live = espp::OmegaCalibration::live_progress(true, 1.5f, -2.5f);

  fx.sent.clear();
  CHECK(!fx.service.notify_position(live)); // no lease yet
  CHECK(fx.sent.empty());

  const auto start = std::chrono::steady_clock::now();
  CHECK(is(fx.request(Msg::SetPositionStream, u32le(500)), Msg::Ok));
  fx.sent.clear();
  CHECK(fx.service.notify_position(live, start));
  const auto f = support::decode_one(fx.sent);
  CHECK(f && is(*f, Msg::CalibrationProgress));
  CHECK_EQ(f->payload[0], 0);    // stage 0: live
  CHECK_EQ(f->payload[1], 0x01); // raw_valid only
  CHECK_NEAR(support::get_f32(f->payload, 10), 1.5f, 0.0);

  // Expired: nothing more is sent.
  fx.sent.clear();
  CHECK(!fx.service.notify_position(live, std::chrono::steady_clock::now() + milliseconds(600)));
  CHECK(fx.sent.empty());
}

TEST(service_position_lease_is_per_service) {
  TempDir dir("svc_position_two");
  OmegaStore store({.calibration_path = dir.file("c.cfg"), .settings_path = dir.file("s.cfg")});
  std::vector<uint8_t> vendor_sent, cdc_sent;
  CalibrationService vendor({.store = store, .send = [&](std::span<const uint8_t> f) {
                               vendor_sent.insert(vendor_sent.end(), f.begin(), f.end());
                               return true;
                             }});
  CalibrationService cdc({.store = store, .send = [&](std::span<const uint8_t> f) {
                            cdc_sent.insert(cdc_sent.end(), f.begin(), f.end());
                            return true;
                          }});
  CHECK(vendor.handle_frame(static_cast<uint8_t>(Msg::SetPositionStream), u32le(1000)));
  vendor_sent.clear();
  const auto live = espp::OmegaCalibration::live_progress(true, 1.0f, 2.0f);
  CHECK(vendor.notify_position(live));
  CHECK(!cdc.notify_position(live));
  CHECK(!vendor_sent.empty());
  CHECK(cdc_sent.empty());
}
