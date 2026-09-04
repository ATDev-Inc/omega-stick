# Omega Stick v1: requirements

Supporting document to [PRD.md](PRD.md). This is the detailed specification;
the PRD is the one-page summary.

Priority: **[M]** must-have for v1, **[S]** should-have, **[C]** could-have.

Evidence behind these requirements is in [docs/research/](research/).

---

## 1. Sensing and signal path

- **SEN-1 [M]** Sample the TMAG5273 X and Y channels continuously at **≥1 kHz**
  from a dedicated pinned FreeRTOS task.
- **SEN-2 [M]** Use the ±40 mT range (x1 part) with **CONV_AVG = 4x**
  (≈3.1 kSPS 3-axis), which gives real noise reduction while staying ~3x faster
  than the sample requirement.
- **SEN-3 [S]** Add an XY-only 4-byte burst read. The driver currently bursts 6
  bytes from `X_MSB_RESULT`; XY-only is ≈158 µs against ≈202 µs at 400 kHz.
- **SEN-4 [S]** Use the sensor's `INT` pin as data-ready if the board exposes it,
  rather than polling on a timer.
- **SEN-5 [M]** Detect sensor loss (I2C failure, or conversion-ready stuck)
  within **500 ms** and surface it per STA-2.
- **SEN-6 [M]** On sensor loss, hold all axes at center rather than transmitting
  the last value or garbage. A stuck-full-deflection axis is an actively harmful
  failure for someone driving a pointer.

## 2. Calibration and centering

- **CAL-1 [M]** Capture a center reference at boot from the resting position.
- **CAL-2 [M]** Provide a guided calibration routine invoked **on the device
  itself**, requiring no host software and **no dedicated control**: capture
  center, then capture the user's reachable extents per axis. Invoked by a stick
  gesture (for example, hold full deflection in one corner for 3 seconds), so it
  costs nothing in the BOM and works with no computer attached. Also invocable
  from the configuration page (CFG-1).
- **CAL-3 [M]** Calibration must be completable by a user who cannot see a
  console. Signal each stage through STA-1.
- **CAL-4 [M]** Map the user's *actual* reachable range to full output range,
  per axis and independently. A user who can reach 30% travel left and 80% right
  must still get full output in both directions.
- **CAL-5 [M]** Slow idle drift correction: when input sits within the deadzone
  for a configurable dwell, migrate the center reference at a rate slow enough
  that it cannot fight deliberate sustained input.
- **CAL-6 [M]** Persist calibration to NVS and restore on boot (see PER-1).
- **CAL-7 [S]** Provide a one-action reset to factory defaults that does not
  require a host.

> Runtime re-centering is mandatory because the TMAG5273 has **5% typical
> sensitivity drift** across temperature, and its on-chip offset registers are a
> static trim rather than drift tracking.

## 3. Response shaping

- **RSP-1 [M]** Configurable center deadzone, radial, default small but nonzero.
- **RSP-2 [M]** Configurable outer (range) deadzone, so full output is reached
  before mechanical end-stop.
- **RSP-3 [M]** Selectable response curve: linear (default) and exponential.
  `espp::FloatRangeMapper` is linear only; `bezier.hpp` is the building block for
  the curve.
- **RSP-4 [M]** Per-axis inversion and axis swap, for users mounting the device
  at an arbitrary orientation.
- **RSP-5 [S]** Per-axis gain scaling. Gain adjustment is one of the few tuning
  parameters with a measured, statistically significant benefit for this
  population (p<0.05, n=16).
- **RSP-6 [S]** Single-axis lock mode, for users who cannot isolate one axis from
  the other.
- **RSP-7 [C]** Selectable tremor filter, **default off**, user-defeatable, with
  the ability to A/B it without a host.

> Tremor filtering defaults off because the one controlled trial of adaptive
> (WFLC) filtering found **no performance improvement** and possible damping of
> intended motion. Fixed low-pass is worse: MS tremor at 3.5 to 5.0 Hz overlaps
> intentional movement.

## 4. Host output

- **OUT-1 [M]** Enumerate as a USB HID device requiring no driver installation on
  Windows 10/11, macOS, Linux, ChromeOS, and Android.
- **OUT-2 [M]** Gamepad report: X/Y (left stick), Z/RZ (right stick), 4-bit hat,
  and buttons.
- **OUT-3 [M]** Mouse report: relative X/Y motion plus at least left and right
  buttons.
- **OUT-4 [M]** Mode selection between gamepad and mouse is made **from the host**
  via the configuration surface (CFG-1), not from a control on the device. The
  selection **persists to NVS** and is restored on boot, so the device keeps its
  mode when later connected to a host that cannot configure it (XAC, console,
  tablet). Switching must not cause re-enumeration.
- **OUT-5 [M]** Set HID `bInterval` to **1 ms**. `espp::UsbDevice`'s
  `HidFunction` defaults `poll_interval_ms` to 10, which is only 100 Hz. **This
  default must be overridden explicitly.**
- **OUT-6 [M]** Send reports on change, decimated from the ≥1 kHz sample task.
- **OUT-7 [M]** Keep HID reports **at or under 64 bytes**. There is a known
  ESP-IDF endpoint-stall defect with consecutive reports above 64 bytes.
- **OUT-8 [M]** Expose the interface the configuration page needs (WebHID feature
  reports, or a WebUSB vendor interface) in the same TinyUSB composite. **Raised
  from [S] to [M] because CFG-1 now depends on it.** Also expose a CDC-ACM
  interface for logging and a fallback configuration CLI, for developers and for
  users whose browser cannot reach the config page.
- **OUT-9 [S]** Verify against the XAC's documented external-joystick mapping
  (X/Y to left stick, Z/RZ to right stick, hat to d-pad), which requires **XAC
  firmware June 2024 or newer**. Default button mapping for external USB joystick
  buttons is reported as unexpected and needs bench confirmation.

## 5. Configuration

- **CFG-1 [M]** Configuration is performed from a **browser-based configuration
  page requiring no installed software** (WebHID or WebUSB). Every parameter in
  sections 2 and 3 marked [M], plus output mode (OUT-4), is settable there.
- **CFG-2 [M]** Every setting **persists to NVS** (PER-1) and survives replug,
  power loss, and a change of host. A user configures once on a computer and the
  device keeps that configuration on an XAC, console, or tablet.
- **CFG-3 [M]** The device is fully functional at first plug-in with factory
  defaults. Configuration is never a prerequisite.
- **CFG-4 [M]** The configuration page must be usable by someone operating the
  computer with the Omega Stick itself, or with whatever input device they
  already use. Large targets, keyboard-reachable, no drag-only interactions, no
  timed interactions. It is an accessibility product: its own settings UI is the
  first thing a user touches.
- **CFG-5 [S]** Support at least two stored profiles (for example a gaming
  profile and a pointer profile).

> **Design principle 2 ("no required host software") is satisfied in spirit but
> narrowed.** A browser page installs nothing, but it does make the *changing* of
> settings host-dependent. CFG-2 is what keeps the device itself
> host-independent: the configuration travels with the device, not the host.
> **CAL-2 deliberately stays on-device**, because recalibration is the one thing
> a user may genuinely need in the field with no computer present.

## 6. Status and failure visibility

- **STA-1 [M]** A non-console status channel (LED or equivalent) that
  distinguishes at minimum: healthy, calibrating, calibration stage advanced,
  sensor fault, and configuration saved.
- **STA-2 [M]** Sensor faults, calibration faults, and NVS faults are each
  distinguishable through STA-1, not merged into one generic error.
- **STA-3 [S]** Status must be perceivable without relying solely on color, since
  the user population overlaps with color vision deficiency.

## 7. Persistence

- **PER-1 [M]** Calibration, configuration, and **selected output mode** persist
  in NVS across power cycles, host changes, and firmware updates within a major
  version. This is what makes host-side configuration (CFG-1) acceptable on hosts
  that cannot configure.
- **PER-2 [M]** Corrupt or absent stored configuration falls back to defaults and
  reports through STA-2, rather than failing to boot.
- **PER-3 [S]** Store a schema version so future firmware can migrate or safely
  reject old records.

## 8. Non-functional

- **NFR-1 [M]** End-to-end latency, stick motion to HID report on the wire,
  **≤5 ms** at the 95th percentile. The computed budget is ~2 to 3 ms at a 1 ms
  bInterval; the sensor and I2C path together are under 0.5 ms and are never the
  bottleneck.
- **NFR-2 [M]** Sustained operation for **≥8 hours** with no drift requiring
  manual recalibration.
- **NFR-3 [M]** No dynamic allocation in the sample or report path.
- **NFR-4 [S]** Set `CONFIG_FREERTOS_HZ=1000` and 240 MHz CPU in
  `sdkconfig.defaults`. Both are currently commented out.
- **NFR-5 [S]** Sustained 1 kHz HID reporting on ESP32-S3 + TinyUSB is
  **unverified**; no published benchmark was found. Bench-measure early and
  revise NFR-1 if it proves unattainable.

---

## 9. Hardware interface requirements

**Not owned by this repository.** Stated because the product claims depend on
them. See the scope note in the [PRD](PRD.md).

- **HW-1 [M]** Actuation force **adjustable across approximately 5 to 25 gf**,
  with the shipped default **at or below 10 gf**. Pending the design-principle
  decision in the PRD.
- **HW-2 [M]** A **documented, repeatable force and throw measurement protocol**,
  published with results. ISO 9241-410 (ergonomic design of physical input
  devices, explicitly covering joysticks) is the natural framework to cite.
  No competitor publishes a method; several disagree with third-party
  measurements of their own products.
- **HW-2a [M]** The protocol must be **performable by the person who printed the
  device, using household items** (for example a set of known small masses or a
  kitchen scale), not laboratory equipment. Because the enclosure is 3D printed
  by third parties, this is not documentation, it is the **only quality control
  the design has**. See HW-9.
- **HW-3 [M]** Mounting: **1/4"-20 UNC insert** plus the **AMPS 4-hole pattern
  (30 mm x 38 mm)**. That combination reaches RAM Mounts (1" B ball is the
  practical AT size), the Rehadapt Universal Device Socket (and through it
  VESA 75/100, Daessy, Armon Edero, LC-Tech), and matches the Xbox Adaptive
  Joystick convention. Negligible BOM cost, large ecosystem reach.
- **HW-4 [S]** Compatibility with the special-controls ecosystem's 6 mm rod and
  half-clamp hardware.
- **HW-5 [M]** USB bus-powered, **no battery**. A cell would trigger UN 38.3
  transport and IEC 62133-2 product-safety obligations for no v1 benefit.
- **HW-6 [M]** Expose the TMAG5273 `INT` pin to a GPIO (enables SEN-4).
- **HW-7 [S]** Magnet and sensor geometry chosen so that full mechanical throw
  maps to a large fraction of the ±40 mT range, maximizing usable resolution of
  the 12-bit ADC (one LSB ≈ 19.5 µT at that range).
- **HW-8 [M]** User-replaceable wear parts, with spares and drawings published.
  Since the body is printed, a worn part should be **reprintable rather than
  ordered**. Repairability is a direct countermeasure to the documented
  abandonment predictors.
- **HW-9 [M]** **Design for print-tolerance variance.** At a 5 to 10 gf budget,
  friction in the return mechanism is most of the specification, and print
  tolerance, layer adhesion, and surface finish vary between machines, materials,
  and successive prints on one machine. A few grams of unintended friction
  consumes the entire budget. Required:
  - Publish tested filament types, layer heights, orientations, and tolerances,
    and state which are known not to work.
  - Prefer **non-printed parts for the friction-critical path** (bearings,
    bushings, pivots, and the return element itself) sourced as standard
    off-the-shelf components, so the printed part positions them rather than
    being the tribological surface.
  - Provide a **printed test coupon or go/no-go gauge** so a builder can check
    their printer before committing to a full build.
  - Publish the acceptance criterion the HW-2a measurement has to meet.
- **HW-10 [S]** Magnet-to-sensor geometry should be **tolerant of the stack-up
  variance a printed assembly introduces**. Firmware calibration (CAL-4) already
  maps each user's actual reachable range, which absorbs a useful amount of
  per-unit geometric variation for free; the mechanical design should lean on
  that rather than demand tight tolerances it cannot enforce.

---

## 10. Host compatibility matrix

| Host | Gamepad mode | Mouse mode | Notes |
| --- | --- | --- | --- |
| Windows 10/11 | Yes (DirectInput / `RawGameController`, appears in `joy.cpl`) | Yes | **Not XInput.** Many modern titles are XInput-only and will not see it. Steam Input is the documented workaround |
| macOS | Yes (IOKit HID) | Yes | **Not `GCController`.** Apple's GameController.framework gates on MFi/Xbox/DualSense PnP IDs; generic pads produce no controller object. Open Apple bug on file |
| Linux | Yes (evdev/joydev) | Yes | Essentially unclaimed by competitors |
| ChromeOS | Yes | Yes | |
| Android | Yes (`SOURCE_GAMEPAD`) | Yes | |
| **iOS / iPadOS** | **No** | **Yes** | Mouse via AssistiveTouch pointer is fully supported and is **the only working mode**. Reports suggest exotic composite descriptors can make iOS ignore a device entirely: bench-verify early |
| Xbox (via XAC USB-A) | Yes | n/a | Requires XAC firmware June 2024+. No licensing needed |
| Xbox (direct) | **No** | **No** | Blocked, error `0x82d60002` |
| PlayStation 4/5 (direct) | **No** | **No** | Requires cryptographic auth |
| Nintendo Switch (direct) | **No** | **No** | Requires proprietary handshake |

Full analysis: [research/03](research/03-hid-platform-compatibility.md).

---

## 11. Known defects to fix first

Found while surveying the existing driver. Both block milestone M-A.

### DEF-1: magnetic range conversion is wrong for every part variant

[`components/tmag5273/include/tmag5273.hpp:770`](../components/tmag5273/include/tmag5273.hpp#L770)
returns `266.0f` unconditionally, and
[line 754](../components/tmag5273/include/tmag5273.hpp#L754) consumes it as the
**LOW** full-scale, doubling it for HIGH. That yields LOW = 266 mT / HIGH = 532
mT, which matches neither variant:

| Part | LOW | HIGH |
| --- | --- | --- |
| A1 | ±40 mT | ±80 mT |
| A2 | ±133 mT | ±266 mT |
| **Code produces** | **266 mT** | **532 mT** |

Reported values are therefore **~6.65x high on an A1** and **~2x high on an A2**.
The in-code comment acknowledges the hardcoding.

### DEF-2: `range_z_mT()` is dead code

[Line 773](../components/tmag5273/include/tmag5273.hpp#L773) is never called.
`convert_raw_to_mT` always reaches for the XY constant even when callers
correctly pass `z_range_`. Numerically harmless only because both constants are
currently the same literal, which stops being true the moment DEF-1 is fixed in
isolation.

> Because joystick math runs in **raw counts**, neither defect can affect how the
> device feels. Both corrupt any published measurement and any bench validation,
> which matters because HW-2 makes a published force protocol part of the
> product's positioning.

### Also unused, worth wiring

- `INT_CONFIG_1`: enables SEN-4 (data-ready interrupt)
- `MAG_OFFSET_CONFIG_1/2`: on-chip static trim, distinct from runtime re-centering
- `MAG_GAIN_CONFIG`
