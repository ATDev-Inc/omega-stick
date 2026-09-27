// CalibrationService: request decoding, replies, store side effects, and the
// framing path through the real espp stream_frame parser/builder.

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
                 }}) {}

  OmegaStore::Config with_paths(OmegaStore::Config c) {
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

XFAIL_TEST(service_ok_reply_is_sent_before_mode_change_callback_runs,
           "set_input_mode fires on_mode_changed synchronously inside handle_frame_locked, "
           "before the OK reply is built; main.cpp's callback calls esp_restart(), so the "
           "console never receives the OK and reports a timeout for a switch that worked") {
  int sends_seen_from_callback = -1;
  Fixture *self = nullptr;
  Fixture fx("svc_mode_order", {.on_mode_changed = [&](OmegaStore::InputMode) {
                sends_seen_from_callback = self->send_calls;
              }});
  self = &fx;

  CHECK(is(fx.request(Msg::SetInputMode, {1}), Msg::Ok));
  CHECK_EQ(sends_seen_from_callback, 1);
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
                          .send = [&](std::span<const uint8_t> f) {
                            sent.assign(f.begin(), f.end());
                          },
                          .start_calibration = [&](std::error_code &ec) {
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
