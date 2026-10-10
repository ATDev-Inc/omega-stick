# omega_calibration

Calibration maths, persisted settings, and the USB console protocol for the
Omega Stick.

| Header | What it holds |
| --- | --- |
| `omega_calibration.hpp` | center and range calibration, deadzones, `key=value` file persistence |
| `omega_store.hpp` | the live calibration and settings behind one mutex, with change callbacks |
| `calibration_service.hpp` | the framed request/reply protocol (dispatcher module 5) that the console talks to |
| `web/calibration_console.html` | the browser console, a single self-contained page |

## Calibration console

`web/calibration_console.html` lets you view and change the stick's live
calibration and settings from a browser, then save them to the device. The
firmware does not serve the page. Open the file directly from disk in a
Chromium-based browser (Chrome or Edge), which provides WebUSB and Web Serial.

### Connecting

The firmware exposes the console on two USB interfaces, and either one works:

- **Connect WebUSB** uses the vendor-class interface. The page filters for the
  device's USB id, `0x1209:0x0d32` by default. If the firmware's id changes,
  update `DEFAULT_VID` / `DEFAULT_PID` in the page to match `OMEGA_USB_VID` /
  `OMEGA_USB_PID` in [main/main.cpp](../../main/main.cpp), or tick "show all
  USB devices".
- **Connect Web Serial** uses the CDC serial port at 115200 baud. The firmware
  console log shares that port, so its output appears in the page's log pane.
  Use this connection when recalibrating, because the calibration prompts are
  printed there.

On connect the page asks the device which modules it has (discovery, module
`0xFF`) and then reads the current state.

### What you can change

| Section | Message | Notes |
| --- | --- | --- |
| Manual center offset (trim) | `0x51 SET_MANUAL_OFFSET` | added to the calibrated center, in the same raw units as Center X / Y. Must be finite |
| Manual deadzone overrides | `0x52 SET_DEADZONE_OVERRIDE` | an unchecked box clears the override back to the calibration-derived value. Values must be finite and >= 0 |
| Nudge-click tuning | `0x53 SET_NUDGE_SETTINGS` | enter threshold in (0, 1], exit threshold in [0, 1], time window in ms |
| Input mode | `0x54 SET_INPUT_MODE` | mouse or XAC gamepad. The device saves all live settings to flash and restarts to apply it. The page currently reports a timeout here even when the switch worked (see the XFAIL list in [tests/host/README.md](../../tests/host/README.md)) |
| Save current state to flash | `0x55 SAVE_TO_FLASH` | writes `omega_calibration.cfg` and `omega_settings.cfg` to the device's file system |
| Re-run calibration | `0x56 START_CALIBRATION` | asynchronous. The result arrives later as an unsolicited `0xD1` frame |

Changes apply live but are lost on reboot until you save them to flash (or
switch input mode, which saves first). The page re-reads the device state
after every change, so the form always shows what the device is actually
using.

### Protocol

The wire format (frame layout, every message and payload, and the 79-byte
STATE reply) is documented at the top of
[calibration_service.hpp](include/calibration_service.hpp). The page's own
header comment mirrors it.

### Firmware wiring

[main/main.cpp](../../main/main.cpp) creates one `CalibrationService` and one
`espp::DispatcherWorker` per USB byte stream (vendor and CDC). The USB receive
callbacks only queue bytes into the worker. Parsing, the reply, and the
discovery answer all run on the worker's own task. This matters because the
receive callbacks run on the TinyUSB task, where `UsbDevice::write_vendor()`
and `write_cdc()` fail immediately instead of waiting for buffer space, so a
reply sent from there can be dropped.

## Tests

Host unit tests for this component live in [tests/host](../../tests/host) and
run without ESP-IDF or a board.
