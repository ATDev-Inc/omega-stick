# Third-party notices

This firmware is released under the MIT License (see [LICENSE](LICENSE)). The
firmware binary statically includes ESP-IDF and the managed components resolved
in [dependencies.lock](dependencies.lock), and each of them is distributed under
its own license. This file lists them with the exact versions in the lock. The
full license text ships with each component (in its source tree, fetched by the
ESP-IDF component manager into `managed_components/` at build time) and is
linked below. The calibration console
([components/omega_calibration/web](components/omega_calibration/web)) is a
self-contained page and loads no third-party code.

Licenses were checked against the `license:` field of each component's
`idf_component.yml` and its LICENSE file or source headers.

| Component | Version | License | Copyright | Source |
| --- | --- | --- | --- | --- |
| ESP-IDF | 6.0.2 | Apache-2.0 | Espressif Systems (Shanghai) CO LTD | https://github.com/espressif/esp-idf |
| espp/base_component | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/base_component |
| espp/base_peripheral | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/base_peripheral |
| espp/cli | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/cli |
| espp/color | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/color |
| espp/coredump | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/coredump |
| espp/dispatcher | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/dispatcher |
| espp/file_system | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/file_system |
| espp/format | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/format |
| espp/hid-rp | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/hid-rp |
| espp/i2c | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/i2c |
| espp/interrupt | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/interrupt |
| espp/joystick | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/joystick |
| espp/logger | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/logger |
| espp/math | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/math |
| espp/neopixel | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/neopixel |
| espp/ota | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/ota |
| espp/qtpy | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/qtpy |
| espp/rmt | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/rmt |
| espp/stream_frame | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/stream_frame |
| espp/task | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/task |
| espp/timer | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/timer |
| espp/usb_device | 1.3.6 | MIT | esp-cpp (see note) | https://github.com/esp-cpp/espp/tree/main/components/usb_device |
| espp/format, bundled fmt (libfmt) | 12.1.1 (via espp/format 1.3.6) | MIT (fmt license) | Victor Zverovich and {fmt} contributors | https://github.com/fmtlib/fmt |
| espp/hid-rp, bundled hid-rp | via espp/hid-rp 1.3.6 | MPL-2.0 | Benedek Kupper | https://github.com/intergatedcircuits/hid-rp |
| espp/cli, bundled cli | via espp/cli 1.3.6 | BSL-1.0 (Boost Software License 1.0) | Daniele Pallastrelli | https://github.com/daniele77/cli |
| espressif/tinyusb | 0.21.0~1 | MIT | hathach (tinyusb.org) | https://github.com/espressif/tinyusb |
| espressif/esp_tinyusb | 2.2.1 | Apache-2.0 | Espressif Systems (Shanghai) CO LTD | https://github.com/espressif/esp-usb/tree/master/device/esp_tinyusb |
| joltwallet/littlefs (esp_littlefs port) | 1.20.1 | MIT | Brian Pugh | https://github.com/joltwallet/esp_littlefs |
| littlefs (ARM, bundled in joltwallet/littlefs) | as bundled in 1.20.1 | BSD-3-Clause | The littlefs authors; Arm Limited | https://github.com/littlefs-project/littlefs |

Notes:

- The espp components are published by the esp-cpp project
  (https://github.com/esp-cpp/espp). Their `idf_component.yml` files declare
  MIT. The component packages do not carry a separate copyright line, so the
  holder is given as the project name; see the upstream repository for its
  LICENSE.
- espp/hid-rp bundles the hid-rp library under MPL-2.0. This project uses it
  unmodified through the espp component and does not alter its source files.
- `idf` 6.0.2 in the lock is ESP-IDF itself, which is not fetched into
  `managed_components/`.

## Apache-2.0 components

The Apache License, Version 2.0 applies to ESP-IDF and esp_tinyusb. The license
text is at https://www.apache.org/licenses/LICENSE-2.0 and is included in each
of those components' source trees.

## Code in this repository

The `components/` directory holds this project's own code: `omega_calibration`,
`omega_control`, `tmag5273` and `xac`, and `hid-rp-mouse`. No file header or
README credits another origin for them. They are written against the espp APIs
(`xac` and `hid-rp-mouse` use the espp/hid-rp report classes, and `tmag5273`
follows the espp driver conventions), but no copied third-party source was
identified.

## About LICENSE

The `Copyright (c) 2023 esp-cpp` line in [LICENSE](LICENSE) comes from the
esp-cpp/template project (MIT) that this repository started from.
