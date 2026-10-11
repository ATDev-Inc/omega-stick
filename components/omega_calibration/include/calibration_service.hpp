#pragma once

// espp omega-stick calibration service: message ids + a transport-agnostic
// request handler layered on the espp `stream_frame` codec (magic "OT" +
// flags u8 + module u8 + type u8 + len u32 + payload + CRC-32, all
// little-endian; see components/stream_frame/include/stream_frame.hpp for
// the authoritative framing spec: this is the SAME codec coredump_service.hpp
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
// byte-for-byte source of truth: this comment restates it for convenience).
//
// Message types & payloads (host -> device), module 5 by default:
//   0x50 GET_STATE              - no payload. Reply: STATE.
//   0x51 SET_MANUAL_OFFSET      - payload: f32 offset_x, f32 offset_y.
//                                 Reply: OK / ERROR.
//   0x52 SET_DEADZONE_OVERRIDE  - payload: u8 flags (bit0 = center override
//                                 present, bit1 = range override present; a
//                                 clear bit clears that override back to
//                                 auto), f32 center_value, f32 range_value
//                                 (value ignored when its flag bit is clear,
//                                 but always present: fixed 9-byte payload).
//                                 A set value must be finite and within
//                                 0..OmegaCalibration::MAX_CENTER_DEADZONE
//                                 (0.5) / MAX_RANGE_DEADZONE (0.1), or the
//                                 reply is ERROR (invalid_argument).
//                                 Reply: OK / ERROR.
//   0x53 SET_NUDGE_SETTINGS     - payload: f32 nudge_enter_threshold (delta),
//                                 f32 nudge_exit_threshold (absolute),
//                                 u32 nudge_time_ms (measurement window).
//                                 Reply: OK / ERROR.
//   0x54 SET_INPUT_MODE         - payload: u8 mode (0 = mouse, 1 = XAC). The OK's
//                                 u32 is 1 when the device is switching (it
//                                 saves and restarts right after the reply)
//                                 and 0 when that mode was already active.
//                                 Reply: OK / ERROR. The mode is applied
//                                 only after the OK has been sent, since
//                                 the application may restart to apply it.
//   0x55 SAVE_TO_FLASH          - no payload. Persists the CURRENT live
//                                 center/range/settings (see OmegaStore::
//                                 save_to_flash()). Reply: OK / ERROR.
//   0x56 START_CALIBRATION      - no payload. ASYNC: asks the device to
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
//   0x57 RESTART                : no payload. Reply: OK (sent first), then
//                                 Config::restart runs ~750 ms later from a
//                                 detached thread. ERROR if Config::restart
//                                 is unset or a payload is present.
//   0x58 OTA_BEGIN              : payload: u32 image_size. Starts a firmware
//                                 upload session. Reply: OK / ERROR. If this
//                                 service already owns an active session (the
//                                 host restarted its upload, e.g. after a
//                                 disconnect), that stale session is aborted
//                                 and a new one begins. ERROR if another
//                                 service owns the active session. A session
//                                 with no OTA_BEGIN/OTA_DATA for the idle
//                                 timeout (main loop: 30 s) is aborted by
//                                 expire_stale_update().
//   0x59 OTA_DATA               : payload: u32 offset + raw image bytes
//                                 (offset must equal the bytes written so
//                                 far). Reply: OK with u32 = total bytes
//                                 written so far / ERROR (a failed write
//                                 aborts the session).
//   0x5A OTA_FINISH             : no payload. Validates + activates the new
//                                 image. Reply: OK, then the device reboots
//                                 ~750 ms later via espp::Ota::restart().
//   0x5B OTA_ABORT              : no payload. Aborts an upload begun by this
//                                 service. Reply: OK.
//   0x5C GET_FW_INFO            : no payload. Reply: FW_INFO.
//   0x5D MARK_VALID             : no payload. Confirms the running image
//                                 (cancels app rollback). Reply: OK / ERROR.
//   0x5E REVERT                 : no payload. Makes the other OTA slot (the
//                                 previously installed firmware, see FW_INFO)
//                                 the boot partition. Reply: OK, then the
//                                 device reboots ~750 ms later. An upload this
//                                 service began is aborted first. ERROR if
//                                 another service's upload is in progress, the
//                                 other slot holds no valid app image, or the
//                                 bootloader selection could not be written.
//                                 The reverted-to image boots PENDING_VERIFY
//                                 again and has to pass the same health check
//                                 as an update.
//   The OTA messages (and GET_FW_INFO/MARK_VALID/REVERT) reply ERROR
//   (function_not_supported) when Config::ota is null.
//
//   0x5F GET_DIAGNOSTICS        : no payload. Reply: DIAGNOSTICS, or ERROR
//                                 (function_not_supported) when
//                                 Config::diagnostics is unset.
//
// Message types & payloads (device -> host), same module, reply flag set:
//   0xD3 DIAGNOSTICS : payload (see build_diagnostics_payload() below):
//                  u8  sensor_fault        (0 none, 1 read failed, 2 stuck)
//                  u8  sensor_first_fault  (same codes; latched since boot)
//                  u32 sensor_fault_count  (trips since boot)
//                  u32 boot_count
//                  u8  reset_count         (N, oldest first)
//                  N × { u32 boot, u8 reason, u8 abnormal, u8 name_len,
//                        name_len bytes of UTF-8 name }
//   0xD0 STATE - payload: fixed 79-byte little-endian struct, in this exact
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
//   0xD2 FW_INFO : payload: u8 pending_verify (0/1), u8 session_active
//                (0/1), then length-prefixed strings (u8 len + bytes, at
//                most 255 bytes each, no terminator) in this order:
//                  project name       (esp_app_desc_t::project_name)
//                  version            (PROJECT_VER, see top-level CMakeLists)
//                  build date         ("Oct 10 2026")
//                  build time         ("14:03:11")
//                  ESP-IDF version    ("v6.0.2")
//                  running partition  ("ota_0" / "ota_1")
//                  image sha          (first 8 hex chars of the ELF SHA-256)
//                  boot partition     (what the bootloader will run next)
//                  previous slot      (the other OTA partition, "ota_1")
//                  previous version   (that slot's PROJECT_VER, "" if empty)
//                  previous date      ("" if empty)
//                  previous time      ("" if empty)
//                  previous state     ("valid", "new", "pending_verify",
//                                      "invalid", "aborted", "undefined",
//                                      or "empty" when the slot holds no
//                                      readable app image)
//                Hosts must stop parsing when the payload runs out: older
//                firmware sends only the first two strings, and later
//                firmware may append more.
//   0xDF OK    - payload: u32 context-dependent value (0 on plain success).
//   0xFE ERROR - payload: u32 code + UTF-8 message, same convention as
//                coredump_service.hpp's ERROR (message is authoritative; the
//                code is informational only / a best-effort std::errc value;
//                see that file's ERROR documentation for the full caveat).
//   0xD1 CalibrationResult - UNSOLICITED (never a reply to any specific
//                request; see notify_calibration_result()). Sent once, when
//                an async recalibration kicked off by START_CALIBRATION
//                finishes. Payload: u8 success (0/1), then, only when
//                success = 0, u32 code + UTF-8 message (same convention as
//                ERROR).
//
// Flow control: same as coredump_service.hpp: the host serializes
// transactions, one request in flight, waits for its reply. The one
// exception is START_CALIBRATION/CalibrationResult, which is intentionally
// async (see both fields' docs) since the underlying calibration sequence
// can take far longer than a normal request-reply round trip and must not
// block whatever context feed()/handle_frame() runs on.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "dispatcher.hpp"
#include "stream_frame.hpp"

#include "base_component.hpp"
#include "omega_store.hpp"
#include "ota.hpp"

#include "esp_app_desc.h"
#include "esp_ota_ops.h"

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
 * patterns, which apply here unchanged; main.cpp instead pushes the received
 * bytes into an espp::DispatcherWorker, so feed()/handle() run on its task
 * rather than the TinyUSB one), or route by module yourself and
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
    Restart = 0x57,             ///< reboot the device (reply first, then Config::restart)
    OtaBegin = 0x58,            ///< u32 image_size
    OtaData = 0x59,             ///< u32 offset + raw image bytes
    OtaFinish = 0x5A,           ///< validate + activate, then reboot
    OtaAbort = 0x5B,            ///< abort an upload begun by this service
    GetFwInfo = 0x5C,           ///< request FwInfo
    MarkValid = 0x5D,           ///< confirm running image (cancel rollback)
    Revert = 0x5E,              ///< boot the other OTA slot (previous firmware), then reboot
    GetDiagnostics = 0x5F,      ///< request the sensor fault status and reset log
    // device -> host
    State = 0xD0,             ///< fixed 79-byte calibration + settings struct
    CalibrationResult = 0xD1, ///< UNSOLICITED: outcome of an async StartCalibration
    FwInfo = 0xD2,            ///< u8 pending_verify, u8 session_active, str project, str version
    Diagnostics = 0xD3,       ///< variable-length sensor fault status + reset log
    Ok = 0xDF,                ///< u32 context-dependent success value
    Error = 0xFE,             ///< u32 informational code + authoritative UTF-8 message
  };

  /// Default dispatcher module id of the calibration protocol (the frame
  /// `module` byte): the id `calibration_console.html` expects. See
  /// Config::module to serve on a different id.
  static constexpr uint8_t kModule = 5;

  /// How long a queued reply is given to reach the wire before a reboot the
  /// same request asked for (RESTART, REVERT, OTA_FINISH).
  static constexpr std::chrono::milliseconds REPLY_FLUSH_DELAY{750};

  /// Wire size of the STATE payload (see the header comment for the exact
  /// field layout).
  static constexpr size_t kStatePayloadSize = 79;

  /**
   * @brief Function used to transmit one encoded reply frame to the host.
   * @param frame The complete encoded frame bytes (header + payload + CRC).
   */
  using send_fn = std::function<bool(std::span<const uint8_t> frame)>;

  /// What GET_DIAGNOSTICS reports: the sensor fault state the polling loop
  /// publishes (SensorFaultMonitor, D21) and the reset log (ResetLog, D23).
  /// Gathered by Config::diagnostics so this service needs neither object.
  struct DiagnosticsData {
    uint8_t sensor_fault{0};       ///< SensorFaultMonitor::Fault in effect now
    uint8_t sensor_first_fault{0}; ///< first fault since boot, latched
    uint32_t sensor_fault_count{0};
    uint32_t boot_count{0};
    struct Reset {
      uint32_t boot{0};
      uint8_t reason{0}; ///< platform reset-reason code
      bool abnormal{false};
      std::string name;
    };
    std::vector<Reset> resets; ///< oldest first
  };
  using diagnostics_fn = std::function<DiagnosticsData()>;

  /// Configuration for the CalibrationService.
  struct Config {
    OmegaStore &store; ///< The live calibration/settings store to serve (must outlive this).
    /// Transmits an encoded reply frame (required). Returns false when the
    /// frame could not be queued (a fail-fast USB write under backpressure):
    /// the service then drops any action the request deferred until after
    /// its reply, since the host never saw that reply.
    send_fn send{nullptr};
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
    /// Reboot the device (RESTART message). Invoked ~750 ms after the OK reply
    /// from a detached thread. When unset, RESTART replies with ERROR.
    std::function<void()> restart{nullptr};
    /// Optional OTA engine (not owned; must outlive the service and may be
    /// shared by several services). When null the OTA messages reply ERROR.
    espp::Ota *ota{nullptr};
    /// Optional: answers Msg::GetDiagnostics. Called under this service's
    /// internal mutex, so it must be quick and must not block on the
    /// control loop (copy out atomics or a snapshot; see main.cpp). When
    /// left unset, GET_DIAGNOSTICS replies with ERROR (function_not_supported).
    diagnostics_fn diagnostics{nullptr};
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
      , start_calibration_(config.start_calibration)
      , diagnostics_(config.diagnostics)
      , restart_(config.restart)
      , ota_(config.ota) {}

  /// @brief The dispatcher module id this service answers on (Config::module;
  ///        kModule by default, which the web console expects).
  uint8_t module_id() const { return module_; }

  /// @brief Discovery metadata for registering this service on a Dispatcher.
  Dispatcher::ModuleInfo module_info() const {
    return {.name = "Calibration",
            .app = "calibration_console.html",
            .description = "Live-view and live-edit calibration, deadzones, "
                           "nudge-click tuning, mouse/XAC mode, and firmware update",
            .protocol = "espp.calibration"};
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
      handle_frame(frame.type, frame.payload);
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
    // Send outside the lock so a re-entrant transport cannot deadlock. A
    // handler's deferred action runs only once its reply is queued.
    run_after_reply(send(reply));
    return handled;
  }

  /// @brief Discard any partially-buffered frame bytes (e.g. on transport
  ///        reconnect or after an RX overflow).
  void reset_parser() {
    std::lock_guard<std::mutex> lock(mutex_);
    parser_.reset();
  }

  /// True while a firmware upload (by any service sharing the engine) is in
  /// flight (use it to pause joystick report generation if you want).
  bool update_in_progress() const { return ota_ && ota_->session_active(); }

  /// Abort an upload this service began (call on transport loss / RX overflow).
  void abort_update() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ota_ && ota_owner_) {
      std::error_code ec;
      ota_->abort(ec);
      ota_owner_ = false;
    }
  }

  /**
   * @brief Abort an upload this service began if it has been idle too long.
   *
   * Bounded expiry for an upload abandoned by an unexpected transport loss
   * (the engine itself has no session timeout). Intended use: the main loop
   * calls this on every service about once a second with a 30 s @p max_idle.
   * Idle time is measured from the last successful OTA_BEGIN / OTA_DATA.
   * A service that does not own the active session never touches it.
   *
   * @param max_idle Idle time at or beyond which the session is aborted.
   * @return true if a session was aborted. false if nothing was owned, the
   *         session is not idle long enough, or the service is busy handling
   *         a frame (a busy service is not idle).
   */
  bool expire_stale_update(std::chrono::milliseconds max_idle) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !ota_ || !ota_owner_) {
      return false;
    }
    if (!ota_->session_active()) {
      ota_owner_ = false; // session already ended elsewhere
      return false;
    }
    if (std::chrono::steady_clock::now() - ota_last_activity_ < max_idle) {
      return false;
    }
    logger_.warn("OTA upload idle for too long; aborting");
    std::error_code ec;
    ota_->abort(ec);
    ota_owner_ = false;
    return true;
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
    send(build(Msg::CalibrationResult, payload, module_id()), /*unsolicited=*/true);
  }

protected:
  /// Handle one frame with the (parser-only) mutex held: touch the
  /// OmegaStore (which has its own independent locking) and BUILD the
  /// encoded reply frame into @p reply, but do NOT send it, exactly like
  /// CoreDumpService::handle_frame_locked(). The caller transmits @p reply
  /// after releasing this mutex.
  bool handle_frame_locked(uint8_t type, std::span<const uint8_t> payload,
                           std::vector<uint8_t> &reply) {
    namespace stream = espp::stream_frame;
    switch (static_cast<Msg>(type)) {
    case Msg::GetDiagnostics: {
      if (!diagnostics_) {
        reply = build_error(std::errc::function_not_supported,
                            "GET_DIAGNOSTICS: this device does not publish diagnostics");
        return true;
      }
      reply = build(Msg::Diagnostics, build_diagnostics_payload(diagnostics_()), module_id());
      return true;
    }
    case Msg::GetState: {
      const OmegaStore::Snapshot snap = store_.snapshot();
      reply = build(Msg::State, build_state_payload(snap), module_id());
      return true;
    }
    case Msg::OtaBegin: {
      if (!ota_) {
        reply = build_error(std::errc::function_not_supported, "OTA not wired on this build");
        return true;
      }
      if (payload.size() != 4) {
        reply = build_error(std::errc::invalid_argument, "OTA_BEGIN needs u32 image size");
        return true;
      }
      if (ota_owner_ && ota_->session_active()) {
        // The host on this link restarted its upload (it can only have one
        // transaction at a time), so the earlier session is stale: take over.
        logger_.info("OTA_BEGIN: aborting this link's stale upload");
        std::error_code abort_ec;
        ota_->abort(abort_ec);
        ota_owner_ = false;
      }
      std::error_code ec;
      if (!ota_->begin(stream::get_u32(payload), ec)) {
        reply = build_error(ec, "OTA_BEGIN failed");
        return true;
      }
      ota_owner_ = true;
      ota_last_activity_ = std::chrono::steady_clock::now();
      logger_.info("OTA_BEGIN {} bytes", stream::get_u32(payload));
      reply = build_ok();
      return true;
    }
    case Msg::OtaData: {
      if (!ota_ || !ota_owner_) {
        reply = build_error(std::errc::operation_not_permitted, "no OTA session started here");
        return true;
      }
      if (payload.size() <= 4) {
        reply = build_error(std::errc::invalid_argument, "OTA_DATA needs u32 offset + data");
        return true;
      }
      if (stream::get_u32(payload) != ota_->bytes_written()) {
        reply = build_error(std::errc::invalid_argument, "OTA_DATA offset mismatch");
        return true;
      }
      std::error_code ec;
      if (!ota_->write(payload.subspan(4), ec)) {
        ota_owner_ = false; // write() already aborted the session
        reply = build_error(ec, "OTA_DATA write failed");
        return true;
      }
      ota_last_activity_ = std::chrono::steady_clock::now();
      reply = build_ok(static_cast<uint32_t>(ota_->bytes_written()));
      return true;
    }
    case Msg::OtaFinish: {
      if (!ota_ || !ota_owner_) {
        reply = build_error(std::errc::operation_not_permitted, "no OTA session started here");
        return true;
      }
      std::error_code ec;
      const bool ok = ota_->finish(ec);
      ota_owner_ = false;
      if (!ok) {
        reply = build_error(ec, "OTA_FINISH (validate/activate) failed");
        return true;
      }
      logger_.info("OTA_FINISH ok; rebooting shortly");
      after_reply_ = [o = ota_] { reboot_after_flush([o] { o->restart(); }); };
      reply = build_ok();
      return true;
    }
    case Msg::OtaAbort: {
      if (ota_ && ota_owner_) {
        std::error_code ec;
        ota_->abort(ec);
        ota_owner_ = false;
      }
      reply = build_ok();
      return true;
    }
    case Msg::GetFwInfo: {
      if (!ota_) {
        reply = build_error(std::errc::function_not_supported, "OTA not wired on this build");
        return true;
      }
      // Read the app descriptor directly rather than through espp::Ota so the
      // report also carries the ELF hash, which identifies a build exactly
      // even when two builds share a version string (e.g. "-dirty" builds).
      const esp_app_desc_t *desc = esp_app_get_description();
      char sha[9];
      snprintf(sha, sizeof(sha), "%02x%02x%02x%02x", desc->app_elf_sha256[0],
               desc->app_elf_sha256[1], desc->app_elf_sha256[2], desc->app_elf_sha256[3]);
      const std::string running = ota_->running_partition_label();
      const std::string boot = ota_->boot_partition_label();
      // The other OTA slot still holds whatever ran before the last update
      // (or nothing, right after a serial flash). Report it so the console
      // can show what REVERT would boot.
      std::string prev_label, prev_version, prev_date, prev_time, prev_state = "empty";
      if (const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr)) {
        prev_label = other->label;
        esp_app_desc_t od;
        if (esp_ota_get_partition_description(other, &od) == ESP_OK) {
          prev_version = od.version;
          prev_date = od.date;
          prev_time = od.time;
          esp_ota_img_states_t st;
          prev_state =
              esp_ota_get_state_partition(other, &st) == ESP_OK ? ota_state_name(st) : "undefined";
        }
      }
      std::vector<uint8_t> p;
      p.push_back(ota_->is_pending_verify() ? 1 : 0);
      p.push_back(ota_->session_active() ? 1 : 0);
      for (const std::string_view s :
           {std::string_view{desc->project_name}, std::string_view{desc->version},
            std::string_view{desc->date}, std::string_view{desc->time},
            std::string_view{desc->idf_ver}, std::string_view{running}, std::string_view{sha},
            std::string_view{boot}, std::string_view{prev_label}, std::string_view{prev_version},
            std::string_view{prev_date}, std::string_view{prev_time},
            std::string_view{prev_state}}) {
        const size_t n = std::min<size_t>(s.size(), 255);
        p.push_back(static_cast<uint8_t>(n));
        p.insert(p.end(), s.begin(), s.begin() + n);
      }
      reply = build(Msg::FwInfo, p, module_id());
      return true;
    }
    case Msg::Revert: {
      if (!ota_) {
        reply = build_error(std::errc::function_not_supported, "OTA not wired on this build");
        return true;
      }
      if (!payload.empty()) {
        reply = build_error(std::errc::invalid_argument, "REVERT takes no payload");
        return true;
      }
      if (ota_owner_ && ota_->session_active()) {
        // An upload this link abandoned (e.g. the console disconnected midway).
        logger_.info("REVERT: aborting this link's abandoned upload");
        std::error_code abort_ec;
        ota_->abort(abort_ec);
        ota_owner_ = false;
      }
      if (ota_->session_active()) {
        reply = build_error(std::errc::operation_in_progress,
                            "REVERT refused: a firmware upload is in progress");
        return true;
      }
      const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
      esp_app_desc_t od;
      if (!other || esp_ota_get_partition_description(other, &od) != ESP_OK) {
        reply = build_error(std::errc::no_such_device,
                            "REVERT refused: the other slot holds no app image");
        return true;
      }
      // Also re-validates the image and resets its state to NEW, so a slot the
      // health check once marked INVALID can be tried again on purpose.
      if (const esp_err_t err = esp_ota_set_boot_partition(other); err != ESP_OK) {
        reply = build_error(std::errc::io_error,
                            std::string("REVERT: esp_ota_set_boot_partition failed: ") +
                                esp_err_to_name(err));
        return true;
      }
      logger_.info("REVERT to {} ({}); rebooting shortly", other->label, od.version);
      after_reply_ = [o = ota_] { reboot_after_flush([o] { o->restart(); }); };
      reply = build_ok();
      return true;
    }
    case Msg::MarkValid: {
      if (!ota_) {
        reply = build_error(std::errc::function_not_supported, "OTA not wired on this build");
        return true;
      }
      std::error_code ec;
      if (!ota_->mark_app_valid(ec)) {
        reply = build_error(ec, "MARK_VALID failed");
        return true;
      }
      reply = build_ok();
      return true;
    }
    case Msg::Restart: {
      if (!payload.empty()) {
        reply = build_error(std::errc::invalid_argument, "RESTART takes no payload");
        return true;
      }
      if (!restart_) {
        reply = build_error(std::errc::function_not_supported, "RESTART not wired on this build");
        return true;
      }
      after_reply_ = [r = restart_] { reboot_after_flush(r); };
      reply = build_ok();
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
      if (!std::isfinite(offset_x) || !std::isfinite(offset_y)) {
        reply = build_error(std::errc::invalid_argument, "manual offset must be finite");
        return true;
      }
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
      // The limits are the same ones calibration clamps its own results to
      // (OmegaCalibration::MAX_*_DEADZONE). Anything larger would make the
      // joystick library clamp the radius and suppress normal travel, and
      // the override persists across reboots, so refuse it up front.
      if (has_center && !(std::isfinite(center_value) && center_value >= 0.0f &&
                          center_value <= OmegaCalibration::MAX_CENTER_DEADZONE)) {
        reply = build_error(std::errc::invalid_argument,
                            fmt::format("center deadzone override must be between 0 and {}",
                                        OmegaCalibration::MAX_CENTER_DEADZONE));
        return true;
      }
      if (has_range && !(std::isfinite(range_value) && range_value >= 0.0f &&
                         range_value <= OmegaCalibration::MAX_RANGE_DEADZONE)) {
        reply = build_error(std::errc::invalid_argument,
                            fmt::format("range deadzone override must be between 0 and {}",
                                        OmegaCalibration::MAX_RANGE_DEADZONE));
        return true;
      }
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
      const char *mode_name = mode == InputMode::XAC ? "XAC" : "MOUSE";
      if (store_.settings().input_mode == mode) {
        // Nothing to save and no restart. The OK's value (0) tells the host
        // so, since the host's own idea of the mode may be stale.
        logger_.info("SET_INPUT_MODE {}: already active", mode_name);
        reply = build_ok(0);
        return true;
      }
      // Applied by run_after_reply() once the OK has been queued:
      // on_mode_changed may restart the device, which would drop the reply.
      after_reply_ = [this, mode] { store_.set_input_mode(mode); };
      logger_.info("SET_INPUT_MODE {}", mode_name);
      reply = build_ok(1); // 1: switching, the device restarts after this reply
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

  /// Encode a Snapshot into the fixed 79-byte STATE payload (see the header
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

  /// Encode DiagnosticsData into the DIAGNOSTICS payload (see the header
  /// comment for the field order). Names are clipped to 255 bytes and the
  /// reset list to what fits in one frame, newest entries kept.
  static std::vector<uint8_t> build_diagnostics_payload(const DiagnosticsData &d) {
    namespace stream = espp::stream_frame;
    std::vector<uint8_t> payload;
    payload.push_back(d.sensor_fault);
    payload.push_back(d.sensor_first_fault);
    stream::put_u32(payload, d.sensor_fault_count);
    stream::put_u32(payload, d.boot_count);

    // Work out how many of the newest resets fit, then emit them in order.
    const size_t header_size = payload.size() + 1; // + u8 reset_count
    size_t first = d.resets.size();
    size_t used = header_size;
    while (first > 0) {
      const auto &r = d.resets[first - 1];
      const size_t entry_size = 4 + 1 + 1 + 1 + std::min<size_t>(r.name.size(), 255);
      if (used + entry_size > stream::kMaxPayloadSize || d.resets.size() - (first - 1) > 255) {
        break;
      }
      used += entry_size;
      first--;
    }
    payload.push_back(static_cast<uint8_t>(d.resets.size() - first));
    for (size_t i = first; i < d.resets.size(); i++) {
      const auto &r = d.resets[i];
      const size_t name_len = std::min<size_t>(r.name.size(), 255);
      stream::put_u32(payload, r.boot);
      payload.push_back(r.reason);
      payload.push_back(r.abnormal ? 1 : 0);
      payload.push_back(static_cast<uint8_t>(name_len));
      payload.insert(payload.end(), r.name.begin(), r.name.begin() + name_len);
    }
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

  /// Hand an encoded frame to the configured send function. Must be called
  /// WITHOUT the internal mutex held. Returns true once the transport has
  /// QUEUED the frame (no send function, or a refusal, gives false). Queued
  /// is not received: REPLY_FLUSH_DELAY is what stands between the queue
  /// and the wire before a reboot. `unsolicited` frames (notifications on
  /// a link no host may have open) are logged at warn, not error.
  bool send(const std::vector<uint8_t> &frame, bool unsolicited = false) {
    if (frame.empty())
      return true;
    if (!send_) {
      logger_.warn("no send function configured; dropping a {}-byte frame", frame.size());
      return false;
    }
    if (!send_(frame)) {
      if (unsolicited) {
        logger_.warn("transport refused a {}-byte notification (no host on this link?)",
                     frame.size());
      } else {
        logger_.error("transport refused a {}-byte reply; the host will see a timeout",
                      frame.size());
      }
      return false;
    }
    return true;
  }

  /// Reboots from a short-lived thread after REPLY_FLUSH_DELAY, so the reply
  /// that was just queued reaches the wire first. Only ever scheduled through
  /// after_reply_, so it never runs for a reply the transport refused.
  static void reboot_after_flush(std::function<void()> reboot) {
    std::thread([reboot = std::move(reboot)] {
      std::this_thread::sleep_for(REPLY_FLUSH_DELAY);
      reboot();
    }).detach();
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
  /// Runs the action a handler deferred until after its reply: the mode
  /// switch (whose callback restarts the device) and the RESTART, REVERT
  /// and OTA_FINISH reboots. Called outside mutex_. When the reply was not
  /// queued the action is dropped with a log line: the host will not see
  /// the OK, so the device must not behave as if it had. The host times out
  /// and can retry.
  void run_after_reply(bool queued) {
    std::function<void()> action;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      action = std::exchange(after_reply_, nullptr);
    }
    if (!action)
      return;
    if (!queued) {
      logger_.error("reply was not queued; the deferred action is dropped");
      return;
    }
    action();
  }

  OmegaStore &store_;
  send_fn send_;
  const uint8_t module_; // Config::module: routing id for requests + replies
  std::function<bool(std::error_code &ec)> start_calibration_; // Config::start_calibration
  diagnostics_fn diagnostics_;                                 // Config::diagnostics
  std::function<void()> restart_;                              // Config::restart
  espp::Ota *ota_;                                             // Config::ota (not owned)
  bool ota_owner_{
      false}; // true while an upload begun by this service is active (guarded by mutex_)
  std::chrono::steady_clock::time_point ota_last_activity_{}; // last OTA_BEGIN/OTA_DATA success

  /// The otadata state of an app slot as the FW_INFO "previous state" string.
  static const char *ota_state_name(esp_ota_img_states_t state) {
    switch (state) {
    case ESP_OTA_IMG_NEW:
      return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
      return "pending_verify";
    case ESP_OTA_IMG_VALID:
      return "valid";
    case ESP_OTA_IMG_INVALID:
      return "invalid";
    case ESP_OTA_IMG_ABORTED:
      return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default:
      return "undefined";
    }
  }
  std::mutex mutex_; // guards parser_, after_reply_, reply-building; not OmegaStore
  Stream parser_;
  std::function<void()>
      after_reply_; // set by a handler, run by run_after_reply() once the reply is out
};

// Compile-time check that the service keeps satisfying the dispatcher's
// module contract (module_id() / module_info() / handle(frame)) -- see
// espp::DispatcherModuleConcept.
static_assert(DispatcherModuleConcept<CalibrationService>);

} // namespace espp