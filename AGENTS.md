# AGENTS.md

Instructions for AI coding agents working in this repository. Human
contributors should read [CONTRIBUTING.md](CONTRIBUTING.md); this file
summarises it and adds the things an agent is likely to get wrong.

## What this is

Firmware for the Omega Stick, a low-force assistive joystick. It is an ESP-IDF
project for the ESP32-S3 (an 8 MB QT Py), written in C++20. The stick reads a
TI TMAG5273 Hall-effect sensor over I2C and presents itself over USB as either
a HID mouse or an Xbox Adaptive Controller (XAC) gamepad.

The people using this device may depend on it to operate their computer. Treat
input behaviour changes with the care that implies.

## Commands

Firmware (ESP-IDF v6.0, the version CI builds with; v5.5.1 is untested.
`dependencies.lock` was resolved against v6.0.2):

```console
idf.py set-target esp32s3
idf.py build
```

Never run `idf.py flash` or `idf.py monitor` unless the user explicitly asks:
a board may or may not be attached, and flashing changes the user's hardware.

Host unit tests (no ESP-IDF, no board; CMake 3.20+ and a C++20 compiler with
`<format>`):

```console
cmake -S tests/host -B build-host -DOMEGA_HOST_WERROR=ON
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

The host build downloads the real espp components pinned in
`dependencies.lock` (hash-checked) and compiles them; only the ESP-only OTA
headers are stand-ins under `tests/host/stubs/`. Sanitizer and coverage
options are in [tests/host/README.md](tests/host/README.md#cmake-options).

Web console tests (Node 24, no dependencies):

```console
node --test "tests/web/*.test.js"
```

They check [omega_protocol.js](components/omega_calibration/web/omega_protocol.js)
against the C++ side, including `tests/protocol/golden_frames.json`, which the
host tests generate, and cover the console's firmware-update logic and the
release feed written by [scripts/release_feed.mjs](scripts/release_feed.mjs)
(see [tests/web/README.md](tests/web/README.md)). After an intentional protocol change, regenerate it with
`OMEGA_WRITE_GOLDEN=1 build-host/omega_host_tests` and review the diff.

Desktop simulator (`omega_sim`, no board; same toolchain as the host tests).
The host test build above already builds it and runs its unit tests and
headless scenarios (`OMEGA_HOST_BUILD_SIM`, on by default). To run it on its
own and connect the console to it:

```console
cmake -S sim -B build-sim
cmake --build build-sim
build-sim/omega_sim --fresh
```

(the program is `build-sim/Debug/omega_sim.exe` with Visual Studio), then
choose **Connect Simulator** in the console. The step-by-step guide and
troubleshooting are in [sim/README.md](sim/README.md). Use it to try console and
calibration flows without hardware; it does not replace hardware testing (see
[docs/simulator.md](docs/simulator.md) for what it cannot prove).

Formatting is clang-format **14.0.6** via pre-commit
([.pre-commit-config.yaml](.pre-commit-config.yaml)). Newer clang-format
releases wrap lines differently, so do not reformat with whatever version is on
`PATH`. Format only the lines you changed:

```console
pre-commit run --files <changed files>
```

## CI

Every pull request runs:

| Workflow | Job | Notes |
| --- | --- | --- |
| [build.yml](.github/workflows/build.yml) | `build` | `idf.py build` for esp32s3 on IDF v6.0, then again with `sdkconfig.no_ota` (updates over USB off) in `build-no-ota`. The merge gate |
| [host_tests.yml](.github/workflows/host_tests.yml) | `host-tests` | the host unit tests above, plus the simulator's unit tests and scenarios, on gcc (coverage), clang (ASan/UBSan) and MSVC, all with `-Werror` |
| [web_tests.yml](.github/workflows/web_tests.yml) | `web-tests` | the console's protocol tests above |
| [e2e_tests.yml](.github/workflows/e2e_tests.yml) | `sim-console-e2e` | builds `omega_sim` and drives the console against it in headless Chrome (`tests/e2e/`) |
| [fuzz.yml](.github/workflows/fuzz.yml) | `frame-fuzz` | libFuzzer on the frame parser, see [tests/fuzz](tests/fuzz/README.md) |
| [static_analysis.yml](.github/workflows/static_analysis.yml) | `static_analysis` | cppcheck, `--std=c++17`, global suppressions in [suppressions.txt](suppressions.txt) |
| [security.yml](.github/workflows/security.yml) | `gitleaks`, `zizmor` | secret scan of the history and a lint of the workflows; also runs daily |
| [size_delta.yml](.github/workflows/size_delta.yml) | `size_delta` | reporting only, not a required check |

[package_main.yml](.github/workflows/package_main.yml) does not gate pull
requests. It builds `main` and every published release; for a full (non-pre)
release it also builds the merged full-flash image and deploys the GitHub Pages
site and release feed (see [docs/firmware-updates.md](docs/firmware-updates.md#releases-and-the-pages-site)).
Pages is already enabled for this repository, with the `github-pages`
environment allowing `main` and `v*` tags, so do not change those settings.

Build, static analysis, and host tests should all pass. Prefer an inline
`// cppcheck-suppress <id>` with a reason over adding to `suppressions.txt`.

`static_analysis.yml` runs on `pull_request_target` with a write token, which
is why it keeps `use_cmake: false` and no init script. Do not change that: it
would run untrusted PR code with write access.

## Layout

```
main/main.cpp                 sensor bring-up, calibration, USB setup, and thin adapters that drive the control core
main/Kconfig.projbuild        OMEGA_STICK_BOOT_FLASH_WINDOW_S (boot delay before USB starts, so idf.py flash works without BOOT) and
                              OMEGA_STICK_OTA (updates over USB, on by default, D25; sdkconfig.no_ota turns it off)
main/idf_component.yml        registry dependencies (espp/*, espressif/tinyusb); resolved set pinned in dependencies.lock
components/tmag5273/          Hall-effect sensor driver
components/hid-rp-mouse/      HID mouse report descriptor and input report
components/xac/               XAC gamepad report (XacInputReport)
components/omega_calibration/ calibration maths, sensor fault monitor, persisted settings, OmegaStore, the console protocol,
                              the firmware health check, and the browser console (web/, protocol code in omega_protocol.js)
components/omega_control/     pure, host-tested control-loop logic (nudge clicks, drift, sensor faults, output hold, the
                              recalibration gate) for both modes
site/                         GitHub Pages landing page and first-flash page (ESP Web Tools), published by package_main.yml
scripts/                      release_feed.mjs: builds latest.json, manifest.json and SHA256SUMS for the Pages site
tests/host/                   desktop unit tests against the real espp code (stubs only for the OTA headers)
tests/web/, tests/protocol/   Node tests for the console, and the golden frames shared with the host tests
tests/fuzz/                   libFuzzer harness for the frame parser
tests/e2e/                    manual end-to-end check of the console against omega_sim in headless Chrome (run by e2e_tests.yml)
sim/                          omega_sim, the desktop simulator: src/, OTA stubs, scenarios/ (run by ctest), tests/
cmake/omega_host.cmake        shared desktop build of espp and the local components, used by tests/host and sim
docs/                         PRD, requirements, decisions, compliance, status, and research snapshots
files/                        source directory for the littlefs image flashed with the app
models/                       STL files for the printed parts
partitions.csv                two OTA app slots of 1920 KB, littlefs 2 MB (calibration and settings files), coredump 64 KB
version.txt                   firmware MAJOR.MINOR.PATCH, see "Versioning" below
sdkconfig.defaults            project-wide config; sdkconfig itself is gitignored
```

A new local component goes in `components/<name>/` and must be added to the
`COMPONENTS` list in the top-level [CMakeLists.txt](CMakeLists.txt), and to
`REQUIRES` in [main/CMakeLists.txt](main/CMakeLists.txt) if `main` uses it.
Add registry dependencies with `idf.py add-dependency "espp/<name>>=1.0"` and
commit both `main/idf_component.yml` and `dependencies.lock`. Do not vendor
espp.

## How the firmware fits together

- **Control loops.** The mouse and XAC loops in `main.cpp` only read the
  sensor and call `mouse_tick()` / `xac_tick()` in
  [omega_control.hpp](components/omega_control/include/omega_control.hpp),
  which reaches hardware through an injected `Ports` interface. Put loop logic
  there, with a host test, not in `main.cpp` (decision D16).
- **Sensor faults.** A failed or frozen TMAG5273 read trips
  `SensorFaultMonitor`; the control core then sends a neutral report instead
  of the last deflection until the sensor recovers (decision D21).
- **Firmware updates** are on by default (`OMEGA_STICK_OTA`, decision D25 as
  amended 2026-10-25). With the option off (`sdkconfig.no_ota`, built by CI):
  no `espp::Ota`, `.ota = nullptr`, no health check, and the console hides its
  update section when GET_FW_INFO replies function_not_supported. Bootloader
  rollback and the partition table are the same either way. Updates go over the
  same console protocol into the inactive OTA slot. A new image boots pending verification and never confirms itself:
  the console reconnects, checks the version and SHA, a GET_STATE round trip and
  GET_DIAGNOSTICS, then sends MARK_VALID. `FirmwareHealthCheck` only rolls back
  an image still pending at the 120 s deadline (or at once on an init failure).
  While an upload runs, and before every console-initiated reboot,
  `CalibrationService` raises
  `Config::output_hold` and the control loop sends neutral reports (decision
  D22 rule 1); `main.cpp` counts the holds from the two services. The console
  checks the image before flashing and reconnects by itself afterwards. Full
  rules in [docs/firmware-updates.md](docs/firmware-updates.md).
- **Recalibration** is requested through a `RecalibrationGate`
  (`omega_control.hpp`) that stays busy until the sweep finishes, so a second
  request is refused with `device_or_resource_busy`. Deadzone overrides are
  bounded: `SET_DEADZONE_OVERRIDE` rejects a center value above
  `OmegaCalibration::MAX_CENTER_DEADZONE` (0.5) or a range value above
  `MAX_RANGE_DEADZONE` (0.1), `OmegaStore` ignores an out-of-range saved
  override at load, and the console checks the same limits.
- **Input mode** (mouse or XAC) is read from the persisted settings once at
  boot and picks the USB descriptor. Changing it at runtime saves settings to
  flash and calls `esp_restart()`.
- **Persistence** is two `key=value` text files on littlefs, not NVS:
  `omega_calibration.cfg` (calibration) and `omega_settings.cfg` (deadzone
  overrides, nudge-click tuning and on/off switch, input mode). They are
  deliberately separate.
- **Calibration console:** a framed request/reply protocol carried over the USB vendor interface (WebUSB) and CDC serial. The wire
  format is documented at the top of
  [calibration_service.hpp](components/omega_calibration/include/calibration_service.hpp)
  and must stay in sync with
  [omega_protocol.js](components/omega_calibration/web/omega_protocol.js),
  which [calibration_console.html](components/omega_calibration/web/calibration_console.html)
  inlines verbatim between its `BEGIN`/`END omega_protocol.js` markers (a web
  test enforces the copy). Message IDs are append-only: never renumber or
  reuse one, since released firmware and the console must keep talking to each
  other. The service is found by its dispatcher protocol id `omega.calibration`
  (`kProtocol`, version `kProtocolVersion` 1), not by module id; module 5 is
  only the default. Change the id only for an incompatible wire format, and
  bump the version otherwise (decision D24).
  See the [component README](components/omega_calibration/README.md).
- **Simulator:** [sim/src/firmware.cpp](sim/src/firmware.cpp) mirrors
  `run_omega_stick()` in `main.cpp` (boot sequence, reset-reason helpers,
  sensor fault status, the `Ports` adapter); the differences are marked
  `sim:`. When you change that wiring in `main.cpp`, make the same change
  there and run the scenarios. Its WebSocket protocol, script format and
  limits are specified in [docs/simulator.md](docs/simulator.md), and the
  console's simulator transport must stay in step with it.
- **USB ids:** `OMEGA_USB_VID` / `OMEGA_USB_PID` in `main/main.cpp` must match
  `DEFAULT_VID` / `DEFAULT_PID` in the console page. The current pair is a
  borrowed placeholder; see [docs/compliance.md](docs/compliance.md) before
  changing it.
- **USB receive callbacks run on the TinyUSB task**, where
  `UsbDevice::write_vendor()` / `write_cdc()` fail fast instead of waiting for
  buffer space. Never reply from inside a receive callback; queue the bytes to
  another task (the console uses `espp::DispatcherWorker`).

## Rules

- **Hardware verification.** There is no hardware test suite. A change to
  sensor reads, HID output, or the calibration sweep cannot be verified by an
  agent. Say so plainly in the PR and describe what a person should test
  (slow movements, held positions, rapid direction changes, the sensor
  unplugged mid-session).
- **Tests.** If you change the calibration store, the console protocol, the
  calibration file format, the control loop, or a driver's register handling,
  add or update a test in the same change. A protocol change updates the C++
  and JavaScript sides together and regenerates the golden frames.
- **Known-bug tests.** `XFAIL_TEST` marks a test that documents a current bug.
  When a fix makes it pass, the runner reports `XPASS` and the run fails until
  the marker becomes `TEST`. Change the marker in the same PR as the fix.
  Never delete an XFAIL test, or weaken it, to make CI green. Current entries,
  if any, are listed in
  [tests/host/README.md](tests/host/README.md#known-bug-tests-xfail_test).
- **Known defects** are tracked in
  [docs/requirements.md](docs/requirements.md#11-known-defects-to-fix-first)
  (DEF-1, DEF-2 in the TMAG5273 driver). They are coupled: DEF-2's dead code
  is only harmless while both range constants are equal, so fixing DEF-1 on
  its own exposes DEF-2. Fix them together.
- **Decisions.** [docs/decisions.md](docs/decisions.md) records settled
  decisions so they are not relitigated per pull request. Do not reverse one
  in passing. Open decisions listed in [docs/status.md](docs/status.md) are
  for the maintainers, not for an agent to resolve.
- **Claims.** [docs/compliance.md](docs/compliance.md) section 1 lists claims
  the project must not make (for example medical claims). Check it before
  writing any user-facing text, including README copy and code comments.
- **Research docs** under `docs/research/` are point-in-time snapshots and are
  not maintained. Anything marked `UNVERIFIED` there is not an established
  fact. `docs/status.md` can lag the code; trust the code.
- **Design principles** (full text in CONTRIBUTING.md): keep actuation force
  low and adjustable, require no host software beyond standard USB HID, keep
  configuration optional, fail visibly rather than silently, and do not assume
  the user's body (ranges and mappings stay adjustable).

## Versioning

`version.txt` holds the firmware version as `MAJOR.MINOR.PATCH`. Bump it by
hand in the PR that warrants it: MAJOR when saved calibration or settings, or
the console protocol, become incompatible; MINOR for a new user-visible
feature; PATCH for fixes only. Builds embed `X.Y.Z` only at a matching
`vX.Y.Z` tag on a clean tree, otherwise `X.Y.Z+<sha>` (plus `.dirty`). See
[docs/firmware-updates.md](docs/firmware-updates.md#firmware-versioning).

## Code style

- C++20. LLVM-based clang-format, 100 columns, 2-space indent, no tabs.
- Report errors with `std::error_code` out-parameters, as espp does, not
  exceptions.
- Match the surrounding code's naming, comment density, and idiom.
- Line endings are LF (`.gitattributes`).

## Commits and pull requests

- One logical change per PR; unrelated cleanups go in their own PR. Do not
  reformat lines you are not otherwise changing.
- Commit subject: short and imperative. Add a body explaining why when it is
  not obvious.
- Add a line under `## [Unreleased]` in [CHANGELOG.md](CHANGELOG.md) for any
  user-visible change (Added, Changed, Fixed or Removed), in the existing
  style: a bold lead-in sentence, then what it means for the user, then the
  PR link.
- Fill in [.github/pull_request_template.md](.github/pull_request_template.md).
  Tick only what was actually done: an agent can usually tick "Built locally
  with `idf.py build`" but not "Flashed and tested on hardware". State the
  host OS you built on, and that hardware testing is still needed.
- Check which branch a change should target before opening a PR. Feature work
  may be in flight on a branch other than `main`.
