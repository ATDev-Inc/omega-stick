# omega_sim

A desktop simulator of the Omega Stick firmware. It runs the firmware's real
calibration service, store, sweep, control loop, health check and reset log
around a simulated sensor, so the calibration console, mode switching,
diagnostics and firmware updates can be tried with no hardware. What it
replaces and what it cannot prove are in [docs/simulator.md](../docs/simulator.md).

## Build

```
cmake -S sim -B build-sim -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sim
ctest --test-dir build-sim --output-on-failure
```

The first configure downloads the pinned espp components (the same ones
`tests/host` uses, and from `managed_components/` when an `idf.py` build has
fetched them). Add `-DOMEGA_HOST_WERROR=ON` for warnings as errors. The
simulator is also built, and its scenarios run, by the host unit-test build
(`cmake -S tests/host ...`, option `OMEGA_HOST_BUILD_SIM`, on by default).
It is not part of the ESP-IDF firmware build.

## Run

```
build-sim/omega_sim --fresh
```

Then open the calibration console, choose **Connect Simulator** and connect to
`ws://127.0.0.1:8765`. A fresh board runs its first-boot calibration: in the
Simulator panel play the `calibrate` pattern while it runs (or start with
`--script sim/scenarios/fresh_board_calibrates.json`).

| Option | Meaning |
|---|---|
| `--port N` | WebSocket port on 127.0.0.1 (default 8765) |
| `--data DIR` | Simulated flash: calibration, settings, reset log, OTA slots (default `./omega_sim_data`, git-ignored) |
| `--fresh` | Erase `--data` first, like a new board |
| `--mode mouse\|xac` | Mode for a board with no saved setting |
| `--script FILE` | Play a script from boot |
| `--headless` | No WebSocket: run `--script` on simulated time, print a report, exit 0 if every check passed |
| `--speed N` | Interactive: simulated time N times faster than real time |

Only pages from `file://`, `localhost`, `127.0.0.1` and the published console
may connect, and only on the loopback interface.

## Scenarios

[scenarios/](scenarios/) holds JSON scripts that run headless and are
registered with ctest (`sim_scenario_*`). Run one by hand:

```
build-sim/omega_sim --headless --fresh --data build-sim/data/x --script sim/scenarios/nudge_click.json
```

The script format, patterns and `expect` keys are in
[docs/simulator.md](../docs/simulator.md).

## Layout

- `src/firmware.cpp`: the simulated firmware, mirroring `run_omega_stick()` in `main/main.cpp`.
- `src/sim_host.cpp`: telemetry, control messages, script runner, restarts.
- `src/sensor.cpp`, `src/stick.cpp`: the field model on a fake TMAG5273 I2C bus; patterns.
- `src/ws_protocol.hpp`, `src/ws_server.cpp`: RFC 6455 server (loopback only).
- `stubs/`: a two-slot fake of `espp::Ota` and the ESP-IDF OTA headers.
- `tests/`: unit tests for the JSON parser, SHA-1, base64, frames, handshake, scripts, sensor and flash fakes.
