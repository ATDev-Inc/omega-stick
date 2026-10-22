# omega_sim

A desktop simulator of the Omega Stick firmware. It runs the firmware's real
calibration service, store, sweep, control loop, health check and reset log
around a simulated sensor, so the calibration console, mode switching,
diagnostics and firmware updates can be tried with no hardware. What it
replaces and what it cannot prove are in [docs/simulator.md](../docs/simulator.md),
which is also the reference for every option, message and script key.

This page is the step-by-step guide. If something goes wrong, see
[Troubleshooting](#troubleshooting).

## 1. What you need

- **CMake 3.20 or later.**
- **A C++20 compiler with `<format>`**: Visual Studio 2022 (MSVC) on Windows,
  or GCC 13+ or Clang 18+ on Linux (the Ubuntu 24.04 versions CI builds
  with). macOS is
  not tested.
- **A network connection for the first build only.** Configuring downloads the
  espp components pinned in `dependencies.lock` (hash-checked). If an
  `idf.py build` has already filled `managed_components/`, those are used
  instead.
- **A current browser** for the calibration console. Any one can connect to
  the simulator; a real stick needs Chrome or Edge.

You do not need ESP-IDF, a board, or Node.

## 2. Build

From the repository root:

```
cmake -S sim -B build-sim
cmake --build build-sim
```

Where the program ends up depends on the generator CMake picked:

| Setup | Program |
|---|---|
| Windows, Visual Studio (the default) | `build-sim\Debug\omega_sim.exe` |
| Windows, Visual Studio, `cmake --build build-sim --config Release` | `build-sim\Release\omega_sim.exe` |
| Windows with Ninja (`-G Ninja`) | `build-sim\omega_sim.exe` |
| Linux | `build-sim/omega_sim` |

The rest of this page writes `omega_sim` for whichever path applies to you.

Optional: `ctest --test-dir build-sim -C Debug --output-on-failure` runs the
simulator's unit tests and scenarios (`-C Debug` matters only for Visual
Studio). `-DOMEGA_HOST_WERROR=ON` turns warnings into errors. The host
unit-test build (`cmake -S tests/host ...`) builds the simulator too, at
`build-host/sim/` (`build-host\sim\Debug\` with Visual Studio). It is not part
of the ESP-IDF firmware build.

## 3. Start the simulator

```
omega_sim --fresh
```

`--fresh` starts as a brand-new board. When it is ready it prints a line like:

```
omega_sim 0.1.0+abc1234 listening on ws://127.0.0.1:8765 (data: C:\...\omega_sim_data)
```

Leave that window open: the simulator runs until you press **Ctrl+C** there.

Its saved calibration and settings live in `omega_sim_data` in the folder you
started it from (or `--data DIR`). Starting from a different folder therefore
starts a different board. Leave out `--fresh` to keep what you saved last
time.

## 4. Open the console and connect

1. Open `components/omega_calibration/web/calibration_console.html` from disk
   in your browser (double-click it, or drag it into a browser window).
2. Leave **Simulator address** as `ws://127.0.0.1:8765` (change the port only
   if you started the simulator with `--port`).
3. Choose **Connect Simulator**. The console behaves as with a real stick and
   adds a **Simulator** panel.

## 5. First run: let it calibrate

A fresh board starts with its first-boot calibration sweep, as a real stick
does, and the sweep needs the stick to move. Nothing happens until it does:
the console shows the calibration prompts, and **Re-run calibration** is refused
as busy.

In the Simulator panel, play the **`calibrate`** pattern while the prompts are
showing. The sweep finishes, the calibration is saved, and the stick is ready
to use. (Or start with
`omega_sim --fresh --script sim/scenarios/fresh_board_calibrates.json`, which
does this for you.)

After that, use the panel's virtual stick and patterns to move the stick, and
watch the mouse or gamepad output it reports.

## Troubleshooting

| What you see | Cause and fix |
|---|---|
| `omega_sim: command not found`, or "is not recognized" (Windows) | Wrong path for your setup. See the table in [Build](#2-build): with Visual Studio the program is in `build-sim\Debug\`. |
| Configure fails while fetching espp | The first configure needs the network. Retry when online, or run `idf.py build` once so `managed_components/` is filled. |
| Compile errors about `<format>` or `std::format` | The compiler is too old. Use Visual Studio 2022, GCC 13+ or Clang 18+. |
| `cannot listen on 127.0.0.1:8765 (is another simulator running?)` | Another `omega_sim` is still running (maybe in another window). Close it with Ctrl+C, or start this one with `--port 8766` and change the console's address to match. |
| Console log: `Simulator connect failed ... Is omega_sim running?` | The simulator is not running, has stopped, or is on another port. Check its window for the `listening on` line and that the address in the console matches. |
| Connect fails even though the simulator is running | The page was not opened from disk or from `localhost`. The simulator accepts only pages from `file://`, `http://localhost`, `http://127.0.0.1` and the published console, and refuses anything else (for example a page served from your machine's network address). Open the file from disk. |
| Connected, but the stick does nothing and the console keeps showing calibration prompts | The first-boot sweep is waiting for movement. Play the `calibrate` pattern ([step 5](#5-first-run-let-it-calibrate)). |
| Calibration or settings look wrong, or are left over from earlier | Restart with `--fresh` to wipe the simulated board, or use the Simulator panel's **Wipe**. |
| Connecting a second console tab disconnects the first | Expected: the simulator serves one console at a time, and the newest connection wins. |
| Firmware update controls are missing | You started it with `--no-ota`, which acts like a v0.1.0 build with updates turned off. Start it without that option. |

If none of these fit, open an issue with your OS, compiler, the exact command,
and what the simulator window and the console log printed.

## Options

```
omega_sim [--port 8765] [--data DIR] [--fresh] [--mode mouse|xac]
          [--script FILE] [--headless] [--speed N] [--no-ota]
```

`omega_sim --help` lists them. What each does is in
[docs/simulator.md](../docs/simulator.md#running-it).

## Scenarios

[scenarios/](scenarios/) holds JSON scripts that run headless and are
registered with ctest (`sim_scenario_*`). Run one by hand:

```
omega_sim --headless --fresh --data build-sim/data/x --script sim/scenarios/nudge_click.json
```

It prints a report and exits 0 if every check passed. The script format,
patterns and `expect` keys are in [docs/simulator.md](../docs/simulator.md).

## Layout

- `src/firmware.cpp`: the simulated firmware, mirroring `run_omega_stick()` in `main/main.cpp`.
- `src/sim_host.cpp`: telemetry, control messages, script runner, restarts.
- `src/sensor.cpp`, `src/stick.cpp`: the field model on a fake TMAG5273 I2C bus; patterns.
- `src/ws_protocol.hpp`, `src/ws_server.cpp`: RFC 6455 server (loopback only).
- `stubs/`: a two-slot fake of `espp::Ota` and the ESP-IDF OTA headers.
- `tests/`: unit tests for the JSON parser, SHA-1, base64, frames, handshake, scripts, sensor and flash fakes.
