// omega_sim: the Omega Stick firmware logic around a simulated sensor.
// See docs/simulator.md.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "firmware.hpp"
#include "script.hpp"
#include "sim_clock.hpp"
#include "sim_flash.hpp"
#include "sim_host.hpp"
#include "ws_server.hpp"

namespace {

namespace fs = std::filesystem;

void usage() {
  std::puts("usage: omega_sim [--port 8765] [--data DIR] [--fresh] [--mode mouse|xac]\n"
            "                 [--script FILE] [--headless] [--speed N]\n"
            "\n"
            "  --port N       WebSocket port on 127.0.0.1 (default 8765)\n"
            "  --data DIR     simulated flash (calibration, settings, reset log, OTA slots);\n"
            "                 default ./omega_sim_data\n"
            "  --fresh        erase --data first, like a brand-new board\n"
            "  --mode M       input mode when no setting is saved yet\n"
            "  --script FILE  play a script from boot\n"
            "  --headless     no WebSocket: run --script on simulated time, print a report,\n"
            "                 exit 0 when every check passed\n"
            "  --speed N      interactive only: simulated time runs N times faster (default 1)");
}

bool parse_args(int argc, char **argv, sim::Options &o, std::string &error) {
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        error = std::string(name) + " needs a value";
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h") {
      usage();
      std::exit(0);
    } else if (a == "--port") {
      const char *v = value("--port");
      if (!v) {
        return false;
      }
      char *end = nullptr;
      const long n = std::strtol(v, &end, 10);
      if (*end != '\0' || n < 0 || n > 65535) {
        error = "--port must be 0..65535";
        return false;
      }
      o.port = static_cast<uint16_t>(n);
    } else if (a == "--data") {
      const char *v = value("--data");
      if (!v) {
        return false;
      }
      o.data = v;
    } else if (a == "--fresh") {
      o.fresh = true;
    } else if (a == "--mode") {
      const char *v = value("--mode");
      if (!v) {
        return false;
      }
      if (std::strcmp(v, "mouse") != 0 && std::strcmp(v, "xac") != 0) {
        error = "--mode must be mouse or xac";
        return false;
      }
      o.mode = v;
    } else if (a == "--script") {
      const char *v = value("--script");
      if (!v) {
        return false;
      }
      o.script_path = v;
    } else if (a == "--headless") {
      o.headless = true;
    } else if (a == "--speed") {
      const char *v = value("--speed");
      if (!v) {
        return false;
      }
      char *end = nullptr;
      const double n = std::strtod(v, &end);
      if (*end != '\0' || !(n > 0.0) || n > 1000.0) {
        error = "--speed must be a number from above 0 to 1000";
        return false;
      }
      o.speed = n;
    } else {
      error = "unknown option " + a;
      return false;
    }
  }
  if (o.headless && o.script_path.empty()) {
    error = "--headless needs --script";
    return false;
  }
  return true;
}

void on_sigint(int) { sim::g_interrupted = true; }

int reset_reason_for(const std::string &reason) {
  if (reason == "crash") {
    return sim::kRstPanic;
  }
  if (reason == "reset" || reason == "wipe") {
    return sim::kRstPoweron;
  }
  return sim::kRstSw;
}

void erase_directory_contents(const fs::path &dir) {
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    fs::remove_all(entry.path(), ec);
  }
}

} // namespace

int main(int argc, char **argv) {
  sim::Options options;
  std::string error;
  if (!parse_args(argc, argv, options, error)) {
    std::fprintf(stderr, "omega_sim: %s\n\n", error.c_str());
    usage();
    return 2;
  }

  // The espp file system is rooted at the working directory off the device, so the
  // data directory becomes the working directory. Resolve paths before moving.
  std::error_code ec;
  options.data = fs::absolute(options.data, ec);
  if (!options.script_path.empty()) {
    options.script_path = fs::absolute(options.script_path, ec).string();
  }
  sim::Script script;
  if (!options.script_path.empty()) {
    std::ifstream in(options.script_path, std::ios::binary);
    if (!in) {
      std::fprintf(stderr, "omega_sim: cannot read %s\n", options.script_path.c_str());
      return 2;
    }
    std::stringstream text;
    text << in.rdbuf();
    if (!sim::parse_script_text(text.str(), script, error)) {
      std::fprintf(stderr, "omega_sim: %s: %s\n", options.script_path.c_str(), error.c_str());
      return 2;
    }
  }
  if (options.fresh) {
    fs::remove_all(options.data, ec);
  }
  fs::create_directories(options.data, ec);
  fs::current_path(options.data, ec);
  if (ec) {
    std::fprintf(stderr, "omega_sim: cannot use data directory %s: %s\n",
                 options.data.string().c_str(), ec.message().c_str());
    return 2;
  }

  std::unique_ptr<sim::Clock> clock;
  if (options.headless) {
    clock = std::make_unique<sim::VirtualClock>();
  } else {
    sim::enable_precise_timers();
    clock = std::make_unique<sim::RealClock>(options.speed);
  }
  sim::set_current_clock(clock.get());

  sim::flash().set_native_version(OMEGA_SIM_FW_VERSION);
  sim::flash().load_or_format("ota_state.cfg");

  sim::Host *host_ptr = nullptr;
  std::unique_ptr<sim::ws::Server> server;
  if (!options.headless) {
    sim::ws::Handlers handlers;
    handlers.on_connect = [&host_ptr] { host_ptr->on_connect(); };
    handlers.on_text = [&host_ptr](const std::string &text) { host_ptr->on_text(text); };
    handlers.on_binary = [&host_ptr](std::span<const uint8_t> data) { host_ptr->on_binary(data); };
    server = std::make_unique<sim::ws::Server>(std::move(handlers));
  }
  sim::Host host(options, *clock, server.get());
  host_ptr = &host;
  sim::flash().set_restart_hook([&host](const char *reason) { host.request_restart(reason); });

  if (server) {
    if (!server->start(options.port, error)) {
      std::fprintf(stderr, "omega_sim: %s\n", error.c_str());
      return 1;
    }
    std::signal(SIGINT, on_sigint);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN); // a page that vanishes mid-send must not kill the simulator
#endif
    std::printf("omega_sim %s listening on ws://127.0.0.1:%u (data: %s)\n", OMEGA_SIM_FW_VERSION,
                static_cast<unsigned>(server->port()), options.data.string().c_str());
    std::fflush(stdout);
  }
  if (!options.script_path.empty()) {
    host.start_script(script);
  }

  int reset_reason = sim::kRstPoweron;
  while (!host.should_exit()) {
    if (sim::flash().boot()) {
      std::puts("bootloader: the running image was never confirmed; rolled back to the other slot");
    }
    const std::string reason = sim::run_firmware(host, reset_reason);
    if (reason.empty()) {
      break;
    }
    std::printf("restarting: %s\n", reason.c_str());
    host.announce_rebooting(reason);
    if (reason == "wipe" || host.take_wipe()) {
      erase_directory_contents(options.data);
      sim::flash().load_or_format("ota_state.cfg");
    }
    reset_reason = reset_reason_for(reason);
    clock->sleep_for(std::chrono::milliseconds(300));
    host.pump();
  }

  if (server) {
    server->stop();
  }
  if (options.headless) {
    return host.finish_report(script.name);
  }
  return 0;
}
