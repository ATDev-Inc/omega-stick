# omega_calibration

Calibration maths, persisted settings, and the USB console protocol for the
Omega Stick.

| Header | What it holds |
| --- | --- |
| `omega_calibration.hpp` | center and range calibration, deadzones, `key=value` file persistence |
| `omega_store.hpp` | the live calibration and settings behind one mutex, with change callbacks |
| `calibration_service.hpp` | the framed request/reply protocol (dispatcher module 5, protocol id `omega.calibration` v1) that the console talks to |
| `web/calibration_console.html` | the browser console, a single self-contained page |
| `web/omega_protocol.js` | the console's framing, CRC and payload code, DOM-free so Node can test it (`tests/web`). The page carries an identical inline copy; a test fails if the two differ |

## Saved file format

Both files are plain `key=value` text, one pair per line, and stay separate on
purpose (see D11 in [docs/decisions.md](../../docs/decisions.md)). Each starts
with `schema=1`, written on every save, which is
`OmegaCalibration::SCHEMA_VERSION`. A file without the key is schema 1, so every
file written before the key existed still loads. A file whose schema is higher
than the firmware knows fails to load with `Error::unsupported_schema`, and
`OmegaStore` then falls back as it does for a corrupt file: it runs
calibration, or uses the default settings. Adding a key under the same schema
does not change the number; only a change that old firmware cannot read does (a
MAJOR bump, see [docs/firmware-updates.md](../../docs/firmware-updates.md#firmware-versioning)).

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
  USB devices". Every stick reports its own USB serial number (its factory MAC),
  so an existing stick looks like a new device to the OS and browser once after
  this firmware, and Chrome asks for USB access again.
- **Connect Web Serial** uses the CDC serial port at 115200 baud. The firmware
  console log shares that port, so its output appears in the page's log pane.
  Use this connection when recalibrating, because the calibration prompts are
  printed there.

On connect the page asks the device which modules it has (discovery, module
`0xFF`), picks the one that advertises the protocol id `omega.calibration` and
uses the module id discovery reports for all its traffic, then reads the
current state and firmware info. A device that only speaks discovery v1 (no
protocol field) is assumed to serve the calibration protocol on module 5. If the
device advertises a different protocol version the page warns in its log.

### What you can change

| Section | Message | Notes |
| --- | --- | --- |
| Manual center offset (trim) | `0x51 SET_MANUAL_OFFSET` | added to the calibrated center, in the same raw units as Center X / Y. Must be finite |
| Manual deadzone overrides | `0x52 SET_DEADZONE_OVERRIDE` | an unchecked box clears the override back to the calibration-derived value. A set value must be finite and within 0 to 0.5 (center, `OmegaCalibration::MAX_CENTER_DEADZONE`) or 0 to 0.1 (range, `MAX_RANGE_DEADZONE`); the device replies ERROR otherwise, the page checks the same limits before sending, and `OmegaStore` ignores an out-of-range saved override when it loads (logged at boot) |
| Nudge-click tuning | `0x53 SET_NUDGE_SETTINGS` | enter threshold in (0, 1], exit threshold in [0, 1], time window in ms |
| Nudge clicks on/off | `0x60 SET_NUDGE_ENABLED` | the "Nudge clicks" checkbox in the nudge-click block. Off means the stick only moves the cursor: no nudge starts, and a held one is released at once. The tuning values are kept, and saved as `nudge_enabled=0/1` in `omega_settings.cfg` (off on a fresh board; a file from before the switch, without the key, loads as on). XAC mode has no nudge clicks, so it is unaffected. Firmware without this message does not answer it, and its 79-byte STATE reply is read as on |
| Input mode | `0x54 SET_INPUT_MODE` | mouse or XAC gamepad. The device saves all live settings to flash and restarts to apply it. The OK reply is sent before the restart, so the page then loses the connection; reconnect after the stick re-enumerates |
| Save current state to flash | `0x55 SAVE_TO_FLASH` | writes `omega_calibration.cfg` and `omega_settings.cfg` to the device's file system |
| Re-run calibration | `0x56 START_CALIBRATION` | asynchronous. The result arrives later as an unsolicited `0xD1` frame. The device stays busy until the sweep finishes and refuses a second request (`device_or_resource_busy`); the page keeps the button disabled until the result arrives |

Changes apply live but are lost on reboot until you save them to flash (or
switch input mode, which saves first). The page re-reads the device state
after every change, so the form always shows what the device is actually
using.

Firmware update, revert, and diagnostics also go through this protocol; see
[docs/firmware-updates.md](../../docs/firmware-updates.md).

### Protocol

The service advertises the protocol id `omega.calibration`, version 1
(`CalibrationService::kProtocol` / `kProtocolVersion`), which is what the
console looks for; module 5 is the default routing id only (decision D24 in
[docs/decisions.md](../../docs/decisions.md)). Change the id only for an
incompatible wire format.

The wire format (frame layout, every message and payload, and the 80-byte
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
