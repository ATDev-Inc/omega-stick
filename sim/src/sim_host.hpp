#pragma once

// Everything around the simulated firmware that is not the firmware: the clock,
// the stick and sensor models, the WebSocket telemetry and control messages, the
// script runner with its expectations, and the restart hand-off. One Host lives
// for the whole process; the firmware (firmware.cpp) is rebuilt around it on
// every simulated restart.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include "omega_control.hpp"
#include "script.hpp"
#include "sensor.hpp"
#include "sim_clock.hpp"
#include "stick.hpp"
#include "ws_server.hpp"

namespace sim {

/// Set by the SIGINT handler; ends the process like Host::request_exit().
inline std::atomic<bool> g_interrupted{false};

struct Options {
  uint16_t port{8765};
  std::filesystem::path data{"omega_sim_data"};
  bool fresh{false};
  std::optional<std::string> mode; ///< "mouse" or "xac"
  std::string script_path;
  bool headless{false};
  double speed{1.0};
};

class Host {
public:
  Host(const Options &options, Clock &clock, ws::Server *server);

  const Options &options() const { return options_; }
  bool headless() const { return options_.headless; }

  // ---- time ---------------------------------------------------------------------
  int64_t now_ns() const { return clock_.now_ns(); }
  int64_t now_ms() const { return clock_.now_ns() / 1'000'000; }
  /// Lets `d` of simulated time pass, then runs the script and telemetry. Throws
  /// RestartRequest when a restart was asked for, like the real device vanishing
  /// mid-sleep.
  void sleep(std::chrono::milliseconds d);
  /// Runs due script steps and sends due telemetry.
  void pump();

  // ---- restart and exit -----------------------------------------------------------
  /// Thread-safe. The first reason wins until the main loop takes it.
  void request_restart(const std::string &reason);
  bool restart_requested() const { return restart_pending_.load(); }
  std::string take_restart_reason();
  /// Process should end (headless script finished, or the user quit).
  bool should_exit() const { return exit_.load() || g_interrupted.load(); }
  void request_exit() { exit_ = true; }
  /// Headless only: the 750 ms reboot delay CalibrationService uses is real time, so
  /// wait for it to elapse (the restart request arrives from its thread).
  void wait_for_restart_real_time(std::chrono::milliseconds limit);
  /// Tells the page (when connected) the stick is about to restart and drops it.
  void announce_rebooting(const std::string &reason);

  // ---- models ----------------------------------------------------------------------
  SensorModel sensor;
  StickSource stick;

  // ---- what the firmware publishes (read by expectations and hello) -----------------
  struct Live {
    std::atomic<int> mode{0}; ///< 0 mouse, 1 xac
    std::atomic<bool> sensor_fault{false};
    std::atomic<bool> have_calibration{false};
    std::atomic<int> last_sweep{-1};              ///< -1 none yet, 0 failed, 1 succeeded
    std::atomic<double> center_x{0}, center_y{0}; ///< measured center, mT
    std::atomic<double> bx{0}, by{0};
  } live;

  // ---- firmware-side services --------------------------------------------------------
  /// The calibration link: the firmware registers where received bytes go.
  void set_link(std::function<void(std::span<const uint8_t>)> feed);
  void clear_link();
  /// Frames from the firmware's dispatcher/service. Goes to the page, or into the
  /// capture while a script action is waiting for its reply.
  bool transmit(std::span<const uint8_t> frame);
  /// Console text (calibration prompts, banners), like the CDC console carries.
  void console_text(const std::string &text);
  /// Calibration prompt: console text that also drives the `calibrate` pattern.
  void calibration_prompt(const std::string &text);
  void on_mouse(const espp::omega_control::MouseReport &report);
  void on_xac(const espp::omega_control::XacReport &report);
  /// Stick position now, and the time it was taken at (what the sensor reads).
  std::pair<StickPos, int64_t> stick_now();

  // ---- scripts ------------------------------------------------------------------------
  void start_script(const Script &script);
  bool script_finished() const;
  /// Prints the headless report; returns the process exit code.
  int report(const std::string &script_name);
  int failures() const { return failures_.load(); }
  void note_error(const std::string &message);

  // ---- page messages -------------------------------------------------------------------
  void on_connect();
  void on_text(const std::string &text);
  void on_binary(std::span<const uint8_t> data);
  /// True after a `wipe` message: the main loop erases the data directory.
  bool take_wipe();

private:
  void send_json(const json::Value &v);
  void handle_control(const json::Value &msg);
  void reject(const std::string &why);
  void flush_telemetry();
  void run_script_steps();
  void execute(const Step &step);
  std::string action_reply(const Step &step);
  std::string request(uint8_t type, const std::vector<uint8_t> &payload);
  void evaluate(const Step &step, size_t index);
  void record(bool pass, int64_t at_ms, const std::string &what);
  json::Value hello_message();

  Options options_;
  Clock &clock_;
  ws::Server *server_;

  std::mutex restart_mu_;
  std::string restart_reason_;
  std::atomic<bool> restart_pending_{false};
  std::atomic<bool> exit_{false};
  std::atomic<bool> wipe_{false};

  std::mutex link_mu_;
  std::function<void(std::span<const uint8_t>)> link_;

  // Replies to a script action are captured instead of being sent to the page.
  std::mutex capture_mu_;
  std::thread::id capture_thread_;
  std::vector<uint8_t> capture_;
  bool capturing_{false};

  // Script runner.
  mutable std::mutex script_mu_;
  Script script_;
  bool script_active_{false};
  bool script_started_{false};
  int64_t script_start_ms_{0};
  size_t script_next_{0};
  bool pumping_{false};
  std::vector<std::string> report_lines_;
  std::atomic<int> failures_{0};
  int checks_{0};

  // Output since the last expect, and telemetry state.
  std::mutex out_mu_;
  long win_dx_{0}, win_dy_{0};
  bool win_left_{false}, win_right_{false}, win_neutral_{true};
  uint8_t prev_buttons_{0};
  double xac_x_{0}, xac_y_{0}; ///< stick convention (+y up)
  long agg_dx_{0}, agg_dy_{0}, agg_wheel_{0};
  uint8_t agg_buttons_{0};
  bool agg_dirty_{false};
  uint8_t sent_buttons_{0};
  bool last_was_xac_{false};
  double sent_xac_x_{1e9}, sent_xac_y_{1e9};
  int64_t last_hid_ns_{0}, last_sensor_ns_{0}, last_source_ns_{0};
  std::string last_source_key_;
};

} // namespace sim
