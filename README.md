# Omega Stick

**A low-force joystick for people with disabilities.** Omega Stick is an
accessible input device that needs under 10 grams of force to actuate, for
computer control and gaming.

[![Build and Package Main](https://github.com/ATDev-Inc/omega-stick/actions/workflows/package_main.yml/badge.svg)](https://github.com/ATDev-Inc/omega-stick/actions/workflows/package_main.yml)
[![Static analysis](https://github.com/ATDev-Inc/omega-stick/actions/workflows/static_analysis.yml/badge.svg)](https://github.com/ATDev-Inc/omega-stick/actions/workflows/static_analysis.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)

This repository holds the **firmware**: an ESP-IDF project for the ESP32-S3,
written in C++20 on top of the [ESP++ (espp)](https://github.com/esp-cpp/espp)
component library.

## Contents

- [Why low force](#why-low-force)
- [Project status](#project-status)
- [Hardware](#hardware)
- [Getting started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Build and flash](#build-and-flash)
  - [Expected output](#expected-output)
- [Repository layout](#repository-layout)
- [Contributing](#contributing)
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
mechanism alone, targeting **under 10 grams**.

Because the sensing is contactless there is also nothing in the signal path to
wear out, which matters for a device someone may rely on daily.

The stick presents itself to the host over USB as a standard **HID** device, so
it works without drivers or vendor software on the host.

## Project status

**Early development.** This is pre-release firmware and the interfaces are not
stable yet.

What is in the tree today:

- A `TMAG5273` driver ([components/tmag5273/include/tmag5273.hpp](components/tmag5273/include/tmag5273.hpp))
  covering configuration, magnetic X/Y/Z reads, temperature, and conversion status.
- A sensor bring-up application ([main/main.cpp](main/main.cpp)) that probes the
  sensor, dumps its configuration, and streams live magnetic readings to the
  console.

Not implemented yet: the HID report descriptor, axis calibration and centering,
deadzone and response curve shaping, and the USB gamepad/mouse output path. The
`espp/hid-rp`, `espp/joystick`, and `espp/usb_device` dependencies are declared
in [main/idf_component.yml](main/idf_component.yml) in preparation for that work.

If you are looking for somewhere to help, those are the open areas. See
[CONTRIBUTING.md](./CONTRIBUTING.md).

## Hardware

The firmware currently targets:

| Part | Notes |
| --- | --- |
| MCU | ESP32-S3, configured via the ESP++ `qtpy` board support |
| Sensor | TI TMAG5273 3-axis Hall-effect sensor, I2C address `0x35`, 400 kHz |
| Bus | Qwiic / STEMMA QT I2C |
| Host link | USB, via TinyUSB |

The mechanical design, PCB, and enclosure are not part of this repository. If
you want to build one, open a
[discussion](https://github.com/ATDev-Inc/omega-stick/discussions) or an issue
and we will point you at what exists.

## Getting started

### Prerequisites

This is an ESP-IDF project targeting **ESP-IDF v5.5.1 or v6.0**. Follow the
[official ESP-IDF getting started
guide](https://docs.espressif.com/projects/esp-idf/en/release-v6.0/esp32s3/get-started/index.html)
to install the toolchain for your platform.

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

### Expected output

With a TMAG5273 connected on the Qwiic bus, the bring-up app prints the device
and manufacturer IDs, the active configuration, and then a live stream of
readings. Move a magnet near the sensor and the values should track it:

```
========================================
       TMAG5273 Driver Test
========================================
I2C Address: 0x35
========================================

Probing for TMAG5273...
TMAG5273 found!

X:    1.234 mT   Y:   -0.567 mT   Z:   12.345 mT   T:  24.50 C   Ready: YES
```

If you see `ERROR: TMAG5273 not found at address 0x35`, check the Qwiic cable
and confirm the sensor's address strap.

## Repository layout

```
main/                     application entry point and IDF component manifest
components/tmag5273/      TMAG5273 Hall-effect sensor driver
.github/workflows/        build, packaging, and static analysis CI
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

## License

Released under the [MIT License](./LICENSE).

## Acknowledgements

Built on [ESP++ (espp)](https://github.com/esp-cpp/espp) and started from the
[esp-cpp/template](https://github.com/esp-cpp/template) project template, both
by [esp-cpp](https://github.com/esp-cpp).
