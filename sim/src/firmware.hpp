#pragma once

#include <string>

#include "sim_host.hpp"

namespace sim {

/// ESP-IDF esp_reset_reason_t values the simulator uses.
enum ResetReason : int { kRstPoweron = 1, kRstSw = 3, kRstPanic = 4 };

/// One boot of the simulated stick: mirrors run_omega_stick() in main/main.cpp
/// around the simulated sensor, link and HID output. Returns why it stopped:
/// "restart", "mode switch", "update", "revert", "rollback", "crash", "reset" or
/// "wipe" for a restart, or an empty string when the process should exit.
/// Everything the boot built is gone when this returns.
std::string run_firmware(Host &host, int reset_reason);

} // namespace sim
