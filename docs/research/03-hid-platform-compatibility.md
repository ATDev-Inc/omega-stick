# 03: HID, platform, and hardware constraints

Captured 2026-09-04. See [README](README.md) for method and caveats.

**Question asked:** What HID class should the device present? What is actually
required for console support? What are the ESP32-S3's real constraints? What
does espp already provide? What latency is achievable?

---

## Local baseline

Verified in the repository at capture: ESP-IDF 6.0.2, espp components pinned
1.2.0, `espp/usb_device` 3.0.0 pulling `espressif/esp_tinyusb` and
`espressif/tinyusb` 0.21.0. `sdkconfig.defaults` already sets
`CONFIG_TINYUSB_HID_COUNT=1` and `CONFIG_TINYUSB_CDC_ENABLED=y`, but leaves
`CONFIG_FREERTOS_HZ=1000` and 240 MHz **commented out**. `main.cpp` is a sensor
bring-up demo on the espp `qtpy` BSP at 400 kHz, address 0x35.

---

## 1. HID device class choice

### What works driverless

A generic HID **gamepad (usage 0x05)** or **joystick (0x04)** is driverless
everywhere:

| Platform | Mechanism |
| --- | --- |
| Windows | HID class driver. Appears in `joy.cpl`, DirectInput, `RawGameController` |
| macOS | IOKit HID |
| Linux / ChromeOS | evdev / joydev |
| Android | `SOURCE_GAMEPAD` |

### The two platform traps

**Windows never exposes a generic HID device via XInput.** XInput is reserved
for devices carrying Microsoft's XUSB descriptor set
([learn.microsoft.com](https://learn.microsoft.com/en-us/windows/win32/xinput/directinput-and-xusb-devices)).
Many modern titles are XInput-only and **will not see the stick at all**.

**macOS and iOS do not accept a generic gamepad into GameController.framework.**
Apple's stack gates on MFi, Xbox, DualSense, and other known PnP IDs. Devices
pair but produce no `GCController`, with an unresolved Apple bug on file
([developer.apple.com/forums/thread/741756](https://developer.apple.com/forums/thread/741756)).

> HID **mouse and keyboard** on iPadOS *is* fully supported via the
> AssistiveTouch pointer. **On iPad, mouse mode is the only working mode.**

### Workarounds for the XInput gap

| Option | Status |
| --- | --- |
| **Steam Input** | **The practical answer.** Reads generic HID and injects a virtual Xbox pad into any Steam-launched game ([partner.steamgames.com](https://partner.steamgames.com/doc/features/steam_controller/steam_input_gamepad_emulation_bestpractices)) |
| ViGEmBus | **Archived November 2023** |
| x360ce, DS4Windows | Per-game shims, fragile |
| True Xbox emulation | Requires the proprietary descriptor plus auth silicon. **Not a legitimate path** |

### Composite devices

Mouse + gamepad on separate report IDs works on Windows, Linux, and macOS, and
is exactly what the AT field already does: Makers Making Change's OpenAT
joysticks (Cedar, Redwood, Willow, Forest Hub) all ship "USB HID Mouse or USB
HID Gamepad" with a **physical mode-cycle switch**
([OpenAT-Joysticks](https://github.com/makersmakingchange/OpenAT-Joysticks)).

**Risk:** Apple forum reports suggest exotic or composite descriptors can make
iOS ignore the device entirely. This needs specific bench verification, since
iPad is the platform where mouse mode is the only option.

### Recommendation adopted

Single USB HID interface exposing **two report IDs, gamepad + mouse**, with a
host-independent mode selector that changes which report is sent, **not** the
descriptor, so the host never re-enumerates. Ship as a DirectInput-style gamepad
first, document Steam Input as the XInput answer, treat mouse mode as the
guaranteed-everywhere fallback.

---

## 2. Console compatibility

### Direct connection is closed on all three

| Console | Barrier |
| --- | --- |
| **Xbox One / Series** | Since November 2023 Microsoft blocks unlicensed accessories with error **`0x82d60002`** after a two-week grace period ([Windows Central](https://www.windowscentral.com/gaming/xbox/xboxs-new-policy-say-goodbye-to-unofficial-accessories-after-november)). Requires "Designed for Xbox" licensing and an auth IC |
| **PS5** | Requires DualSense cryptographic auth for PS5-native titles. **PS4** mode needs uploaded encryption keys or a passthrough auth device. GP2040-CE documents this and refuses to distribute keys ([gp2040-ce.info FAQ](https://gp2040-ce.info/faq/faq-console-compatibility/)) |
| **Nintendo Switch** | Does **not** accept generic USB HID. Requires a proprietary handshake (`80`-prefixed commands, baud negotiation, USB-lock) ([dekuNukem notes](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/USB-HID-Notes.md)). Switch 2 auth: **UNVERIFIED** |

Brook, Titan Two, and CronusMax are gray-area passthrough devices with terms-of-
service and ban risk.

### The XAC is the legitimate exemption

Microsoft confirmed the accessory block has **"no impact to players using the
Xbox Adaptive Controller regardless of the peripherals plugged into its USB and
3.5mm ports"**
([Can I Play That](https://caniplaythat.com/2023/10/30/xbox-unlicensed-accessory-block-sparks-adaptive-controller-concerns/)).
The console only ever sees the licensed XAC.

### XAC USB-A is proven for DIY HID joysticks

The `dinput_pluggable` project documents the exact mapping
([dinput_pluggable](https://github.com/controllercustom/dinput_pluggable),
[Arduino forum](https://forum.arduino.cc/t/joystick-for-xbox-adaptive-controller/1325039)):

| Descriptor usage | XAC target |
| --- | --- |
| X, Y | Left stick |
| Z, RZ | Right stick |
| Hat switch | D-pad |

**Requires XAC firmware June 2024 or newer.** Warning: the default button
mapping for external USB joystick buttons is reported as unexpected.

### XAC 3.5 mm ports

- **19 digital jacks**: TRS, tip-to-sleeve momentary closure against an internal
  1.8 V pull-up. Trivially driven by a GPIO or optocoupler. Digital only.
- **Analog ports X1, X2, LT, RT**: TRRS, potentiometer-style input.
  **Tip = axis 1, Ring1 = axis 2, Ring2 = GND, Sleeve = 3.3 V reference**
  ([Adafruit XAC standards](https://learn.adafruit.com/diy-adaptive-game-controllers/xac-standards)).
  This is what OpenAT's Oak and Spruce joysticks use.

### Xbox Adaptive Joystick (2025)

~$30, wired, connects to Xbox, Windows 11, or the XAC. It is itself licensed. It
does **not** change what the XAC's ports accept
([Pure Xbox](https://www.purexbox.com/news/2025/03/xboxs-new-adaptive-joystick-is-officially-now-available-to-buy/)).

### Recommendation adopted

Console support routes **through the XAC USB-A port**. Match the
`dinput_pluggable` descriptor shape (X/Y/Z/RZ + hat) so the stick maps to the
left thumbstick with no licensing. Analog TRRS output to X1/X2 needs a DAC and a
hardware variant: deferred to v2. Direct PS4/PS5/Switch/Xbox is explicitly out
of scope.

---

## 3. ESP32-S3 specifics

### Bluetooth: BLE only

ESP-IDF states plainly: **"ESP-Bluedroid for ESP32-S3 supports Bluetooth LE
only. Classic Bluetooth is not supported"**
([docs.espressif.com](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/ble/overview.html)).
Only the original ESP32 is dual-mode. **Bluetooth Classic HID is off the table
entirely.**

BLE HID (HOGP) works via Bluedroid `ble_hid_device_demo`. NimBLE has no official
Espressif HID example ([esp-idf#15220](https://github.com/espressif/esp-idf/issues/15220)),
though espp ships a real `hid_service` NimBLE GATT component.

**Host acceptance mirrors USB:** Android and Windows accept generic BLE HID
gamepads; **iOS and macOS reject them** unless the PnP ID matches a known
controller. BLE therefore buys no platform that USB does not already provide.

### USB PHY: the eFuse question

The ESP32-S3 has one full-speed (12 Mbit/s) USB-OTG plus USB-Serial-JTAG,
sharing GPIO19/20. The ESP-IDF console documentation says the internal PHY
defaults to USB_SERIAL_JTAG and can be **"permanently"** switched via the
`USB_PHY_SEL` eFuse
([usb-otg-console](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/usb-otg-console.html)).

That wording concerns the **ROM/console** case. In practice `esp_tinyusb`
re-routes the internal PHY to OTG at runtime, which is why stock S3 boards run
TinyUSB over the built-in USB-C with no eFuse burn.

> **Treat "no eFuse burn required" as high confidence but UNVERIFIED. Bench-
> verify on the QT Py before committing hardware.** If wrong, every unit needs an
> irreversible eFuse burn. Open item, to be resolved before a hardware commit.

### Consequences either way

- Once TinyUSB owns the port, the USB-Serial-JTAG console is gone.
- `CONFIG_ESP_CONSOLE_USB_CDC` (ROM CDC) is **incompatible** with TinyUSB.
- Reflashing typically needs the BOOT+RESET combo
  ([esptool troubleshooting](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/troubleshooting.html)).

### Known rough edges

- **Sustained 1 kHz HID on ESP32-S3 + TinyUSB: UNVERIFIED.** No benchmark found.
- **Endpoint stalls after consecutive HID reports larger than 64 bytes**
  ([esp-idf#9992](https://github.com/espressif/esp-idf/issues/9992)). Keep
  reports small.

### Recommendation adopted

USB-only. BLE explicitly deferred, and never a path to iOS gamepad support. Add
a CDC-ACM interface alongside HID in the same TinyUSB composite for logging and
an optional config CLI, plus a UART console on spare GPIOs for bring-up. Set
`CONFIG_FREERTOS_HZ=1000` and 240 MHz in `sdkconfig.defaults`.

---

## 4. espp capabilities and gaps

### What exists

**`hid-rp`** provides
`espp::GamepadInputReport<BUTTON_COUNT=15, JOYSTICK_TYPE=uint16_t, ...>` with
**X/Y left stick, Z/RZ right stick, 2 triggers, 4-bit hat, N buttons**, plus
`set_left_joystick(float,float)` in [-1,1], `set_button`, `set_hat`,
`get_report()`, and `static get_descriptor()`. It also ships Xbox, Switch Pro,
DualShock 4, and DualSense report and descriptor sets
([hid-rp-gamepad.hpp](https://github.com/esp-cpp/espp/blob/main/components/hid-rp/include/hid-rp-gamepad.hpp)).

> **This layout is a direct match for the XAC mapping in section 2.**

Caveat: `get_descriptor()` returns a fragment the caller must wrap in
`usage_page<generic_desktop>` / `usage(GAMEPAD)` / `collection::application`.

**`joystick`** provides `espp::Joystick` with
`Config{x_calibration, y_calibration (FloatRangeMapper::Config), type
RECTANGULAR|CIRCULAR, center_deadzone_radius, range_deadzone, get_values_fn}`,
plus `update()`, `x()/y()/position()/raw()`, and `set_calibration()`.
`FloatRangeMapper::Config` carries `center, center_deadband, minimum, maximum,
range_deadband, invert_output`.

> **There is no `calibrate()` routine, and mapping is linear only.**

**`usb_device`** wraps `esp_tinyusb`.
`HidFunction{interface_name, std::vector<uint8_t> report_descriptor,
has_out_endpoint=false, poll_interval_ms=10}` where `poll_interval_ms` becomes
bInterval. Also `write_hid_report(report_id, span, ec)` and `is_hid_ready()`.
CDC and vendor/WebUSB functions live in the same `Config`, so the composite
device in section 3 is a configuration change, not new code.

> **`poll_interval_ms` defaults to 10, which is only 100 Hz.** This must be
> overridden explicitly. Recorded as PRD requirement OUT-5.

Also available: `math` (`RangeMapper`, `bezier.hpp`, `vector2d`), `filters`
(lowpass, butterworth, biquad, kalman), `nvs`, and `hid_service` (BLE HOGP).

### Gaps Omega Stick must write itself

- **The glue from sensor to `Joystick` to `GamepadInputReport` to
  `write_hid_report`. No such example exists anywhere in espp.**
- NVS calibration schema
- Boot and idle auto-centering
- Exponential and response-curve shaping (`RangeMapper` is linear;
  `bezier.hpp` is the building block)
- The gamepad/mouse mode switch

---

## 5. Sensor and latency budget

### TMAG5273 characteristics

| Property | Value |
| --- | --- |
| ADC | **12-bit**, 3-axis |
| Sample rate | **20 kSPS single-axis / 10 kSPS 3-axis** at CONV_AVG=1x |
| I2C | up to 1 MHz |
| Current | 2.3 mA active |
| Ranges | **x1 part ±40 / ±80 mT; x2 part ±133 / ±266 mT** |
| Sensitivity drift | **5% typical** over -40 to 125 °C |

Source: [ti.com/product/TMAG5273](https://www.ti.com/product/TMAG5273)

Averaging trades rate for noise: 4x ≈ 3.1 kSPS 3-axis, 32x ≈ 0.4 kSPS.

**RMS noise figures: UNVERIFIED**, not extractable from the PDF this session.
Open item requiring bench measurement.

At ±40 mT, one LSB ≈ **19.5 µT** (computed). The x1 part at ±40 mT is the right
choice for a small magnet over a few millimeters.

### Drift requires runtime re-centering

The on-chip offset registers are a **static user trim, not drift tracking**.
With 5% typical sensitivity drift across temperature, **runtime re-centering is
required, not optional.** The on-chip temperature sensor plus MAG_TEMPCO
compensates NdFeB and ceramic magnets.

### I2C throughput (computed, 400 kHz)

| Read | Time | Bus-limited rate |
| --- | --- | --- |
| XY-only, 4 bytes | ≈158 µs | ≈6.3 kHz |
| XYZ, 6 bytes | ≈202 µs | ≈4.9 kHz |

Sensor + I2C + filtering is comfortably **under 0.5 ms**. **The sensor is never
the bottleneck.**

The device has an **INT pin** usable as data-ready, and 1-byte read modes that
skip the register-address write.

### End-to-end latency (computed)

| bInterval | End-to-end |
| --- | --- |
| 1 ms | **2 to 3 ms** |
| 4 ms | 4 to 6 ms |
| 8 ms | 6 to 9 ms |

Human thresholds are far coarser (~67 ms for twitch genres,
[Input lag](https://en.wikipedia.org/wiki/Input_lag)). Commonly cited Xbox pad
125 Hz and DualSense 250 Hz figures are **UNVERIFIED**.

### Findings in the local driver

Reviewed `components/tmag5273/include/tmag5273.hpp`:

- `read_raw()` already does a **6-byte burst** from `X_MSB_RESULT`, so the fast
  path exists, **but there is no XY-only 4-byte read**.
- `INT_CONFIG_1`, `MAG_GAIN_CONFIG`, and `MAG_OFFSET_CONFIG_1/2` are **declared
  and never used**, so there is no data-ready interrupt and no on-chip trim.
- **Defect:** `range_xy_mT()` returns `266.0f` unconditionally with a comment
  acknowledging it, and `convert_raw_to_mT` consumes it as the LOW full-scale
  while doubling for HIGH. `read_z()` passes `z_range_` correctly but
  `convert_raw_to_mT` always reaches for the XY constant, leaving `range_z_mT()`
  dead. Full analysis in the PRD's known-defects section.

### Recommendation adopted

x1 part at ±40 mT, **CONV_AVG 4x** (≈3.1 kSPS, real noise reduction, still ~10x
faster than needed), continuous mode, **XY 4-byte burst polled at 1 kHz from a
dedicated pinned task**, decimated to a **1 ms bInterval HID report sent on
change**. Fix the range defect and **work the joystick math in raw counts** so
the mT bug cannot affect feel. Wire the INT pin as data-ready if the board
exposes it. Do runtime re-centering (boot capture plus slow idle drift
correction) to absorb thermal sensitivity drift.
