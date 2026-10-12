# Omega Stick

<img src="assets/logo/lockup.svg" alt="Omega Stick" width="360">

**A low-force joystick for accessible computer control and gaming.** Omega
Stick is an accessible input device that presents itself as a standard USB
mouse or gamepad. Its actuation force is designed to be adjustable across
roughly 5 to 25 grams, defaulting below 10 (a design target until the
measurements in [docs/status.md](docs/status.md) are published).

[![Build and Package Main](https://github.com/ATDev-Inc/omega-stick/actions/workflows/package_main.yml/badge.svg)](https://github.com/ATDev-Inc/omega-stick/actions/workflows/package_main.yml)
[![Static analysis](https://github.com/ATDev-Inc/omega-stick/actions/workflows/static_analysis.yml/badge.svg)](https://github.com/ATDev-Inc/omega-stick/actions/workflows/static_analysis.yml)
[![Host tests](https://github.com/ATDev-Inc/omega-stick/actions/workflows/host_tests.yml/badge.svg)](https://github.com/ATDev-Inc/omega-stick/actions/workflows/host_tests.yml)
[![Web tests](https://github.com/ATDev-Inc/omega-stick/actions/workflows/web_tests.yml/badge.svg)](https://github.com/ATDev-Inc/omega-stick/actions/workflows/web_tests.yml)
[![Fuzz](https://github.com/ATDev-Inc/omega-stick/actions/workflows/fuzz.yml/badge.svg)](https://github.com/ATDev-Inc/omega-stick/actions/workflows/fuzz.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)

This repository holds the **firmware**: an ESP-IDF project for the ESP32-S3,
written in C++20 on top of the [ESP++ (espp)](https://github.com/esp-cpp/espp)
component library.

## Contents

- [Why low force](#why-low-force)
- [Project status](#project-status)
- [Documentation](#documentation)
- [Firmware updates and versioning](#firmware-updates-and-versioning)
- [Hardware](#hardware)
- [Getting started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Build and flash](#build-and-flash)
  - [Expected output](#expected-output)
  - [Calibration console and input modes](#calibration-console-and-input-modes)
  - [Build options](#build-options)
- [Repository layout](#repository-layout)
- [Contributing](#contributing)
- [Getting help](#getting-help)
- [License](#license)
- [Acknowledgements](#acknowledgements)

## Why low force

Conventional joysticks use a mechanical gimbal and centering springs. You have
to physically push against that spring on every input, and the force required is
typically well over 100 grams. For someone with limited strength, reduced range
of motion, tremor, or fatigue, that resistance is the difference between an
input device that works and one that does not.

Omega Stick removes the mechanical resistance from the sensing path. It uses a
**contactless 3-axis Hall-effect sensor** (TI TMAG5273) that measures the
position of a small magnet, so nothing has to be depressed, wiped, or deflected
against a spring to register movement. The actuation force is set by the return
mechanism alone, and is designed to be **adjustable from roughly 5 to 25
grams, defaulting below 10**. Those figures are design targets: the force and
throw measurements are not published yet (see
[docs/status.md](docs/status.md)).

Adjustability matters as much as the floor. Vendors at the low-force frontier
report that a stick can be *too* light to control well, so the usable range
matters more than the minimum, and which point in it works is a property of the
person rather than the device. See D12 in [docs/decisions.md](docs/decisions.md).

Low force on its own is not what makes this project unusual: several sub-10 gram
sticks already ship. They are closed, mostly dealer-gated, and often unpriced.
The gap is the combination of low force with an open design, a plain USB HID
interface, a published price, and a published method for measuring the force
claim. See [docs/PRD.md](docs/PRD.md).

Because the sensing is contactless there is also nothing in the signal path to
wear out, which matters for a device someone may rely on daily.

The stick presents itself to the host over USB as a standard **HID** device, so
it works without drivers or vendor software on the host.

## Project status

**Early development.** This is pre-release firmware and the interfaces are not
stable yet. For component-level detail and known gaps, see
[docs/status.md](docs/status.md).

What works today:

- Sensor driver and calibration: a `TMAG5273` driver
  ([components/tmag5273/include/tmag5273.hpp](components/tmag5273/include/tmag5273.hpp)),
  center and range calibration, and idle drift correction, persisted to flash.
- **Mouse mode**: the default. Relative X/Y plus stick nudge gestures for clicks
  and scroll.
- **Browser calibration console**: WebUSB and Web Serial, for viewing and tuning
  settings, switching input mode, and re-running calibration.
- **Firmware updates over USB** from the console, with automatic rollback if a
  new image does not come up healthy, and a button to revert to the previous
  firmware. See [Firmware updates and versioning](#firmware-updates-and-versioning).
- **Sensor fault handling**: if the sensor stops answering or freezes, the
  device reports a centered stick until it recovers.
- **Tests in CI**: host unit tests against the real espp code (calibration,
  control loop, console protocol, sensor driver, HID reports), Node tests for
  the console, and fuzzing of the frame parser. No hardware-in-the-loop tests
  yet.

Experimental: **XAC (Xbox Adaptive Controller) gamepad mode** is implemented but
has not been verified on real XAC hardware.

Not implemented yet: response curves, per-axis inversion and gain, tremor
filtering, profiles, and a status LED.

If you are looking for somewhere to help, those are the open areas. See
[CONTRIBUTING.md](./CONTRIBUTING.md).

## Documentation

Product and planning documents live in [docs/](docs/):

| Document | Contents |
| --- | --- |
| [PRD.md](docs/PRD.md) | What v1 is, who it is for, scope, and success metrics |
| [requirements.md](docs/requirements.md) | Functional and non-functional specification |
| [decisions.md](docs/decisions.md) | Product decisions with rationale, and what is still open |
| [compliance.md](docs/compliance.md) | What the project may and may not claim about itself |
| [status.md](docs/status.md) | Current working status |
| [firmware-updates.md](docs/firmware-updates.md) | USB firmware updates, rollback, and versioning |
| [CHANGELOG.md](CHANGELOG.md) | What changed in each release |
| [research/](docs/research/) | Background research these were built from |
| [assets/logo/](assets/logo/) | Brand marks, wordmark, the animated hero scene, and usage notes |

Research documents are point-in-time snapshots and are not maintained.

## Firmware updates and versioning

A new board is flashed once, from the browser at the
[flash page](https://atdev-inc.github.io/omega-stick/flash.html) or with
`idf.py -p PORT flash`. (GitHub Pages is set up for this repository, but the
flash page and the hosted console only go live with the first full release
built by the current workflow.) After that, firmware is updated over USB from the
calibration console: open it (the copy at
<https://atdev-inc.github.io/omega-stick/console/> works too), connect, and
choose or drag in the app image (`build/omega_stick.bin`, or a release's
`omega_stick-X.Y.Z.bin`) in the "Firmware update" section. The page checks the
file, shows what is running and what will replace it, and reconnects by itself
afterwards. A new image must prove itself (USB enumerated, sensor answering, ten
seconds of running) before it is kept; otherwise the device reverts to the
previous image, and the console's "Revert to previous firmware" button goes
back on demand. The console can also check for the latest release and install
it after verifying its size and SHA-256. The device holds its output neutral
while an update is in flight. Versions are `MAJOR.MINOR.PATCH` from `version.txt` (release builds
report exactly that; other builds append `+<commit>`). Please paste the
console's Copy line in bug reports. A first flash erases saved calibration and
settings; console updates keep them. Details are in
[docs/firmware-updates.md](docs/firmware-updates.md).

## Hardware

The firmware currently targets:

| Part | Notes |
| --- | --- |
| MCU | ESP32-S3, configured via the ESP++ `qtpy` board support |
| Sensor | TI TMAG5273 3-axis Hall-effect sensor, I2C address `0x35`, 400 kHz |
| Bus | Qwiic / STEMMA QT I2C |
| Host link | USB, via TinyUSB |

**Never plug or unplug the Qwiic cable (or any similar connector) while the
board is powered.** These connectors do not make power and ground first and
break them last, which can damage the ESP32's I2C pins. Unplug USB, change the
cable, then plug USB back in.

STL files for the printed parts (joystick, magnet and sensor holders, sockets,
topper) are in [models/](models/). The PCB, wiring, and editable CAD source are
not part of this repository. If you want to build one, open a
[discussion](https://github.com/ATDev-Inc/omega-stick/discussions) or an issue
and we will point you at what exists.

## Getting started

### Prerequisites

This is an ESP-IDF project targeting **ESP-IDF v6.0**, the version CI builds with (v5.5.1 is
untested). [dependencies.lock](./dependencies.lock) was resolved against v6.0.2. Follow the
[official ESP-IDF getting started
guide](https://docs.espressif.com/projects/esp-idf/en/release-v6.0/esp32s3/get-started/index.html)
to install the toolchain for your platform. On Windows, Espressif's installer
can do it in two commands (see [CONTRIBUTING.md](./CONTRIBUTING.md#development-environment)).

Component dependencies are pulled automatically by the IDF Component Manager
from [main/idf_component.yml](main/idf_component.yml); the resolved set is
pinned in [dependencies.lock](./dependencies.lock). You do not need to vendor
`espp` yourself.

### Build and flash

```console
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with your serial port (`COM7` on Windows, `/dev/ttyACM0` on
Linux, `/dev/cu.usbmodem*` on macOS). Exit the monitor with `Ctrl-]`.

### Run the host tests

Most of the firmware logic has unit tests that run on your desktop without a
board or ESP-IDF: the calibration maths and persistence, the console protocol,
the TMAG5273 driver, the HID reports and descriptors, and the control loop.
They need CMake and a C++20 compiler:

```console
cmake -S tests/host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

The calibration console's protocol code has its own tests under Node:

```console
node --test "tests/web/*.test.js"
```

See [tests/host/README.md](tests/host/README.md) and
[tests/web/README.md](tests/web/README.md) for what they cover. CI runs both,
plus a short fuzzing pass over the frame parser ([tests/fuzz](tests/fuzz/README.md)).

### Calibration console

[components/omega_calibration/web/calibration_console.html](components/omega_calibration/web/calibration_console.html)
is a browser page for viewing and tuning the stick's calibration and settings
over USB. Open it from disk in Chrome or Edge and connect with WebUSB or Web
Serial. See the [omega_calibration README](components/omega_calibration/README.md)
for details.

No stick to hand? Run the desktop simulator (`omega_sim`, see
[docs/simulator.md](docs/simulator.md)) and choose **Connect Simulator**. The
console then works as with a real stick and adds a Simulator panel: a virtual
stick, built-in motion patterns, sensor faults, power-cycle and wipe, script
upload, and a view of the mouse or gamepad output.

### Expected output

With a TMAG5273 connected on the Qwiic bus, the serial monitor shows the sensor
coming up and then the calibration step:

```
Initializing TMAG5273...
TMAG5273 initialized successfully!
```

On a board with no saved calibration, the firmware then runs an interactive
center calibration followed by a range calibration, prints the results, and
starts USB (`Waiting for USB host...`, `HID ready: yes`). The calibration is
saved to the `littlefs` partition and loaded on later boots
(`Loaded existing calibration.`).

If you see `ERROR: TMAG5273 initialization failed`, check the Qwiic cable (with USB
unplugged) and confirm the sensor's address strap (the firmware expects `0x35`).

### Calibration console and input modes

The stick boots as a USB HID **mouse** by default. It can instead present as an
Xbox Adaptive Controller style gamepad (XAC). The mode is a saved setting, and
switching it restarts the device to apply it.

The browser-based calibration console,
[components/omega_calibration/web/calibration_console.html](components/omega_calibration/web/calibration_console.html),
talks to the stick over WebUSB (vendor interface) or Web Serial (CDC). Open the
file in a Chromium-based browser (Chrome, Edge, Opera) from `file://`,
`http://localhost`, or an https origin, then connect. From there you can:

- view live calibration and settings, and adjust the manual center offset,
  deadzone overrides, and nudge-click tuning;
- switch between mouse and XAC mode;
- re-run the full center and range calibration (use Web Serial to see the
  prompts; the button stays disabled until the result arrives);
- save the current settings to flash.

### Build options

`idf.py menuconfig`, under "Omega Stick Configuration", has one option:
`OMEGA_STICK_BOOT_FLASH_WINDOW_S` (default 0, range 0 to 60) is how many
seconds the firmware waits at boot before starting USB. While it waits, the
built-in USB Serial/JTAG still owns the port, so `idf.py flash` works without
holding BOOT. As a developer, set it locally in `idf.py menuconfig` (your
`sdkconfig` is gitignored); leave it at 0 for anything users run. The flash layout is in
[partitions.csv](partitions.csv) and assumes the 8 MB QT Py ESP32-S3.

## Repository layout

The short version is below. [DIRECTORY.md](DIRECTORY.md) maps every directory and the files that matter in each.

```
main/                     application entry point, IDF component manifest, Kconfig options
components/tmag5273/      TMAG5273 Hall-effect sensor driver
components/omega_calibration/
                          calibration, persisted settings, the USB console protocol,
                          and the web calibration console (web/)
components/omega_control/ the control loop's logic (nudge clicks, drift, recalibration
                          hand-off), pure and host-tested; main.cpp wires hardware to it
components/hid-rp-mouse/  HID mouse report descriptor and input report
components/xac/           Xbox Adaptive Controller gamepad report
tests/host/               desktop unit tests for the components above (no hardware needed)
tests/web/                Node tests for the console's protocol code
tests/protocol/           golden frames generated by the C++ service, checked by both suites
site/                     GitHub Pages landing page and first-flash page, published with each full release
scripts/                  release_feed.mjs, which builds the release feed for the Pages site
tests/fuzz/               libFuzzer harness and seed corpus for the frame parser
tests/e2e/                manual end-to-end check of the console against the simulator
sim/                      omega_sim, a desktop simulator of the firmware for testing without hardware
cmake/                    shared CMake setup for the host tests and the simulator
models/                   STL files for the printed parts (joystick, magnet and sensor holders,
                          QT Py holder, sockets, topper)
docs/                     product, requirements, decisions, and research documents
assets/logo/              brand assets
.github/workflows/        build, packaging and Pages publishing, size delta, static analysis, security scans, host, web and fuzz test CI
partitions.csv            flash partition table (nvs, otadata, phy_init, two OTA app slots, littlefs, coredump)
sdkconfig.defaults        project-wide ESP-IDF configuration defaults
```

## Contributing

Contributions are welcome, and not only code.

- **Are you a user of assistive technology?** Feedback about how the stick
  actually performs for you is the most valuable input this project can get. You
  do not need to know anything about programming or electronics to file
  [accessibility
  feedback](https://github.com/ATDev-Inc/omega-stick/issues/new/choose).
- **Are you a developer?** See [CONTRIBUTING.md](./CONTRIBUTING.md) for
  environment setup, code style, and the pull request process.

Everyone participating in this project is expected to follow the [Code of
Conduct](./CODE_OF_CONDUCT.md). To report a security issue, see
[SECURITY.md](./SECURITY.md).

## Getting help

Questions go to [GitHub Discussions](https://github.com/ATDev-Inc/omega-stick/discussions),
bugs and requests to the [issue forms](https://github.com/ATDev-Inc/omega-stick/issues/new/choose),
and security reports privately as described in [SECURITY.md](SECURITY.md). See
[SUPPORT.md](SUPPORT.md) for what this project can and cannot help with.

## License

Released under the [MIT License](./LICENSE). The firmware image also contains
third-party code under its own licenses (MIT, Apache-2.0, BSD-3-Clause,
MPL-2.0 and BSL-1.0), plus the libraries ESP-IDF itself bundles; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Acknowledgements

Built on [ESP++ (espp)](https://github.com/esp-cpp/espp) and started from the
[esp-cpp/template](https://github.com/esp-cpp/template) project template, both
by [esp-cpp](https://github.com/esp-cpp).
