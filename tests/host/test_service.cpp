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
  CalibrationService svc(
      {.store = store,
       .send = [&](std::span<const uint8_t> f) { sent.assign(f.begin(), f.end()); },
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
                                   }};
    if (wire_ota) {
      cfg.ota = &ota;
    }
    if (wire_restart) {
      cfg.restart = [this] { restart_ran = true; };
    }
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
  auto &fake = esp_ota_fake();
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
