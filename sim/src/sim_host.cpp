#include "sim_host.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "calibration_service.hpp"
#include "omega_calibration.hpp"
#include "ota.hpp"
#include "stream_frame.hpp"

namespace sim {

namespace {

using Msg = espp::CalibrationService::Msg;
namespace stream = espp::stream_frame;

std::vector<uint8_t> f32_bytes(float v) {
  std::vector<uint8_t> out(sizeof(float));
  std::memcpy(out.data(), &v, sizeof(float));
  return out;
}

/// The DIAGNOSTICS calibration_state byte after the reset list, or -1 when the
/// payload ends there (older firmware) or is malformed.
int diagnostics_calibration_state(const std::vector<uint8_t> &p) {
  if (p.size() < 11) {
    return -1;
  }
  size_t off = 11;
  for (unsigned i = 0; i < p[10]; i++) {
    if (off + 7 > p.size()) {
      return -1;
    }
    off += 7 + p[off + 6];
  }
  return off < p.size() ? p[off] : -1;
}

/// The calibration the maintainer's board saved after a first-boot sweep with
/// the stick untouched (D26): usable radius 0.049 against center noise 0.086,
/// in raw sensor units (mT).
bool plant_unusable_calibration(std::error_code &ec) {
  espp::OmegaCalibration::CenterResult center;
  center.noise_radius = 0.086f;
  center.deadzone = espp::OmegaCalibration::MAX_CENTER_DEADZONE;
  espp::OmegaCalibration::RangeResult range;
  range.min_x = -0.058f;
  range.max_x = 0.060f;
  range.min_y = -0.049f;
  range.max_y = 0.055f;
  range.usable_radius = 0.049f;
  range.deadzone = espp::OmegaCalibration::MAX_RANGE_DEADZONE;
  return espp::OmegaCalibration::save(
      center, range, espp::OmegaCalibration::default_path("omega_calibration.cfg"), ec);
}

} // namespace

Host::Host(const Options &options, Clock &clock, ws::Server *server)
    : options_(options)
    , clock_(clock)
    , server_(server) {}

// ---- time ----------------------------------------------------------------------------

void Host::sleep(std::chrono::milliseconds d) {
  clock_.sleep_for(d);
  pump();
  if (restart_pending_.load()) {
    throw RestartRequest{take_restart_reason()};
  }
}

void Host::pump() {
  run_script_steps();
  flush_telemetry();
  if (options_.headless && script_started_ && script_finished()) {
    exit_ = true; // headless runs end with the script
  }
}

// ---- restart ---------------------------------------------------------------------------

void Host::request_restart(const std::string &reason) {
  std::lock_guard<std::mutex> lock(restart_mu_);
  if (!restart_pending_.load()) {
    restart_reason_ = reason;
    restart_pending_ = true;
  }
}

std::string Host::take_restart_reason() {
  std::lock_guard<std::mutex> lock(restart_mu_);
  std::string r = restart_reason_.empty() ? "restart" : restart_reason_;
  restart_reason_.clear();
  restart_pending_ = false;
  return r;
}

void Host::wait_for_restart_real_time(std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!restart_pending_.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void Host::announce_rebooting(const std::string &reason) {
  if (server_ && server_->connected()) {
    json::Value m = json::Value::object();
    m.set("type", "rebooting").set("reason", reason);
    server_->send_text(m.dump());
    server_->close_client(1001);
  }
}

bool Host::take_wipe() { return wipe_.exchange(false); }

// ---- link and output -------------------------------------------------------------------

void Host::set_link(std::function<void(std::span<const uint8_t>)> feed) {
  std::lock_guard<std::mutex> lock(link_mu_);
  link_ = std::move(feed);
}

void Host::clear_link() {
  std::lock_guard<std::mutex> lock(link_mu_);
  link_ = nullptr;
}

bool Host::transmit(std::span<const uint8_t> frame) {
  {
    std::lock_guard<std::mutex> lock(capture_mu_);
    if (capturing_ && capture_thread_ == std::this_thread::get_id()) {
      capture_.insert(capture_.end(), frame.begin(), frame.end());
      return true;
    }
  }
  if (!server_) {
    return true; // headless: nobody to send to, nothing to refuse
  }
  return server_->send_binary(frame);
}

void Host::console_text(const std::string &text) {
  std::printf("%s\n", text.c_str());
  std::fflush(stdout);
  if (server_ && server_->connected()) {
    const std::string line = text + "\n";
    server_->send_binary(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(line.data()), line.size()));
  }
}

void Host::calibration_prompt(const std::string &text) {
  stick.note_prompt(text, now_ms());
  console_text(text);
}

void Host::on_mouse(const espp::omega_control::MouseReport &report) {
  const auto &r = report;
  std::lock_guard<std::mutex> lock(out_mu_);
  win_dx_ += r.dx;
  win_dy_ += r.dy;
  if ((r.buttons & 0x01) != 0 && (prev_buttons_ & 0x01) == 0) {
    win_left_ = true;
  }
  if ((r.buttons & 0x02) != 0 && (prev_buttons_ & 0x02) == 0) {
    win_right_ = true;
  }
  prev_buttons_ = r.buttons;
  if (r.dx != 0 || r.dy != 0 || r.wheel != 0 || r.buttons != 0) {
    win_neutral_ = false;
  }
  agg_dx_ += r.dx;
  agg_dy_ += r.dy;
  agg_wheel_ += r.wheel;
  agg_buttons_ = r.buttons;
  agg_dirty_ = true;
  last_was_xac_ = false;
}

void Host::on_xac(const espp::omega_control::XacReport &report) {
  const auto &r = report;
  std::lock_guard<std::mutex> lock(out_mu_);
  xac_x_ = r.x;
  xac_y_ = -r.y; // the firmware's y is negative for physical up; the page sees +y up
  if (std::fabs(r.x) > 1e-6 || std::fabs(r.y) > 1e-6) {
    win_neutral_ = false;
  }
  last_was_xac_ = true;
}

std::pair<StickPos, int64_t> Host::stick_now() {
  const int64_t t = now_ns();
  return {stick.position(t / 1'000'000), t};
}

// ---- page messages ------------------------------------------------------------------------

json::Value Host::hello_message() {
  json::Value m = json::Value::object();
  m.set("type", "hello").set("sim", "omega_sim").set("version", 1);
  m.set("fw", std::string(esp_app_get_description()->version));
  m.set("mode", live.mode.load() == 1 ? "xac" : "mouse");
  m.set("slot", flash().running_label());
  return m;
}

void Host::send_json(const json::Value &v) {
  if (server_) {
    server_->send_text(v.dump());
  }
}

void Host::on_connect() {
  {
    std::lock_guard<std::mutex> lock(out_mu_);
    sent_xac_x_ = sent_xac_y_ = 1e9;
    sent_buttons_ = 0;
    last_source_key_.clear();
  }
  send_json(hello_message());
}

void Host::on_binary(std::span<const uint8_t> data) {
  std::lock_guard<std::mutex> lock(link_mu_);
  if (link_) {
    link_(data);
  } // else: the stick is not listening yet (booting), like an unenumerated USB port
}

void Host::reject(const std::string &why) {
  json::Value m = json::Value::object();
  m.set("type", "error").set("message", why);
  send_json(m);
  std::fprintf(stderr, "omega_sim: rejected control message: %s\n", why.c_str());
}

void Host::on_text(const std::string &text) {
  std::string err;
  const auto msg = json::parse(text, &err);
  if (!msg || !msg->is_object()) {
    reject("control message is not a JSON object: " + err);
    return;
  }
  handle_control(*msg);
}

void Host::handle_control(const json::Value &msg) {
  const std::string type = msg.string_or("type");
  const int64_t now = now_ms();
  auto number = [&](const char *key, double lo, double hi, double &out) {
    const json::Value *v = msg.find(key);
    if (!v || !v->is_number() || !std::isfinite(v->as_number()) || v->as_number() < lo ||
        v->as_number() > hi) {
      reject(type + ": '" + key + "' must be a number from " + json::Value(lo).dump() + " to " +
             json::Value(hi).dump());
      return false;
    }
    out = v->as_number();
    return true;
  };

  if (type == "stick") {
    const json::Value *x = msg.find("x");
    const json::Value *y = msg.find("y");
    if (!x || !y || !x->is_number() || !y->is_number() || !std::isfinite(x->as_number()) ||
        !std::isfinite(y->as_number())) {
      reject("stick: x and y must be numbers");
      return;
    }
    stick.set_manual(x->as_number(), y->as_number()); // clamps to -1..1
  } else if (type == "pattern") {
    if (!stick.play_pattern(msg.string_or("name"), now)) {
      reject("pattern: unknown pattern '" + msg.string_or("name") + "'");
    }
  } else if (type == "script") {
    const json::Value *doc = msg.find("script");
    if (!doc) {
      reject("script: missing 'script'");
      return;
    }
    if (doc->dump().size() > kMaxScriptBytes) {
      reject("script: larger than 256 KiB");
      return;
    }
    Script script;
    std::string why;
    if (!parse_script(*doc, script, why)) {
      reject("script: " + why);
      return;
    }
    start_script(script);
  } else if (type == "stop") {
    stick.stop();
    std::lock_guard<std::mutex> lock(script_mu_);
    script_active_ = false;
  } else if (type == "fault") {
    const auto f = parse_fault(msg.string_or("sensor"));
    if (!f) {
      reject("fault: sensor must be ok, no_response, stuck_counter or weak_magnet");
      return;
    }
    sensor.set_fault(*f);
  } else if (type == "drift") {
    double rate = 0;
    if (number("rate", 0, kMaxDriftRate, rate)) {
      sensor.set_drift(rate, now_ns());
    }
  } else if (type == "noise") {
    double sigma = 0;
    if (number("sigma", 0, kMaxNoiseSigma, sigma)) {
      sensor.set_noise(sigma);
    }
  } else if (type == "next_image") {
    NextImage b;
    if (!parse_next_image(msg.string_or("behavior"), b)) {
      reject("next_image: behavior must be ok, fail_self_test, crash_loop, slow_sensor_start, "
             "slow_sensor_fail or crash_when_confirmed");
      return;
    }
    flash().set_next_image(b);
  } else if (type == "reset") {
    request_restart("reset");
  } else if (type == "wipe") {
    wipe_ = true;
    request_restart("wipe");
  } // hello and unknown types are ignored
}

// ---- telemetry -------------------------------------------------------------------------------

void Host::flush_telemetry() {
  if (!server_ || !server_->connected()) {
    return;
  }
  const int64_t t = now_ns();
  constexpr int64_t kHidNs = 33'000'000, kSensorNs = 100'000'000, kSourceNs = 250'000'000;

  if (t - last_hid_ns_ >= kHidNs) {
    json::Value m;
    {
      std::lock_guard<std::mutex> lock(out_mu_);
      if (last_was_xac_) {
        if (std::fabs(xac_x_ - sent_xac_x_) > 1e-4 || std::fabs(xac_y_ - sent_xac_y_) > 1e-4) {
          m = json::Value::object();
          m.set("type", "hid").set("mode", "xac").set("x", xac_x_).set("y", xac_y_);
          sent_xac_x_ = xac_x_;
          sent_xac_y_ = xac_y_;
        }
      } else if (agg_dirty_ && (agg_dx_ != 0 || agg_dy_ != 0 || agg_wheel_ != 0 ||
                                agg_buttons_ != sent_buttons_)) {
        m = json::Value::object();
        m.set("type", "hid").set("mode", "mouse").set("dx", static_cast<double>(agg_dx_));
        m.set("dy", static_cast<double>(agg_dy_)).set("wheel", static_cast<double>(agg_wheel_));
        m.set("buttons", static_cast<int>(agg_buttons_));
        sent_buttons_ = agg_buttons_;
      }
      agg_dx_ = agg_dy_ = agg_wheel_ = 0;
      agg_dirty_ = false;
    }
    last_hid_ns_ = t;
    if (m.is_object()) {
      send_json(m);
    }
  }

  if (t - last_sensor_ns_ >= kSensorNs) {
    last_sensor_ns_ = t;
    const StickPos p = stick.position(t / 1'000'000);
    json::Value m = json::Value::object();
    m.set("type", "sensor").set("x", p.x).set("y", p.y);
    m.set("bx", live.bx.load()).set("by", live.by.load());
    m.set("ok", !live.sensor_fault.load()).set("fault", fault_name(sensor.fault()));
    send_json(m);
  }

  const StickSource::Info info = stick.info(t / 1'000'000);
  const char *kind = info.kind == StickSource::Kind::Manual    ? "manual"
                     : info.kind == StickSource::Kind::Pattern ? "pattern"
                                                               : "script";
  const std::string key = std::string(kind) + "/" + info.name + (info.done ? "/done" : "");
  if (key != last_source_key_ ||
      (info.kind != StickSource::Kind::Manual && !info.done && t - last_source_ns_ >= kSourceNs)) {
    last_source_key_ = key;
    last_source_ns_ = t;
    json::Value m = json::Value::object();
    m.set("type", "source").set("kind", kind);
    if (info.kind != StickSource::Kind::Manual) {
      m.set("name", info.name).set("t_ms", static_cast<double>(info.t_ms)).set("done", info.done);
    }
    send_json(m);
  }
}

// ---- scripts ------------------------------------------------------------------------------------

void Host::start_script(const Script &script) {
  std::lock_guard<std::mutex> lock(script_mu_);
  script_ = script;
  script_active_ = true;
  script_started_ = true;
  script_start_ms_ = now_ms();
  script_next_ = 0;
  stick.play_script(script_, script_start_ms_);
  std::lock_guard<std::mutex> out(out_mu_);
  win_dx_ = win_dy_ = 0;
  win_left_ = win_right_ = false;
  win_neutral_ = true;
}

bool Host::script_finished() const {
  std::lock_guard<std::mutex> lock(script_mu_);
  return !script_active_ || script_next_ >= script_.steps.size();
}

void Host::run_script_steps() {
  if (pumping_) {
    return;
  }
  pumping_ = true;
  while (true) {
    Step step;
    size_t index = 0;
    {
      std::lock_guard<std::mutex> lock(script_mu_);
      if (!script_active_ || script_next_ >= script_.steps.size() ||
          script_.steps[script_next_].at_ms > now_ms() - script_start_ms_) {
        break;
      }
      index = script_next_++;
      step = script_.steps[index];
    }
    if (step.kind == Step::Kind::Expect) {
      evaluate(step, index);
    } else {
      execute(step);
    }
  }
  pumping_ = false;
}

void Host::record(bool pass, int64_t at_ms, const std::string &what) {
  ++checks_;
  if (!pass) {
    ++failures_;
  }
  char head[64];
  std::snprintf(head, sizeof(head), "%s t=%lldms ", pass ? "PASS" : "FAIL",
                static_cast<long long>(at_ms));
  const std::string line = std::string(head) + what;
  report_lines_.push_back(line);
  if (!options_.headless || !pass) {
    std::fprintf(pass ? stdout : stderr, "script: %s\n", line.c_str());
  }
}

void Host::note_error(const std::string &message) {
  ++failures_;
  report_lines_.push_back("FAIL " + message);
  std::fprintf(stderr, "omega_sim: %s\n", message.c_str());
}

void Host::execute(const Step &step) {
  switch (step.kind) {
  case Step::Kind::Fault:
    sensor.set_fault(*parse_fault(step.name));
    break;
  case Step::Kind::Drift:
    sensor.set_drift(step.number, now_ns());
    break;
  case Step::Kind::Noise:
    sensor.set_noise(step.number);
    break;
  case Step::Kind::NextImage: {
    NextImage b = NextImage::OK;
    parse_next_image(step.name, b);
    flash().set_next_image(b);
    break;
  }
  case Step::Kind::Action: {
    const std::string got = action_reply(step);
    const std::string want = step.expect_reply.value_or("ok");
    record(got == want, step.at_ms,
           "action " + step.name + " replied " + got +
               (got == want ? "" : " (expected " + want + ")"));
    // The deferred reboot of RESTART, REVERT and OTA_FINISH fires from a real-time
    // thread; let it land before simulated time moves on.
    if (options_.headless && got == "ok" &&
        (step.name == "restart" || step.name == "flash" || step.name == "revert")) {
      wait_for_restart_real_time(std::chrono::milliseconds(3000));
    }
    break;
  }
  case Step::Kind::Stick:
  case Step::Kind::Pattern:
  case Step::Kind::Expect:
    break; // positions come from the stick source; expects are evaluated separately
  }
}

std::string Host::request(uint8_t type, const std::vector<uint8_t> &payload,
                          std::vector<uint8_t> *reply_payload) {
  const auto frame = stream::build_frame(false, espp::CalibrationService::kModule, type, payload);
  {
    std::lock_guard<std::mutex> lock(capture_mu_);
    capture_.clear();
    capture_thread_ = std::this_thread::get_id();
    capturing_ = true;
  }
  bool linked = false;
  {
    std::lock_guard<std::mutex> lock(link_mu_);
    if (link_) {
      link_(frame);
      linked = true;
    }
  }
  std::vector<uint8_t> bytes;
  {
    std::lock_guard<std::mutex> lock(capture_mu_);
    capturing_ = false;
    bytes.swap(capture_);
  }
  if (!linked) {
    return "none";
  }
  stream::StreamParser parser;
  for (const auto &f : parser.feed(bytes)) {
    if (!f.is_reply()) {
      continue;
    }
    if (f.type == static_cast<uint8_t>(Msg::Error)) {
      const uint32_t code = f.payload.size() >= 4 ? stream::get_u32(f.payload) : 0;
      if (code == static_cast<uint32_t>(std::errc::device_or_resource_busy)) {
        return "busy";
      }
      if (code == static_cast<uint32_t>(std::errc::invalid_argument)) {
        return "invalid";
      }
      if (code == static_cast<uint32_t>(std::errc::function_not_supported)) {
        return "unsupported";
      }
      return "error";
    }
    if (reply_payload) {
      *reply_payload = f.payload;
    }
    return "ok";
  }
  return "none";
}

std::string Host::action_reply(const Step &step) {
  auto type = [](Msg m) { return static_cast<uint8_t>(m); };
  if (step.name == "recalibrate") {
    return request(type(Msg::StartCalibration), {});
  }
  if (step.name == "save") {
    return request(type(Msg::SaveToFlash), {});
  }
  if (step.name == "restart") {
    return request(type(Msg::Restart), {});
  }
  if (step.name == "revert") {
    return request(type(Msg::Revert), {});
  }
  if (step.name == "mark_valid") {
    return request(type(Msg::MarkValid), {});
  }
  if (step.name == "fw_info") {
    return request(type(Msg::GetFwInfo), {});
  }
  if (step.name == "plant_unusable_calibration") {
    // sim only: writes the file straight to the simulated flash, as if an older
    // firmware had saved it. It takes effect at the next boot (see "restart").
    std::error_code ec;
    return plant_unusable_calibration(ec) ? "ok" : "error";
  }
  if (step.name == "set_mode") {
    return request(type(Msg::SetInputMode), {static_cast<uint8_t>(step.text == "xac" ? 1 : 0)});
  }
  if (step.name == "set_nudge_enabled") {
    // step.text is "on" or "off" (script value true or false).
    return request(type(Msg::SetNudgeEnabled), {static_cast<uint8_t>(step.text == "on" ? 1 : 0)});
  }
  if (step.name == "set_deadzone") {
    uint8_t flags = 0;
    if (step.center) {
      flags |= 0x01;
    }
    if (step.range) {
      flags |= 0x02;
    }
    std::vector<uint8_t> p{flags};
    const auto c = f32_bytes(static_cast<float>(step.center.value_or(0.0)));
    const auto r = f32_bytes(static_cast<float>(step.range.value_or(0.0)));
    p.insert(p.end(), c.begin(), c.end());
    p.insert(p.end(), r.begin(), r.end());
    return request(type(Msg::SetDeadzoneOverride), p);
  }
  if (step.name == "flash") {
    const std::vector<uint8_t> image = make_image(step.text);
    std::vector<uint8_t> begin;
    stream::put_u32(begin, static_cast<uint32_t>(image.size()));
    std::string reply = request(type(Msg::OtaBegin), begin);
    if (reply != "ok") {
      return reply;
    }
    constexpr size_t kChunk = 1024;
    for (size_t off = 0; off < image.size(); off += kChunk) {
      std::vector<uint8_t> data;
      stream::put_u32(data, static_cast<uint32_t>(off));
      const size_t n = std::min(kChunk, image.size() - off);
      data.insert(data.end(), image.begin() + static_cast<std::ptrdiff_t>(off),
                  image.begin() + static_cast<std::ptrdiff_t>(off + n));
      reply = request(type(Msg::OtaData), data);
      if (reply != "ok") {
        request(type(Msg::OtaAbort), {});
        return reply;
      }
    }
    return request(type(Msg::OtaFinish), {});
  }
  return "error";
}

void Host::evaluate(const Step &step, size_t index) {
  const Expect &e = step.expect;
  long dx, dy;
  bool left, right, neutral;
  double ux, uy;
  {
    std::lock_guard<std::mutex> lock(out_mu_);
    dx = win_dx_;
    dy = win_dy_;
    left = win_left_;
    right = win_right_;
    neutral = win_neutral_;
    ux = xac_x_;
    uy = xac_y_;
    win_dx_ = win_dy_ = 0;
    win_left_ = win_right_ = false;
    win_neutral_ = true;
  }
  const std::string where = "expect #" + std::to_string(index) + ": ";
  auto check = [&](bool pass, const std::string &what) { record(pass, step.at_ms, where + what); };
  auto yes_no = [](bool b) { return b ? "true" : "false"; };

  if (e.calibrated) {
    const bool got = live.have_calibration.load() && live.last_sweep.load() != 0;
    check(got == *e.calibrated,
          std::string("calibrated ") + yes_no(got) + " (expected " + yes_no(*e.calibrated) + ")");
  }
  if (e.center_max) {
    const auto [tx, ty] = sensor.true_center_mT(now_ns());
    const double ex = std::fabs(live.center_x.load() - tx) / SensorModel::mT_per_unit_x();
    const double ey = std::fabs(live.center_y.load() - ty) / SensorModel::mT_per_unit_y();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "center error x=%.4f y=%.4f (limit %.4f)", ex, ey,
                  *e.center_max);
    check(live.have_calibration.load() && ex <= *e.center_max && ey <= *e.center_max, buf);
  }
  if (e.mouse_moved) {
    constexpr long kMin = 3;
    std::string got = "none";
    if (std::labs(dx) >= kMin || std::labs(dy) >= kMin) {
      if (std::labs(dx) >= std::labs(dy)) {
        got = dx > 0 ? "right" : "left";
      } else {
        got = dy < 0 ? "up" : "down";
      }
    }
    check(got == *e.mouse_moved, "mouse moved " + got + " (dx=" + std::to_string(dx) + " dy=" +
                                     std::to_string(dy) + "; expected " + *e.mouse_moved + ")");
  }
  if (e.clicked) {
    const std::string got = left ? "left" : right ? "right" : "none";
    check(got == *e.clicked, "clicked " + got + " (expected " + *e.clicked + ")");
  }
  if (e.mode) {
    const std::string got = live.mode.load() == 1 ? "xac" : "mouse";
    check(got == *e.mode, "mode " + got + " (expected " + *e.mode + ")");
  }
  if (e.sensor_observed) {
    const bool got = live.sensor_observed.load();
    check(got == *e.sensor_observed, std::string("sensor_observed ") + yes_no(got) + " (expected " +
                                         yes_no(*e.sensor_observed) + ")");
  }
  if (e.sensor_fault) {
    const bool got = live.sensor_fault.load();
    check(got == *e.sensor_fault, std::string("sensor_fault ") + yes_no(got) + " (expected " +
                                      yes_no(*e.sensor_fault) + ")");
  }
  if (e.neutral) {
    check(neutral == *e.neutral,
          std::string("neutral ") + yes_no(neutral) + " (expected " + yes_no(*e.neutral) + ")");
  }
  if (e.needs_calibration) {
    // Asks the firmware over the link, as the console does.
    std::vector<uint8_t> payload;
    const std::string reply = request(static_cast<uint8_t>(Msg::GetDiagnostics), {}, &payload);
    const int state = reply == "ok" ? diagnostics_calibration_state(payload) : -1;
    if (state < 0) {
      check(false,
            "needs_calibration: GET_DIAGNOSTICS replied " + reply + " without a calibration state");
    } else {
      const bool got = state != 0;
      check(got == *e.needs_calibration, std::string("needs_calibration ") + yes_no(got) +
                                             " (expected " + yes_no(*e.needs_calibration) + ")");
    }
  }
  if (e.fw_slot) {
    const std::string got = flash().running_label();
    check(got == *e.fw_slot, "fw_slot " + got + " (expected " + *e.fw_slot + ")");
  }
  if (e.fw_pending) {
    const bool got = flash().pending_verify();
    check(got == *e.fw_pending,
          std::string("fw_pending ") + yes_no(got) + " (expected " + yes_no(*e.fw_pending) + ")");
  }
  if (e.fw_version) {
    const std::string got = esp_app_get_description()->version;
    check(got == *e.fw_version, "fw_version " + got + " (expected " + *e.fw_version + ")");
  }
  if (e.xac_x) {
    check(std::fabs(ux - *e.xac_x) <= 0.1,
          "xac_x " + json::Value(ux).dump() + " (expected " + json::Value(*e.xac_x).dump() + ")");
  }
  if (e.xac_y) {
    check(std::fabs(uy - *e.xac_y) <= 0.1,
          "xac_y " + json::Value(uy).dump() + " (expected " + json::Value(*e.xac_y).dump() + ")");
  }
}

int Host::finish_report(const std::string &script_name) {
  std::printf("\nomega_sim script \"%s\"\n", script_name.c_str());
  for (const std::string &line : report_lines_) {
    std::printf("  %s\n", line.c_str());
  }
  std::printf("%d checks, %d failed (simulated time %.1f s)\n", checks_, failures_.load(),
              static_cast<double>(now_ns()) / 1e9);
  return failures_.load() == 0 && checks_ > 0 ? 0 : 1;
}

} // namespace sim
