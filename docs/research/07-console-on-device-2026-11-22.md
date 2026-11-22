# 07: Carrying the console on the stick, 2026-11-22

Point-in-time snapshot. Captured 2026-11-22 against `main` at `95db1d2`
(firmware 0.2.0 unreleased, espp 1.3.8). Method: a read of the vendored TinyUSB
and espp sources, two web research passes, and one prototype branch that was
built but never flashed. Nothing here was tested on hardware. Claims that could
not be confirmed against a primary source are marked `UNVERIFIED`.

**Question asked:** Can the stick serve the calibration console itself, so the
console works with no internet and does not depend on the project website?

---

## Headline findings

1. **The stick has no spare USB endpoint.** The ESP32-S3 allows four data IN
   endpoints, and both modes already use all four. Nothing can be added beside
   the current functions.
2. **A fifth IN endpoint does not fail safely.** The whole device fails to
   configure, so the mouse or gamepad goes with it. espp's own budget check does
   not catch this.
3. **A drive is still possible, but only in place of the serial port or of
   WebUSB.** No comparable product was found that carries its whole
   configuration tool on a drive.
4. **Recommendation:** do not add a drive. Keep the offline copy of the hosted
   console (PR #121) and add two cheap durability steps: publish the console as
   a release asset, and give the page a "save a copy" button.

## Why a drive does not fit

### The endpoint limit

The ESP32-S3 USB-OTG core supports at most five IN endpoints active at once,
**including EP0 IN**. That leaves four data IN endpoints.

- Vendored TinyUSB, `src/portable/synopsys/dwc2/dwc2_esp32.h:51`: the S3
  controller is declared with `.ep_count = 7, .ep_in_count = 5`.
- `dcd_dwc2.c:227`: `dfifo_alloc()` asserts
  `allocated_epin_count < ep_in_count` for every IN endpoint, and
  `dfifo_device_init()` (`dcd_dwc2.c:268`) allocates EP0 IN through the same
  function, so EP0 counts as one of the five.
- A TinyUSB issue cites the ESP32-S3 technical reference manual for "maximum of
  five IN endpoints concurrently active, including EP0 IN":
  <https://github.com/hathach/tinyusb/issues/4450>. The manual itself was not
  read (UNVERIFIED at the primary source).
- CircuitPython documents the same limit for these chips ("only 4 IN endpoints
  active at once") and counts the CDC notification endpoint against it:
  <https://learn.adafruit.com/customizing-usb-devices-in-circuitpython/how-many-usb-devices-can-i-have>.

What the stick uses today, in both modes:

| Function | IN endpoints | IN endpoint numbers |
| --- | --- | --- |
| CDC serial (Web Serial transport, firmware log) | 2 (notification and data) | 1, 2 |
| Vendor (WebUSB transport) | 1 | 3 |
| HID (mouse, or the XAC gamepad) | 1 | 4 |
| **Total** | **4 of 4** | |

A mass storage function needs one more IN endpoint, which would be the fifth.

### What happens with a fifth

Read from the vendored source, not observed on hardware:

1. `dfifo_alloc()` fails its assertion, so `dcd_edpt_open()` returns false.
2. The class driver's `open` then claims no interface, no driver matches, and
   `process_set_config()` fails at `usbd.c:1296`.
3. The device stays unconfigured. Every function is lost, not only the new one.

The host symptom is probably "This device cannot start (Code 10)" on Windows
(UNVERIFIED).

`espp::UsbDevice::initialize()` succeeds in this case, because the failure
comes later, when the host configures the device. A fallback that retries
without the drive when `initialize()` fails therefore cannot catch it.

### A second trap: IN endpoint numbers above 4

On the S2 and S3, an IN endpoint numbered 5 or 6 opens without error and then
never sends data: <https://github.com/hathach/tinyusb/issues/4007> (measured on
an S3). Whether the pinned TinyUSB copy behaves the same is UNVERIFIED.

espp numbers endpoints in sequence in a fixed order (`usb_device.cpp:991` to
`1081`): CDC, vendor, HID, X-Input, mass storage. Today HID lands on 4, the
highest number that works. Any future layout must keep its IN numbers within
1 to 4.

### espp's budget is off by one

espp 1.3.8 allows five data IN endpoints besides EP0
(`kMaxInEndpoints = 5`, `usb_device.cpp:203`), and its README lists
CDC + vendor + HID + mass storage as "at the hard limit, not recommended".
Upstream `main` and the latest release (v1.4.6, 2026-11-22) still say the
same, and no upstream issue discusses it. On an S3 that combination passes
espp's check and then fails as described above.

## The prototype

Branch `feat/console-drive` (local only, commit `8be7882`, never pushed) adds a
read-only drive in mouse mode beside the existing functions:

- a `console` FAT partition appended to `partitions.csv`,
- the console page embedded in the app image (about 270 KB) and copied onto the
  partition at boot when it differs,
- `OMEGA_STICK_CONSOLE_DRIVE` to turn it off, and host tests for the file logic.

It builds in both configurations (app 1,203,888 bytes, 39% of the slot free)
and its host tests pass. **It must not be flashed or merged as it is**: it
creates exactly the five-endpoint layout above. The file-sync logic and the
partition design are reusable if a drive is ever built in place of another
function.

## Ways a drive could fit, and what each costs

| Option | IN endpoints | Cost |
| --- | --- | --- |
| Drive replaces the serial port | HID, vendor, drive: 3 | No Web Serial transport and no firmware log over USB while the drive is present. Reverses part of D14 |
| Drive replaces WebUSB | HID, serial, drive: 4 | No WebUSB transport and no browser prompt on plug-in. Reverses part of D14 |
| Serial port without its notification endpoint | 4 | Not viable: Linux `cdc-acm` rejects a control interface with no endpoint. Windows and macOS are UNVERIFIED. espp and esp_tinyusb always emit the endpoint |
| Console talks over the HID interface (WebHID feature reports) | 2 or 3 | A new transport in the firmware and the console. Frees the serial port's two endpoints. Chrome blocks mouse and keyboard collections, so it needs a separate vendor-defined collection; one report of `sendReport()` failing on such a device is unexplained (UNVERIFIED) |

Costs that apply to any drive:

- **Hosts that reject storage.** A device-level USB storage policy disables
  every function of a composite device, so the pointer is lost with the drive.
  Interface-level rules leave HID working; which kind a given organisation uses
  varies.
- **Simple hosts.** One report of a BIOS that stopped reading a keyboard once
  the device also offered mass storage:
  <https://forum.gl-inet.com/t/rm1pe-virtual-media-freezes-the-target-bios-keyboard-drive-never-boots-same-as-66265-logs-attached/71600.md>.
  Behaviour on the Xbox Adaptive Controller's USB ports, consoles, iPadOS and
  Android is UNVERIFIED.
- **One full flash.** An update over USB cannot change the partition table.
- **Windows descriptor caching** when an already-seen device gains an
  interface: hardware ids and the `usbflags` key include bcdDevice, so bumping
  it is the usual precaution (UNVERIFIED that it is needed).
- **Opening the page from the drive.** `file://` is a potentially trustworthy
  origin under the Secure Contexts specification, and this project's README
  already tells people to open the console from disk. Whether Chrome stores a
  WebUSB or Web Serial permission for a `file://` page between visits is
  UNVERIFIED.

## How comparable products do it

Coverage here was thin; treat it as indicative.

- **Keyboard configurators** (VIA, Vial, ZMK Studio) and gaming tools use a
  hosted web page over WebHID or Web Serial, or a desktop app. Vial keeps the
  keyboard's description in the firmware and sends it to the tool on connect.
- **Maker boards** that expose a drive (the UF2 bootloader's `INDEX.HTM`,
  micro:bit's `MICROBIT.HTM`) carry a small link to a website, not the tool
  itself (from background knowledge, UNVERIFIED by source).
- **CircuitPython's drive** holds the user's code, and lets `boot.py` hide the
  drive or make it read-only.
- No product was found that carries a whole configuration tool on its own
  drive, and none of the web configurators documented an offline mode.
- Nothing specific was found for assistive controllers or switch interfaces.

## Keeping the console available without a drive

| Approach | Survives no internet | Survives the website going away | Notes |
| --- | --- | --- | --- |
| Offline copy via service worker (PR #121) | Yes, after one visit | Until the browser data is cleared or evicted | Chrome's cache storage is best effort: <https://web.dev/articles/persistent-storage> |
| A copy the user saved to disk | Yes | Yes | The user has to know to save it |
| Console attached to each GitHub release | No | Yes, while the repository exists | One line in the release workflow |
| Page on the stick's own drive | Yes | Yes | Costs a USB function, see above |

A saved or cached page can be older than the firmware. The firmware
reports its protocol version on connect (D24); a visible "this page is older
than the firmware" note would cover that case.

## Recommendation

1. **Do not add a drive now.** The stick is at its endpoint limit, the failure
   mode takes the pointer with it, and the benefit is covered more cheaply.
2. **Merge the offline copy (PR #121)** as the convenience layer.
3. **Attach `calibration_console.html` to each release** and add a "Save a
   copy of this page" button to the console, so there is a durable copy that
   does not depend on the website or on browser storage.
4. **Report the endpoint budget to espp** (`kMaxInEndpoints`, the README table,
   and the endpoint numbering above 4).
5. **Record the limit** where the next person adding a USB function will see
   it: both modes use four of four IN endpoints.
6. If the project later wants the drive anyway, the least costly route is the
   drive in place of the serial port in mouse mode, as an option that is off by
   default. A WebHID transport would remove the trade, but it is a much larger
   change.

## Not verified

- Everything about behaviour on real hardware and real hosts, including the
  configure failure and the Windows symptom.
- The technical reference manual wording (taken from a TinyUSB issue).
- Whether the pinned TinyUSB has the "IN endpoint number above 4" fault.
- Serial ports without a notification endpoint on Windows, macOS and Web Serial.
- Permission persistence for WebUSB and Web Serial on `file://` pages.
- Read-only mass storage handling on Windows, macOS and Linux.
- Mass storage beside HID on the Xbox Adaptive Controller, consoles, iPadOS,
  Android and KVM switches.
