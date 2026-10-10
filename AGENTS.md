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

Firmware (needs ESP-IDF v5.5.1 or v6.0; `dependencies.lock` was resolved
against v6.0.2, CI uses `v6.0`):

```console
idf.py set-target esp32s3
idf.py build
```

Never run `idf.py flash` or `idf.py monitor` unless the user explicitly asks:
a board may or may not be attached, and flashing changes the user's hardware.

Host unit tests (no ESP-IDF, no board; CMake 3.20+ and a C++20 compiler with
`<format>`):

```console
cmake -S tests/host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

On Windows toolchains the host build fails in the pinned espp `hid-rp`
headers (`std::bit_cast` without `<bit>`). Use the force-include workaround in
[tests/host/README.md](tests/host/README.md#component-quirks-the-host-build-surfaced).

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
| [build.yml](.github/workflows/build.yml) | `build` | `idf.py build` for esp32s3 on IDF v6.0. The merge gate |
| [host_tests.yml](.github/workflows/host_tests.yml) | `host-tests` | the host unit tests above |
| [static_analysis.yml](.github/workflows/static_analysis.yml) | `static_analysis` | cppcheck, `--std=c++17`, global suppressions in [suppressions.txt](suppressions.txt) |
| [size_delta.yml](.github/workflows/size_delta.yml) | `size_delta` | reporting only, not a required check |

Build, static analysis, and host tests should all pass. Prefer an inline
`// cppcheck-suppress <id>` with a reason over adding to `suppressions.txt`.

`static_analysis.yml` runs on `pull_request_target` with a write token, which
is why it keeps `use_cmake: false` and no init script. Do not change that: it
would run untrusted PR code with write access.

## Layout

```
main/main.cpp                 the whole application: sensor bring-up, calibration, mouse and XAC loops, USB setup
main/Kconfig.projbuild        OMEGA_STICK_BOOT_FLASH_WINDOW_S (boot delay before USB starts, so idf.py flash works without BOOT)
main/idf_component.yml        registry dependencies (espp/*, espressif/tinyusb); resolved set pinned in dependencies.lock
components/tmag5273/          Hall-effect sensor driver
components/hid-rp-mouse/      HID mouse report descriptor and input report
components/xac/               XAC gamepad report (XacInputReport)
components/omega_calibration/ calibration maths, persisted settings, OmegaStore, the console protocol, and the browser console (web/)
tests/host/                   desktop unit tests for omega_calibration and xac, with stubs for the ESP-IDF-only espp pieces
docs/                         PRD, requirements, decisions, compliance, status, and research snapshots
files/                        source directory for the littlefs image flashed with the app
models/                       STL files for the printed parts
partitions.csv                factory app 4 MB, littlefs 2 MB (calibration and settings files), coredump 64 KB
sdkconfig.defaults            project-wide config; sdkconfig itself is gitignored
```

A new local component goes in `components/<name>/` and must be added to the
`COMPONENTS` list in the top-level [CMakeLists.txt](CMakeLists.txt), and to
`REQUIRES` in [main/CMakeLists.txt](main/CMakeLists.txt) if `main` uses it.
Add registry dependencies with `idf.py add-dependency "espp/<name>>=1.0"` and
commit both `main/idf_component.yml` and `dependencies.lock`. Do not vendor
espp.

## How the firmware fits together

- **Input mode** (mouse or XAC) is read from the persisted settings once at
  boot and picks the USB descriptor. Changing it at runtime saves settings to
  flash and calls `esp_restart()`.
- **Persistence** is two `key=value` text files on littlefs, not NVS:
  `omega_calibration.cfg` (calibration) and `omega_settings.cfg` (deadzone
  overrides, nudge-click tuning, input mode). They are deliberately separate.
- **Calibration console:** a framed request/reply protocol (dispatcher module
  5) carried over the USB vendor interface (WebUSB) and CDC serial. The wire
  format is documented at the top of
  [calibration_service.hpp](components/omega_calibration/include/calibration_service.hpp)
  and must stay in sync with
  [calibration_console.html](components/omega_calibration/web/calibration_console.html).
  See the [component README](components/omega_calibration/README.md).
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
- **Tests.** If you change the calibration store, the console protocol, or the
  calibration file format, add or update a host test in the same change.
- **Known-bug tests.** `XFAIL_TEST` marks a test that documents a current bug.
  When a fix makes it pass, the runner reports `XPASS` and the run fails until
  the marker becomes `TEST`. Change the marker in the same PR as the fix.
  Never delete an XFAIL test, or weaken it, to make CI green. The current list
  is in [tests/host/README.md](tests/host/README.md#known-bug-tests-xfail_test).
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
- Fill in [.github/pull_request_template.md](.github/pull_request_template.md).
  Tick only what was actually done: an agent can usually tick "Built locally
  with `idf.py build`" but not "Flashed and tested on hardware". State the
  host OS you built on, and that hardware testing is still needed.
- Check which branch a change should target before opening a PR. Feature work
  may be in flight on a branch other than `main`.
