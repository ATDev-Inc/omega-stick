#include "script.hpp"

#include <array>
#include <cmath>

#include "sensor.hpp"
#include "sim_flash.hpp"

namespace sim {

namespace {

constexpr std::array<const char *, 8> kPatterns = {
    "rest", "circle", "calibrate", "nudge_right", "nudge_left", "nudge_up", "nudge_down", "wander"};
constexpr std::array<const char *, 11> kActions = {"recalibrate",      "save",
                                                   "restart",          "set_mode",
                                                   "set_deadzone",     "flash",
                                                   "revert",           "mark_valid",
                                                   "fw_info",          "plant_unusable_calibration",
                                                   "set_nudge_enabled"};
constexpr std::array<const char *, 5> kReplies = {"ok", "busy", "invalid", "unsupported", "error"};

template <typename Arr> bool contains(const Arr &arr, const std::string &s) {
  for (const char *item : arr) {
    if (s == item) {
      return true;
    }
  }
  return false;
}

struct Fail {
  std::string msg;
};

[[noreturn]] void fail(size_t index, const std::string &what) {
  throw Fail{"step " + std::to_string(index) + ": " + what};
}

double number_in(const json::Value &v, size_t index, const char *what, double lo, double hi) {
  if (!v.is_number() || !std::isfinite(v.as_number()) || v.as_number() < lo || v.as_number() > hi) {
    fail(index, std::string(what) + " must be a number from " + json::Value(lo).dump() + " to " +
                    json::Value(hi).dump());
  }
  return v.as_number();
}

Expect parse_expect(const json::Value &e, size_t index) {
  if (!e.is_object()) {
    fail(index, "expect must be an object");
  }
  Expect x;
  auto want_bool = [&](const json::Value &v, const std::string &key) {
    if (!v.is_bool()) {
      fail(index, "expect." + key + " must be true or false");
    }
    return v.as_bool();
  };
  auto want_choice = [&](const json::Value &v, const std::string &key,
                         std::initializer_list<const char *> choices) {
    if (v.is_string()) {
      for (const char *c : choices) {
        if (v.as_string() == c) {
          return v.as_string();
        }
      }
    }
    fail(index, "expect." + key + " has an unsupported value");
  };
  for (size_t i = 0; i < e.keys().size(); i++) {
    const std::string &key = e.keys()[i];
    const json::Value &v = e.at_index(i);
    if (key == "calibrated") {
      x.calibrated = want_bool(v, key);
    } else if (key == "center_max") {
      x.center_max = number_in(v, index, "expect.center_max", 0, 10);
    } else if (key == "mouse_moved") {
      x.mouse_moved = want_choice(v, key, {"right", "left", "up", "down", "none"});
    } else if (key == "clicked") {
      x.clicked = want_choice(v, key, {"left", "right", "none"});
    } else if (key == "mode") {
      x.mode = want_choice(v, key, {"mouse", "xac"});
    } else if (key == "sensor_fault") {
      x.sensor_fault = want_bool(v, key);
    } else if (key == "sensor_observed") {
      x.sensor_observed = want_bool(v, key);
    } else if (key == "neutral") {
      x.neutral = want_bool(v, key);
    } else if (key == "needs_calibration") {
      x.needs_calibration = want_bool(v, key);
    } else if (key == "fw_slot") {
      x.fw_slot = want_choice(v, key, {"ota_0", "ota_1"});
    } else if (key == "fw_pending") {
      x.fw_pending = want_bool(v, key);
    } else if (key == "fw_version") {
      if (!v.is_string()) {
        fail(index, "expect.fw_version must be a string");
      }
      x.fw_version = v.as_string();
    } else if (key == "xac_x") {
      x.xac_x = number_in(v, index, "expect.xac_x", -1, 1);
    } else if (key == "xac_y") {
      x.xac_y = number_in(v, index, "expect.xac_y", -1, 1);
    } else {
      fail(index, "unknown expect key '" + key + "'");
    }
  }
  return x;
}

Step parse_step(const json::Value &s, size_t index) {
  if (!s.is_object()) {
    fail(index, "must be an object");
  }
  Step st;
  const json::Value *at = s.find("at_ms");
  if (!at) {
    fail(index, "missing at_ms");
  }
  st.at_ms =
      static_cast<int64_t>(number_in(*at, index, "at_ms", 0, static_cast<double>(kMaxScriptMs)));

  static const std::array<const char *, 8> kKinds = {"stick", "pattern",    "fault",  "drift",
                                                     "noise", "next_image", "action", "expect"};
  const json::Value *body = nullptr;
  std::string body_key;
  for (size_t i = 0; i < s.keys().size(); i++) {
    const std::string &key = s.keys()[i];
    if (key == "at_ms" || key == "duration_ms" || key == "expect_reply" || key == "value" ||
        key == "version") {
      continue;
    }
    if (!contains(kKinds, key)) {
      fail(index, "unknown key '" + key + "'");
    }
    if (body) {
      fail(index, "needs exactly one of stick, pattern, fault, drift, noise, next_image, action, "
                  "expect");
    }
    body = &s.at_index(i);
    body_key = key;
  }
  if (!body) {
    fail(index, "needs one of stick, pattern, fault, drift, noise, next_image, action, expect");
  }

  if (body_key == "stick") {
    st.kind = Step::Kind::Stick;
    if (!body->is_array() || body->items().size() != 2) {
      fail(index, "stick must be [x, y]");
    }
    st.x = number_in(body->items()[0], index, "stick x", -1, 1);
    st.y = number_in(body->items()[1], index, "stick y", -1, 1);
  } else if (body_key == "pattern") {
    st.kind = Step::Kind::Pattern;
    if (!body->is_string() || !is_known_pattern(body->as_string())) {
      fail(index, "unknown pattern");
    }
    st.name = body->as_string();
    if (const json::Value *d = s.find("duration_ms")) {
      st.duration_ms = static_cast<int64_t>(
          number_in(*d, index, "duration_ms", 0, static_cast<double>(kMaxScriptMs)));
    }
  } else if (body_key == "fault") {
    st.kind = Step::Kind::Fault;
    if (!body->is_string() || !parse_fault(body->as_string())) {
      fail(index, "fault must be ok, no_response, stuck_counter or weak_magnet");
    }
    st.name = body->as_string();
  } else if (body_key == "drift") {
    st.kind = Step::Kind::Drift;
    st.number = number_in(*body, index, "drift", 0, kMaxDriftRate);
  } else if (body_key == "noise") {
    st.kind = Step::Kind::Noise;
    st.number = number_in(*body, index, "noise", 0, kMaxNoiseSigma);
  } else if (body_key == "next_image") {
    st.kind = Step::Kind::NextImage;
    NextImage ignored;
    if (!body->is_string() || !parse_next_image(body->as_string(), ignored)) {
      fail(index, "next_image must be ok, fail_self_test, crash_loop, slow_sensor_start, "
                  "slow_sensor_fail or crash_when_confirmed");
    }
    st.name = body->as_string();
  } else if (body_key == "expect") {
    st.kind = Step::Kind::Expect;
    st.expect = parse_expect(*body, index);
  } else { // action
    st.kind = Step::Kind::Action;
    if (!body->is_string() || !contains(kActions, body->as_string())) {
      fail(index, "unknown action");
    }
    st.name = body->as_string();
    if (const json::Value *r = s.find("expect_reply")) {
      if (!r->is_string() || !contains(kReplies, r->as_string())) {
        fail(index, "expect_reply must be ok, busy, invalid, unsupported or error");
      }
      st.expect_reply = r->as_string();
    }
    const json::Value *value = s.find("value");
    if (st.name == "set_mode") {
      if (!value || !value->is_string() ||
          (value->as_string() != "mouse" && value->as_string() != "xac")) {
        fail(index, "set_mode needs value \"mouse\" or \"xac\"");
      }
      st.text = value->as_string();
    } else if (st.name == "set_nudge_enabled") {
      if (!value || !value->is_bool()) {
        fail(index, "set_nudge_enabled needs value true or false");
      }
      st.text = value->as_bool() ? "on" : "off";
    } else if (st.name == "set_deadzone") {
      if (!value || !value->is_object() || value->size() == 0) {
        fail(index, "set_deadzone needs value {\"center\": n, \"range\": n}");
      }
      // Out-of-limit values are allowed only when the step expects the firmware to
      // refuse them, so a scenario can test the firmware's own check.
      const bool on_purpose = st.expect_reply && *st.expect_reply == "invalid";
      for (const std::string &k : value->keys()) {
        if (k != "center" && k != "range") {
          fail(index, "set_deadzone value has an unknown key '" + k + "'");
        }
      }
      auto limited = [&](const char *key, double hi) -> std::optional<double> {
        const json::Value *v = value->find(key);
        if (!v) {
          return std::nullopt;
        }
        if (on_purpose) {
          if (!v->is_number() || !std::isfinite(v->as_number())) {
            fail(index, std::string("set_deadzone ") + key + " must be a number");
          }
          return v->as_number();
        }
        return number_in(*v, index, (std::string("set_deadzone ") + key).c_str(), 0, hi);
      };
      st.center = limited("center", kMaxCenterDeadzone);
      st.range = limited("range", kMaxRangeDeadzone);
    } else if (st.name == "flash") {
      st.text = s.string_or("version", "9.9.9-sim");
      if (st.text.empty() || st.text.size() > 31) {
        fail(index, "flash version must be 1 to 31 characters");
      }
    }
  }
  return st;
}

} // namespace

bool is_known_pattern(const std::string &name) { return contains(kPatterns, name); }

bool parse_script(const json::Value &doc, Script &out, std::string &error) {
  try {
    if (!doc.is_object()) {
      throw Fail{"a script must be a JSON object"};
    }
    if (const json::Value *v = doc.find("version"); v && (!v->is_number() || v->as_number() != 1)) {
      throw Fail{"unsupported script version"};
    }
    const json::Value *steps = doc.find("steps");
    if (!steps || !steps->is_array()) {
      throw Fail{"a script needs a steps array"};
    }
    if (steps->items().size() > kMaxScriptSteps) {
      throw Fail{"too many steps (limit " + std::to_string(kMaxScriptSteps) + ")"};
    }
    Script script;
    script.name = doc.string_or("name", "script");
    int64_t last = 0;
    for (size_t i = 0; i < steps->items().size(); i++) {
      Step st = parse_step(steps->items()[i], i);
      if (st.at_ms < last) {
        fail(i, "at_ms is not sorted (steps must be in time order)");
      }
      last = st.at_ms;
      script.steps.push_back(std::move(st));
    }
    out = std::move(script);
    return true;
  } catch (const Fail &f) {
    error = f.msg;
    return false;
  }
}

bool parse_script_text(const std::string &text, Script &out, std::string &error) {
  if (text.size() > kMaxScriptBytes) {
    error = "script is larger than 256 KiB";
    return false;
  }
  std::string perr;
  const auto doc = json::parse(text, &perr);
  if (!doc) {
    error = "script is not valid JSON: " + perr;
    return false;
  }
  return parse_script(*doc, out, error);
}

} // namespace sim
