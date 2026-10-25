// Golden frames shared with the browser console tests.
//
// Drives every request the calibration console can send through the real
// espp Dispatcher + CalibrationService (so through the real stream_frame
// parser) from a fixed store state, captures the exact bytes of each request
// and reply, and compares the resulting JSON byte-for-byte with the committed
// tests/protocol/golden_frames.json. tests/web/golden.test.js reads the same
// file and checks omega_protocol.js against it, so the two languages cannot
// drift apart silently.
//
// Regenerate after an intentional protocol change:
//   OMEGA_WRITE_GOLDEN=1 build-host/omega_host_tests
//
// Determinism: the service emits no timestamps or uptime fields. The one
// platform-dependent part is the ERROR reply, whose message ends with the C++
// library's text for std::errc::invalid_argument ("Invalid argument" on glibc,
// "invalid argument" on MSVC). That suffix is rewritten to a fixed string and
// the frame is re-encoded, so the file is identical on every platform.

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "calibration_service.hpp"
#include "dispatcher.hpp"
#include "omega_store.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

#ifndef OMEGA_PROTOCOL_DIR
#error "OMEGA_PROTOCOL_DIR must point at tests/protocol (set in tests/host/CMakeLists.txt)"
#endif

using espp::CalibrationService;
using espp::OmegaStore;
namespace stream = espp::stream_frame;

namespace {

using Msg = CalibrationService::Msg;

/// True when the environment variable is set (std::getenv is deprecated on the Windows CRT).
bool env_set(const char *name) {
#if defined(_MSC_VER)
  size_t size = 0;
  getenv_s(&size, nullptr, 0, name);
  return size > 0;
#else
  return std::getenv(name) != nullptr;
#endif
}

std::string hex(std::span<const uint8_t> bytes) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const uint8_t b : bytes) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0x0F]);
  }
  return out;
}

/// Shortest decimal text that round-trips the float (valid JSON number).
std::string num(float v) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  return std::string(buf, r.ptr);
}

std::string num(uint32_t v) { return std::to_string(v); }

std::string quote(const std::string &s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

/// Builds a JSON object body from key/value pairs whose values are already JSON text.
struct Obj {
  std::string text;
  Obj &add(const std::string &key, const std::string &json_value) {
    text += text.empty() ? "" : ", ";
    text += quote(key) + ": " + json_value;
    return *this;
  }
  std::string str() const { return "{" + text + "}"; }
};

struct RequestSpec {
  std::string name;
  uint8_t module;
  uint8_t type;
  std::vector<uint8_t> payload;
  std::string args; // JSON object: the arguments the JS encoder takes
};

struct Captured {
  std::string name;
  uint8_t type;
  uint8_t module;
  std::vector<uint8_t> frame;
  std::string fields;
};

/// Decoded DIAGNOSTICS fields, in the shape omega_protocol.js's decodeDiagnostics() returns.
std::string diagnostics_fields(const std::vector<uint8_t> &p) {
  const std::span<const uint8_t> s(p);
  Obj o;
  o.add("sensorFault", num(uint32_t{p[0]}));
  o.add("sensorFirstFault", num(uint32_t{p[1]}));
  o.add("sensorFaultCount", num(stream::get_u32(s.subspan(2))));
  o.add("bootCount", num(stream::get_u32(s.subspan(6))));
  std::string resets = "[";
  size_t off = 11;
  for (unsigned i = 0; i < p[10]; i++) {
    Obj r;
    r.add("boot", num(stream::get_u32(s.subspan(off))));
    off += 4;
    r.add("reason", num(uint32_t{p[off]}));
    off += 1;
    r.add("abnormal", p[off] ? "true" : "false");
    off += 1;
    const size_t len = p[off];
    off += 1;
    r.add("name", "\"" + std::string(p.begin() + off, p.begin() + off + len) + "\"");
    off += len;
    resets += (i ? "," : "") + r.str();
  }
  o.add("resets", resets + "]");
  // Trailing u8 (D26); null when an older payload ends after the resets.
  o.add("calibrationState", off < p.size() ? num(uint32_t{p[off]}) : std::string("null"));
  return o.str();
}

std::string state_fields(const std::vector<uint8_t> &p) {
  using support::get_f32;
  static const char *const kFloatNames[14] = {"centerX",
                                              "centerY",
                                              "centerNoise",
                                              "centerDeadzoneAuto",
                                              "manualOffsetX",
                                              "manualOffsetY",
                                              "rangeMinX",
                                              "rangeMaxX",
                                              "rangeMinY",
                                              "rangeMaxY",
                                              "usableRadius",
                                              "reliableMaxRadius",
                                              "normalizedMaxRadius",
                                              "rangeDeadzoneAuto"};
  Obj o;
  for (size_t i = 0; i < 14; ++i) {
    o.add(kFloatNames[i], num(get_f32(p, i * 4)));
  }
  o.add("manualCenterDeadzone", p[56] ? num(get_f32(p, 57)) : "null");
  o.add("manualRangeDeadzone", p[61] ? num(get_f32(p, 62)) : "null");
  o.add("nudgeEnterThreshold", num(get_f32(p, 66)));
  o.add("nudgeExitThreshold", num(get_f32(p, 70)));
  o.add("nudgeTimeMs", num(stream::get_u32(std::span<const uint8_t>(p).subspan(74))));
  o.add("inputMode", std::to_string(p[78]));
  return o.str();
}

/// Rewrites the platform-specific std::errc text in an ERROR reply so the
/// golden file is portable. Returns the (possibly re-encoded) frame and the
/// fields object.
Captured portable_error(const std::string &name, const stream::Frame &reply,
                        std::vector<uint8_t> frame) {
  const std::string native = std::make_error_code(std::errc::invalid_argument).message();
  std::string message = support::error_message(reply);
  CHECK(message.size() > native.size());
  CHECK_EQ(message.substr(message.size() - native.size()), native);
  message.resize(message.size() - native.size());
  message += "invalid argument";

  std::vector<uint8_t> payload;
  stream::put_u32(payload, stream::get_u32(reply.payload));
  payload.insert(payload.end(), message.begin(), message.end());
  frame = stream::build_frame(true, reply.module, reply.type, payload);

  Obj o;
  o.add("code", num(stream::get_u32(reply.payload))).add("message", quote(message));
  return {name, reply.type, reply.module, frame, o.str()};
}

std::string build_golden_json() {
  support::TempDir dir("golden");
  OmegaStore store({.calibration_path = dir.file("omega_calibration.cfg"),
                    .settings_path = dir.file("omega_settings.cfg")});
  // Fixed starting state: explicit calibration, overrides, nudge tuning, mode.
  store.set_calibration(support::sample_center(), support::sample_range());
  store.set_deadzone_override(std::nullopt, 0.09f);
  store.set_nudge_settings(0.45f, 0.75f, std::chrono::milliseconds(120));
  store.set_input_mode(OmegaStore::InputMode::XAC);

  std::vector<uint8_t> sent;
  auto transmit = [&sent](std::span<const uint8_t> f) {
    sent.insert(sent.end(), f.begin(), f.end());
    return true;
  };
  CalibrationService service({.store = store,
                              .send = transmit,
                              .start_calibration = [](std::error_code &) { return true; },
                              .diagnostics =
                                  [] {
                                    CalibrationService::DiagnosticsData d;
                                    d.sensor_fault = 2;
                                    d.sensor_first_fault = 1;
                                    d.sensor_fault_count = 3;
                                    d.boot_count = 42;
                                    d.resets.push_back({40, 9, true, "brownout"});
                                    d.resets.push_back({41, 1, false, "power-on"});
                                    d.resets.push_back({42, 3, false, "software"});
                                    d.calibration_state = CalibrationService::CALIBRATION_NEEDED;
                                    return d;
                                  }});
  espp::Dispatcher dispatcher;
  dispatcher.register_module(service);
  dispatcher.set_device_info("omega-stick", "golden");
  dispatcher.serve_discovery(transmit);

  const uint8_t module = CalibrationService::kModule;
  auto type_of = [](Msg m) { return static_cast<uint8_t>(m); };
  auto f32_payload = [](std::initializer_list<float> values) {
    std::vector<uint8_t> p;
    for (const float v : values) {
      support::put_f32(p, v);
    }
    return p;
  };

  std::vector<RequestSpec> requests;
  requests.push_back({"GET_STATE", module, type_of(Msg::GetState), {}, "{}"});
  requests.push_back({"GET_DIAGNOSTICS", module, type_of(Msg::GetDiagnostics), {}, "{}"});
  requests.push_back({"SET_MANUAL_OFFSET", module, type_of(Msg::SetManualOffset),
                      f32_payload({0.75f, -1.25f}),
                      Obj().add("x", num(0.75f)).add("y", num(-1.25f)).str()});
  {
    std::vector<uint8_t> p{0x03};
    support::put_f32(p, 0.04f);
    support::put_f32(p, 0.06f);
    requests.push_back({"SET_DEADZONE_OVERRIDE_BOTH", module, type_of(Msg::SetDeadzoneOverride), p,
                        Obj()
                            .add("centerOn", "true")
                            .add("centerVal", num(0.04f))
                            .add("rangeOn", "true")
                            .add("rangeVal", num(0.06f))
                            .str()});
  }
  {
    std::vector<uint8_t> p{0x02};
    support::put_f32(p, 0.0f);
    support::put_f32(p, 0.06f);
    requests.push_back({"SET_DEADZONE_OVERRIDE_RANGE_ONLY", module,
                        type_of(Msg::SetDeadzoneOverride), p,
                        Obj()
                            .add("centerOn", "false")
                            .add("centerVal", num(0.0f))
                            .add("rangeOn", "true")
                            .add("rangeVal", num(0.06f))
                            .str()});
  }
  {
    std::vector<uint8_t> p = f32_payload({0.35f, 0.65f});
    stream::put_u32(p, 150);
    requests.push_back({"SET_NUDGE_SETTINGS", module, type_of(Msg::SetNudgeSettings), p,
                        Obj()
                            .add("enter", num(0.35f))
                            .add("exit", num(0.65f))
                            .add("timeMs", num(uint32_t{150}))
                            .str()});
  }
  requests.push_back({"SET_INPUT_MODE_MOUSE",
                      module,
                      type_of(Msg::SetInputMode),
                      {0},
                      Obj().add("mode", "0").str()});
  requests.push_back({"GET_STATE_AFTER", module, type_of(Msg::GetState), {}, "{}"});
  requests.push_back({"SAVE_TO_FLASH", module, type_of(Msg::SaveToFlash), {}, "{}"});
  requests.push_back({"START_CALIBRATION", module, type_of(Msg::StartCalibration), {}, "{}"});
  requests.push_back({"LIST_MODULES",
                      espp::Dispatcher::kDiscoveryModule,
                      static_cast<uint8_t>(espp::Dispatcher::Discovery::ListModules),
                      {},
                      "{}"});
  requests.push_back({"SET_INPUT_MODE_INVALID",
                      module,
                      type_of(Msg::SetInputMode),
                      {2},
                      Obj().add("mode", "2").str()});

  std::vector<Captured> replies;
  std::string request_json;
  for (const auto &req : requests) {
    const auto frame = stream::build_frame(false, req.module, req.type, req.payload);
    sent.clear();
    dispatcher.feed(frame);
    const auto reply = support::decode_one(sent);
    CHECK(reply.has_value());
    CHECK(reply->is_reply());
    CHECK_EQ(reply->module, req.module);

    if (!request_json.empty()) {
      request_json += ",\n";
    }
    request_json += "    {\"name\": " + quote(req.name) +
                    ", \"module\": " + std::to_string(req.module) +
                    ", \"type\": " + std::to_string(req.type) + ", \"args\": " + req.args +
                    ", \"payload_hex\": " + quote(hex(req.payload)) +
                    ", \"frame_hex\": " + quote(hex(frame)) + "}";

    if (reply->type == type_of(Msg::State)) {
      replies.push_back({req.name, reply->type, reply->module, sent, state_fields(reply->payload)});
    } else if (reply->type == type_of(Msg::Diagnostics)) {
      replies.push_back(
          {req.name, reply->type, reply->module, sent, diagnostics_fields(reply->payload)});
    } else if (reply->type == type_of(Msg::Ok)) {
      replies.push_back({req.name, reply->type, reply->module, sent,
                         Obj().add("value", num(stream::get_u32(reply->payload))).str()});
    } else if (reply->type == type_of(Msg::Error)) {
      replies.push_back(portable_error(req.name, *reply, sent));
    } else if (req.module == espp::Dispatcher::kDiscoveryModule) {
      // Discovery: version, device, fw, then the one registered module.
      const auto info = service.module_info();
      Obj mod;
      mod.add("id", std::to_string(service.module_id()))
          .add("name", quote(std::string(info.name)))
          .add("app", quote(std::string(info.app)))
          .add("desc", quote(std::string(info.description)))
          .add("protocol", quote(std::string(info.protocol)))
          .add("protocolVersion", std::to_string(info.protocol_version));
      Obj o;
      o.add("version", std::to_string(espp::Dispatcher::kDiscoveryVersion))
          .add("device", quote("omega-stick"))
          .add("fw", quote("golden"))
          .add("modules", "[" + mod.str() + "]");
      replies.push_back({req.name, reply->type, reply->module, sent, o.str()});
    } else {
      throw harness::Failure("unexpected reply type for " + req.name);
    }
  }

  // Unsolicited device -> host frame (no request): recalibration finished.
  sent.clear();
  service.notify_calibration_result(true);
  const auto result = support::decode_one(sent);
  CHECK(result.has_value());
  CHECK_EQ(result->type, type_of(Msg::CalibrationResult));
  replies.push_back({"CALIBRATION_RESULT_OK", result->type, result->module, sent,
                     Obj().add("success", result->payload[0] ? "true" : "false").str()});

  std::string reply_json;
  for (const auto &r : replies) {
    if (!reply_json.empty()) {
      reply_json += ",\n";
    }
    reply_json += "    {\"name\": " + quote(r.name) + ", \"module\": " + std::to_string(r.module) +
                  ", \"type\": " + std::to_string(r.type) +
                  ", \"frame_hex\": " + quote(hex(r.frame)) + ", \"fields\": " + r.fields + "}";
  }

  return "{\n  \"requests\": [\n" + request_json + "\n  ],\n  \"replies\": [\n" + reply_json +
         "\n  ]\n}\n";
}

} // namespace

TEST(golden_frames_match_committed_file) {
  const std::string actual = build_golden_json();
  const std::filesystem::path path =
      std::filesystem::path(OMEGA_PROTOCOL_DIR) / "golden_frames.json";

  if (env_set("OMEGA_WRITE_GOLDEN")) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(actual.data(), static_cast<std::streamsize>(actual.size()));
    CHECK(out.good());
    return;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw harness::Failure("cannot open " + path.string() +
                           "; set OMEGA_WRITE_GOLDEN=1 and rerun to generate it");
  }
  std::string expected((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  // A Windows checkout may have converted line endings; the content is what matters.
  std::erase(expected, '\r');
  if (expected != actual) {
    throw harness::Failure(
        "golden frames differ from " + path.string() +
        "; if the protocol change is intentional, set OMEGA_WRITE_GOLDEN=1 and rerun to "
        "regenerate it, then review the diff (tests/web/golden.test.js reads the same file)");
  }
}
