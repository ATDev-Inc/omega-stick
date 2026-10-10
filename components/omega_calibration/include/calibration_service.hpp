#pragma once

// espp omega-stick calibration service — message ids + a transport-agnostic
// request handler layered on the espp `stream_frame` codec (magic "OT" +
// flags u8 + module u8 + type u8 + len u32 + payload + CRC-32, all
// little-endian; see components/stream_frame/include/stream_frame.hpp for
// the authoritative framing spec — this is the SAME codec coredump_service.hpp
// and the OTA service use). The calibration protocol occupies dispatcher
// MODULE 5 by default (CalibrationService::Config::module can move it), so
// it shares one byte stream with the other espp protocols (coredump on
// module 4, OTA, or free-form console text): frames for other modules are
// simply ignored (route with espp::Dispatcher, or call handle_frame() after
// routing by module).
//
// This is the device-side counterpart to the reference client
// `web/calibration_console.html`; see that file's header comment for the
// same spec written from the host's point of view (it is the definitive,
// byte-for-byte source of truth — this comment restates it for convenience).
//
// Message types & payloads (host -> device), module 5 by default:
//   0x50 GET_STATE              — no payload. Reply: STATE.
//   0x51 SET_MANUAL_OFFSET      — payload: f32 offset_x, f32 offset_y.
//                                 Reply: OK / ERROR.
//   0x52 SET_DEADZONE_OVERRIDE  — payload: u8 flags (bit0 = center override
//                                 present, bit1 = range override present; a
//                                 clear bit clears that override back to
//                                 auto), f32 center_value, f32 range_value
//                                 (value ignored when its flag bit is clear,
//                                 but always present — fixed 9-byte payload).
//                                 Reply: OK / ERROR.
//   0x53 SET_NUDGE_SETTINGS     — payload: f32 nudge_enter_threshold (delta),
//                                 f32 nudge_exit_threshold (absolute),
//                                 u32 nudge_time_ms (measurement window).
//                                 Reply: OK / ERROR.
//   0x54 SET_INPUT_MODE         — payload: u8 mode (0 = mouse, 1 = XAC).
//                                 Reply: OK / ERROR.
//   0x55 SAVE_TO_FLASH          — no payload. Persists the CURRENT live
//                                 center/range/settings (see OmegaStore::
//                                 save_to_flash()). Reply: OK / ERROR.
//   0x56 START_CALIBRATION      — no payload. ASYNC: asks the device to
//                                 re-run the full interactive center+range
//                                 calibration sequence via
//                                 Config::start_calibration, which must
//                                 return almost immediately (it only kicks
//                                 the work off -- see that field's docs).
//                                 Reply: OK (accepted/started) / ERROR
//                                 (rejected, e.g. one is already running, or
//                                 Config::start_calibration is unset). The
//                                 actual pass/fail arrives later as an
//                                 unsolicited CalibrationResult frame (see
//                                 notify_calibration_result()).
//
// Message types & payloads (device -> host), same module, reply flag set:
//   0xD0 STATE — payload: fixed 79-byte little-endian struct, in this exact
//                order (see build_state_payload() below):
//                  f32 center_x, center_y, center_noise_radius,
//                  f32 center_deadzone                 (calibration-derived)
//                  f32 center_manual_offset_x, center_manual_offset_y
//                  f32 range_min_x, range_max_x, range_min_y, range_max_y
//                  f32 range_usable_radius, range_reliable_max_radius,
//                      range_normalized_max_radius
//                  f32 range_deadzone                  (calibration-derived)
//                  u8  has_manual_center_deadzone       (0/1)
//                  f32 manual_center_deadzone           (valid iff above = 1)
//                  u8  has_manual_range_deadzone        (0/1)
//                  f32 manual_range_deadzone            (valid iff above = 1)
//                  f32 nudge_enter_threshold            (delta)
//                  f32 nudge_exit_threshold             (absolute)
//                  u32 nudge_time_ms                    (measurement window)
//                  u8  input_mode                       (0 = mouse, 1 = XAC)
//   0xDF OK    — payload: u32 context-dependent value (0 on plain success).
//   0xFE ERROR — payload: u32 code + UTF-8 message, same convention as
//                coredump_service.hpp's ERROR (message is authoritative; the
//                code is informational only / a best-effort std::errc value —
//                see that file's ERROR documentation for the full caveat).
//   0xD1 CalibrationResult — UNSOLICITED (never a reply to any specific
//                request; see notify_calibration_result()). Sent once, when
//                an async recalibration kicked off by START_CALIBRATION
//                finishes. Payload: u8 success (0/1), then, only when
//                success = 0, u32 code + UTF-8 message (same convention as
//                ERROR).
//
// Flow control: same as coredump_service.hpp — the host serializes
// transactions, one request in flight, waits for its reply. The one
// exception is START_CALIBRATION/CalibrationResult, which is intentionally
// async (see both fields' docs) since the underlying calibration sequence
// can take far longer than a normal request-reply round trip and must not
// block whatever context feed()/handle_frame() runs on.

#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "dispatcher.hpp"
#include "stream_frame.hpp"

#include "base_component.hpp"
#include "omega_store.hpp"

namespace espp {

/**
 * @brief Transport-agnostic service exposing an espp::OmegaStore's live
 *        calibration + settings (deadzone overrides, nudge-click tuning,
 *        mouse/XAC mode) over any framed byte stream, matching the reference
 *        client `web/calibration_console.html`.
 *
 * Device-side counterpart of CoreDumpService, built the same way: construct
 * with a `send` function that transmits an encoded reply frame, then mount
 * it on a transport (`feed(data)` from the transport's receive callback --
 * see CoreDumpService's class-level docs for the USB vendor / CDC / socket
 * patterns, which apply here unchanged), or route by module yourself and
 * call `handle(frame)` / `handle_frame(type, payload)` directly.
 *
 * All calibration/settings access goes through the shared espp::OmegaStore
 * (see that class), which owns its own mutex -- this service does NOT touch
 * shared state directly, so it can be mounted on several transports (one
 * instance per transport, as recommended, each wrapping the SAME OmegaStore)
 * without any of them racing the control loop or each other.
 *
 * **Threading & the `send` callback contract**: identical to
 * CoreDumpService -- `feed()` / `handle_frame()` / `reset_parser()` are
 * serialized against each other by an internal mutex covering this
 * instance's OWN parser state and reply-building, but the `send` callback is
 * always invoked after that mutex is released, so `send` may freely call
 * back into the service. That internal mutex is unrelated to (and never
 * held across a call into) OmegaStore's own mutex, so there is no
 * lock-ordering hazard between the two.
 *
 * **START_CALIBRATION is deliberately async** (see Config::start_calibration
 * and notify_calibration_result()): unlike every other message here, the
 * work it triggers is NOT expected to run inside handle_frame_locked(), so
 * it never blocks this mutex (or whatever transport callback drives feed())
 * for the tens-of-seconds an interactive calibration sweep can take.
 *
 * \section calibration_service_ex1 CalibrationService Example
 * \snippet calibration_example.cpp calibration_example
 */
class CalibrationService : public BaseComponent {
public:
  using Stream = espp::stream_frame::StreamParser;
  using CenterResult = OmegaStore::CenterResult;
  using RangeResult = OmegaStore::RangeResult;
  using DeviceSettings = OmegaStore::DeviceSettings;
  using InputMode = OmegaStore::InputMode;

  /// Calibration protocol message types (the stream_frame `type` byte within
  /// the service's module, 5 by default; see the header comment for the
  /// payload spec). Reply/unsolicited types keep the high bit set, exactly
  /// like CoreDumpService::Msg, so build() can map it to the frame reply
  /// flag without a separate table.
  enum class Msg : uint8_t {
    // host -> device
    GetState = 0x50,            ///< request the full calibration + settings state
    SetManualOffset = 0x51,     ///< set the manual center trim (f32 x, f32 y)
    SetDeadzoneOverride = 0x52, ///< set/clear the manual deadzone overrides
    SetNudgeSettings = 0x53,    ///< set the nudge-click distance/hold tuning
    SetInputMode = 0x54,        ///< switch mouse/XAC mode
    SaveToFlash = 0x55,         ///< persist the current live state to flash
    StartCalibration = 0x56,    ///< ASYNC: kick off an interactive recalibration
    // device -> host
    State = 0xD0,             ///< fixed 79-byte calibration + settings struct
    CalibrationResult = 0xD1, ///< UNSOLICITED: outcome of an async StartCalibration
    Ok = 0xDF,                ///< u32 context-dependent success value
    Error = 0xFE,             ///< u32 informational code + authoritative UTF-8 message
  };

  /// Default dispatcher module id of the calibration protocol (the frame
  /// `module` byte): the id `calibration_console.html` expects. See
  /// Config::module to serve on a different id.
  static constexpr uint8_t kModule = 5;

  /// Wire size of the STATE payload (see the header comment for the exact
  /// field layout).
  static constexpr size_t kStatePayloadSize = 79;

  /**
   * @brief Function used to transmit one encoded reply frame to the host.
   * @param frame The complete encoded frame bytes (header + payload + CRC).
   */
  using send_fn = std::function<void(std::span<const uint8_t> frame)>;

  /// Configuration for the CalibrationService.
  struct Config {
    OmegaStore &store;     ///< The live calibration/settings store to serve (must outlive this).
    send_fn send{nullptr}; ///< Transmits an encoded reply frame (required).
    /// Dispatcher module id this instance answers on (and stamps on its
    /// replies). The default (kModule, 5) is what
    /// `calibration_console.html` looks for; change it only if your host
    /// tooling is told the new id.
    uint8_t module{kModule};
    /// Optional: handles Msg::StartCalibration by kicking off an
    /// interactive recalibration. **Must return almost immediately** --
    /// e.g. by just setting an atomic flag that the device's own control
    /// loop already polls once per iteration and, when set, runs the
    /// actual center/range calibration sequence itself (see main.cpp's
    /// `recalibration_requested` for the reference pattern). Do NOT run the
    /// calibration sequence itself from inside this callback: it executes
    /// under this service's internal mutex (see the class docs), so
    /// blocking here for the tens of seconds a calibration sweep can take
    /// would stall every other feed()/handle_frame() call on this
    /// transport, and if feed() is itself called from a USB/UART receive
    /// interrupt or task, likely the transport's own RX processing too.
    /// Return true if a (re)calibration was accepted/started, false + `ec`
    /// if it could not be (e.g. one is already in progress). Whatever
    /// actually runs the calibration must call notify_calibration_result()
    /// on every CalibrationService instance mounted on a transport the host
    /// might be listening on once it finishes -- this service has no other
    /// way to learn that. When left unset, START_CALIBRATION always replies
    /// with ERROR (function_not_supported).
    std::function<bool(std::error_code &ec)> start_calibration{nullptr};
    espp::Logger::Verbosity log_level{espp::Logger::Verbosity::WARN}; ///< Logger verbosity.
  };

  /**
   * @brief Construct the service.
   * @param config Configuration parameters (the OmegaStore to serve and the
   *        reply `send` function).
   */
  explicit CalibrationService(const Config &config)
      : BaseComponent("CalibrationService", config.log_level)
      , store_(config.store)
      , send_(config.send)
      , module_(config.module)
      , start_calibration_(config.start_calibration) {}

  /// @brief The dispatcher module id this service answers on (Config::module;
  ///        kModule by default, which the web console expects).
  uint8_t module_id() const { return module_; }

  /// @brief Discovery metadata for registering this service on a Dispatcher.
  Dispatcher::ModuleInfo module_info() const {
    return {.name = "Calibration",
            .app = "calibration_console.html",
            .description = "Live-view and live-edit calibration, deadzones, "
                           "nudge-click tuning, and mouse/XAC mode"};
  }

  /**
   * @brief Dispatcher entry point: handle one routed frame.
   *
   * Frames for other modules and reply-flagged frames are ignored, so this
   * can be registered directly: `dispatcher.register_module(service)`.
   */
  void handle(const espp::stream_frame::Frame &frame) {
    if (frame.module != module_id() || frame.is_reply())
      return;
    handle_frame(frame.type, frame.payload);
  }

  /**
   * @brief Feed received transport bytes to the service.
   *
   * Runs the internal incremental frame parser and processes every complete
   * frame (see handle_frame()); identical shape to
   * CoreDumpService::feed() -- see its docs for the resynchronization /
   * bounded-reply-memory rationale, which applies unchanged here.
   *
   * @param data Any number of received bytes.
   */
  void feed(std::span<const uint8_t> data) {
    std::vector<espp::stream_frame::Frame> frames;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      frames = parser_.feed(data);
    }
    for (const auto &frame : frames) {
      if (frame.module != module_id() || frame.is_reply())
        continue;
      std::vector<uint8_t> reply;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!handle_frame_locked(frame.type, frame.payload, reply))
          continue;
      }
      // send outside the lock so a re-entrant transport cannot deadlock
      if (!reply.empty())
        send(reply);
    }
  }

  /**
   * @brief Handle one already-parsed frame.
   * @param type The frame type byte.
   * @param payload The frame payload bytes.
   * @return true if the frame type belongs to the calibration protocol and
   *         was processed, false if it was ignored (another protocol's
   *         frame).
   */
  bool handle_frame(uint8_t type, std::span<const uint8_t> payload) {
    std::vector<uint8_t> reply;
    bool handled;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      handled = handle_frame_locked(type, payload, reply);
    }
    if (!reply.empty())
      send(reply);
    return handled;
  }

  /// @brief Discard any partially-buffered frame bytes (e.g. on transport
  ///        reconnect or after an RX overflow).
  void reset_parser() {
    std::lock_guard<std::mutex> lock(mutex_);
    parser_.reset();
  }

  /**
   * @brief Send an unsolicited CalibrationResult frame reporting how an
   *        asynchronous recalibration (kicked off via Msg::StartCalibration
   *        / Config::start_calibration) turned out.
   *
   * This service has no way to know on its own when that recalibration
   * finishes -- Config::start_calibration only KICKS IT OFF and must return
   * almost immediately (see its docs) -- so whatever actually runs the
   * interactive calibration sequence (e.g. main.cpp's own control-loop,
   * polling for the flag once per iteration) must call this itself once it
   * is done, on every CalibrationService instance mounted on a transport
   * the host might be listening on (e.g. both the vendor and CDC instances
   * main.cpp constructs).
   *
   * Safe to call from any context that may also call send() concurrently;
   * it takes no lock of its own beyond what send() already does (none --
   * see the class docs on the `send` callback contract).
   *
   * @param success Whether the recalibration succeeded.
   * @param ec On failure, an error code describing why (its message() is
   *        sent to the host verbatim, exactly like build_error()); ignored
   *        when @p success is true.
   */
  void notify_calibration_result(bool success, const std::error_code &ec = {}) {
    namespace stream = espp::stream_frame;
    std::vector<uint8_t> payload;
    payload.push_back(success ? 1 : 0);
    if (!success) {
      stream::put_u32(payload, static_cast<uint32_t>(ec.value()));
      const std::string message = ec.message();
      size_t count = std::min(message.size(), stream::kMaxPayloadSize - payload.size());
      while (count > 0 && count < message.size() &&
             (static_cast<uint8_t>(message[count]) & 0xC0) == 0x80) {
        --count;
      }
      payload.insert(payload.end(), message.begin(), message.begin() + count);
    }
    logger_.info("notify_calibration_result success={}", success);
    send(build(Msg::CalibrationResult, payload, module_id()));
  }

protected:
  /// Handle one frame with the (parser-only) mutex held: touch the
  /// OmegaStore (which has its own independent locking) and BUILD the
  /// encoded reply frame into @p reply — but do NOT send it, exactly like
  /// CoreDumpService::handle_frame_locked(). The caller transmits @p reply
  /// after releasing this mutex.
  bool handle_frame_locked(uint8_t type, std::span<const uint8_t> payload,
                           std::vector<uint8_t> &reply) {
    namespace stream = espp::stream_frame;
    switch (static_cast<Msg>(type)) {
    case Msg::GetState: {
      const OmegaStore::Snapshot snap = store_.snapshot();
      reply = build(Msg::State, build_state_payload(snap), module_id());
      return true;
    }
    case Msg::SetManualOffset: {
      if (payload.size() != 8) {
        reply = build_error(std::errc::invalid_argument,
                            "SET_MANUAL_OFFSET needs f32 offset_x + f32 offset_y");
        return true;
      }
      const float offset_x = get_f32(payload, 0);
      const float offset_y = get_f32(payload, 4);
      store_.set_manual_offset(offset_x, offset_y);
      logger_.info("SET_MANUAL_OFFSET x={:.6f} y={:.6f}", offset_x, offset_y);
      reply = build_ok();
      return true;
    }
    case Msg::SetDeadzoneOverride: {
      if (payload.size() != 9) {
        reply = build_error(std::errc::invalid_argument,
                            "SET_DEADZONE_OVERRIDE needs u8 flags + f32 + f32");
        return true;
      }
      const uint8_t flags = payload[0];
      const bool has_center = (flags & 0x01) != 0;
      const bool has_range = (flags & 0x02) != 0;
      const float center_value = get_f32(payload, 1);
      const float range_value = get_f32(payload, 5);
      store_.set_deadzone_override(has_center ? std::optional<float>(center_value) : std::nullopt,
                                   has_range ? std::optional<float>(range_value) : std::nullopt);
      logger_.info("SET_DEADZONE_OVERRIDE center={} range={}",
                   has_center ? std::to_string(center_value) : std::string("(auto)"),
                   has_range ? std::to_string(range_value) : std::string("(auto)"));
      reply = build_ok();
      return true;
    }
    case Msg::SetNudgeSettings: {
      if (payload.size() != 12) {
        reply = build_error(std::errc::invalid_argument,
                            "SET_NUDGE_SETTINGS needs f32 enter + f32 exit + u32 time_ms");
        return true;
      }
      const float enter_threshold = get_f32(payload, 0);
      const float exit_threshold = get_f32(payload, 4);
      const uint32_t time_ms = stream::get_u32(payload.subspan(8));
      if (!(enter_threshold > 0.0f) || enter_threshold > 1.0f) {
        reply = build_error(std::errc::invalid_argument, "nudge enter threshold must be in (0, 1]");
        return true;
      }
      if (!(exit_threshold >= 0.0f) || exit_threshold > 1.0f) {
        reply = build_error(std::errc::invalid_argument, "nudge exit threshold must be in [0, 1]");
        return true;
      }
      store_.set_nudge_settings(enter_threshold, exit_threshold,
                                std::chrono::milliseconds(time_ms));
      logger_.info("SET_NUDGE_SETTINGS enter={:.3f} exit={:.3f} time_ms={}", enter_threshold,
                   exit_threshold, time_ms);
      reply = build_ok();
      return true;
    }
    case Msg::SetInputMode: {
      if (payload.size() != 1 || payload[0] > 1) {
        reply = build_error(std::errc::invalid_argument, "SET_INPUT_MODE needs u8 mode (0 or 1)");
        return true;
      }
      const auto mode = static_cast<InputMode>(payload[0]);
      store_.set_input_mode(mode);
      logger_.info("SET_INPUT_MODE {}", mode == InputMode::XAC ? "XAC" : "MOUSE");
      reply = build_ok();
      return true;
    }
    case Msg::SaveToFlash: {
      std::error_code ec;
      if (!store_.save_to_flash(ec)) {
        reply = build_error(ec, "SAVE_TO_FLASH failed");
        return true;
      }
      logger_.info("SAVE_TO_FLASH ok");
      reply = build_ok();
      return true;
    }
    case Msg::StartCalibration: {
      if (payload.size() != 0) {
        reply = build_error(std::errc::invalid_argument, "START_CALIBRATION takes no payload");
        return true;
      }
      if (!start_calibration_) {
        reply = build_error(std::errc::function_not_supported,
                            "START_CALIBRATION not wired on this build");
        return true;
      }
      // start_calibration_ is required to return almost immediately (see
      // Config::start_calibration's docs) -- it only kicks the work off, it
      // does not run it, so it is safe to call here under mutex_.
      std::error_code cal_ec;
      if (!start_calibration_(cal_ec)) {
        reply = build_error(cal_ec, "START_CALIBRATION could not be started");
        return true;
      }
      logger_.info("START_CALIBRATION accepted; running asynchronously");
      reply = build_ok();
      return true;
    }
    default:
      // Not a calibration protocol frame: IGNORE it (no error reply), so
      // this service can share a stream with other espp protocols.
      return false;
    }
  }

  /// Encode a Snapshot into the fixed 75-byte STATE payload (see the header
  /// comment for the exact field order).
  static std::vector<uint8_t> build_state_payload(const OmegaStore::Snapshot &snap) {
    std::vector<uint8_t> payload;
    payload.reserve(kStatePayloadSize);
    const auto &center = snap.center;
    const auto &range = snap.range;
    const auto &settings = snap.settings;

    put_f32(payload, center.center_x);
    put_f32(payload, center.center_y);
    put_f32(payload, center.noise_radius);
    put_f32(payload, center.deadzone);
    put_f32(payload, center.manual_offset_x);
    put_f32(payload, center.manual_offset_y);

    put_f32(payload, range.min_x);
    put_f32(payload, range.max_x);
    put_f32(payload, range.min_y);
    put_f32(payload, range.max_y);
    put_f32(payload, range.usable_radius);
    put_f32(payload, range.reliable_max_radius);
    put_f32(payload, range.normalized_max_radius);
    put_f32(payload, range.deadzone);

    payload.push_back(settings.manual_center_deadzone.has_value() ? 1 : 0);
    put_f32(payload, settings.manual_center_deadzone.value_or(0.0f));
    payload.push_back(settings.manual_range_deadzone.has_value() ? 1 : 0);
    put_f32(payload, settings.manual_range_deadzone.value_or(0.0f));

    put_f32(payload, settings.nudge_distance_threshold);
    put_f32(payload, settings.nudge_exit_threshold);
    espp::stream_frame::put_u32(payload, static_cast<uint32_t>(settings.nudge_hold_time.count()));
    payload.push_back(static_cast<uint8_t>(settings.input_mode));

    return payload;
  }

  /// Appends a little-endian f32 to @p out. The ESP32 targets espp builds
  /// for are little-endian, matching the wire format and the reference
  /// client's `DataView.getFloat32(offset, true)` reads, so this is a
  /// straight byte copy (no bit-twiddling needed, unlike a portable
  /// byteswap-based encoder would require).
  static void put_f32(std::vector<uint8_t> &out, float value) {
    uint8_t bytes[4];
    std::memcpy(bytes, &value, sizeof(bytes));
    out.insert(out.end(), bytes, bytes + sizeof(bytes));
  }

  /// Reads a little-endian f32 starting at @p offset within @p data.
  /// Caller-validated: every call site checks payload.size() up front.
  static float get_f32(std::span<const uint8_t> data, size_t offset) {
    float value;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
  }

  /// Build an encoded frame for a calibration protocol message type.
  static std::vector<uint8_t> build(Msg type, std::span<const uint8_t> payload = {},
                                    uint8_t module = kModule) {
    namespace stream = espp::stream_frame;
    const bool reply = (static_cast<uint8_t>(type) & 0x80) != 0;
    return stream::build_frame(reply, module, static_cast<uint8_t>(type), payload);
  }

  /// Build a plain OK reply (u32 payload, 0 = success).
  std::vector<uint8_t> build_ok(uint32_t value = 0) const {
    namespace stream = espp::stream_frame;
    std::vector<uint8_t> payload;
    stream::put_u32(payload, value);
    return build(Msg::Ok, payload, module_id());
  }

  /// Transmit an encoded reply frame via the configured send function. Must
  /// be called WITHOUT the internal mutex held.
  void send(const std::vector<uint8_t> &frame) {
    if (frame.empty())
      return;
    if (!send_) {
      logger_.warn("no send function configured; dropping a {}-byte reply", frame.size());
      return;
    }
    send_(frame);
  }

  /// Build an ERROR reply frame (u32 code + UTF-8 context message).
  /// Identical convention to CoreDumpService::build_error(): codes are
  /// normalized to a std::errc value where possible (falling back to
  /// std::errc::io_error), and are informational only -- the message is
  /// authoritative (see coredump_service.hpp's ERROR docs for the full
  /// caveat about std::errc numbering not being portable across stdlibs).
  std::vector<uint8_t> build_error(const std::error_code &ec, std::string_view context) const {
    namespace stream = espp::stream_frame;
    logger_.error("{}: {}", context, ec.message());
    int code = ec.value();
    if (ec.category() != std::generic_category()) {
      const std::error_condition cond = ec.default_error_condition();
      code = (cond.category() == std::generic_category()) ? cond.value()
                                                          : static_cast<int>(std::errc::io_error);
    }
    std::vector<uint8_t> payload;
    stream::put_u32(payload, static_cast<uint32_t>(code));
    const std::string message = std::string(context) + ": " + ec.message();
    size_t count = std::min(message.size(), stream::kMaxPayloadSize - payload.size());
    while (count > 0 && count < message.size() &&
           (static_cast<uint8_t>(message[count]) & 0xC0) == 0x80) {
      --count;
    }
    payload.insert(payload.end(), message.begin(), message.begin() + count);
    return build(Msg::Error, payload, module_id());
  }

  /// Overload taking a std::errc directly.
  std::vector<uint8_t> build_error(std::errc errc, std::string_view context) const {
    return build_error(std::make_error_code(errc), context);
  }

private:
  OmegaStore &store_;
  send_fn send_;
  const uint8_t module_; // Config::module: routing id for requests + replies
  std::function<bool(std::error_code &ec)> start_calibration_; // Config::start_calibration
  std::mutex mutex_; // guards parser_ + reply-building only; OmegaStore has its own lock
  Stream parser_;
};

// Compile-time check that the service keeps satisfying the dispatcher's
// module contract (module_id() / module_info() / handle(frame)) -- see
// espp::DispatcherModuleConcept.
static_assert(DispatcherModuleConcept<CalibrationService>);

} // namespace espp