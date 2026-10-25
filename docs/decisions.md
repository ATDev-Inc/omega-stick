# Omega Stick v1: product decisions

Supporting document to [PRD.md](PRD.md). Each decision records **why**, so it is
not relitigated per pull request. Evidence is in [docs/research/](research/).

---

## D1: USB wired only in v1. No BLE.

The ESP32-S3 has **BLE only, no Bluetooth Classic**
([ESP-IDF docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/ble/overview.html)),
so Bluetooth Classic HID is impossible regardless of preference.

BLE would trigger FCC Part 15 Subpart C and EU RED obligations (RED applies to
the whole product and subsumes EMC/LVD; module pre-certification does not remove
device-level assessment). And **iOS and macOS reject generic BLE HID gamepads**
the same way they reject generic USB ones.

> BLE costs real compliance work and buys no platform. Deferred to a separate
> compliance-gated milestone.

## D2: Composite HID, gamepad and mouse on one interface, two report IDs

Mouse is the **only mode iPadOS accepts** and the universal fallback. Gamepad is
what games need. Mode switching changes which report is sent, **never the
descriptor**, so the host never re-enumerates.

The AT field precedent is Makers Making Change's OpenAT joysticks, which ship
"USB HID Mouse or USB HID Gamepad" with a **physical mode-cycle switch**. Omega
Stick differs here: see D11.

**Current firmware (2026-10-10):** the code differs from this decision. Switching
input mode saves settings and restarts the device (`esp_restart()`), so the host
re-enumerates. Each mode uses a single-mode descriptor: mouse-only (report ID 0)
or gamepad-only (report ID 1). There is no composite interface carrying both.
The gap is tracked in [status.md](status.md#gaps-against-requirements-and-decisions).

**Risk:** Apple forum reports suggest exotic or composite descriptors can make
iOS ignore a device entirely. Bench-verify on iPadOS early, since that is the
platform where mouse mode is the only option.

## D11: Mode and configuration are set from the host, then persisted

**Decision:** there is **no physical mode control**. Output mode, deadzone,
response curve, gain, and axis inversion are all set from a browser-based
configuration page that installs nothing, and **every setting persists to
on-device flash**. The current firmware stores calibration and settings as
`key=value` files on the LittleFS partition (`omega_calibration.cfg`,
`omega_settings.cfg`), not in NVS.

**Why persistence carries the design.** The device is configured once on a
computer and then keeps that configuration everywhere, including on an XAC, a
console, or a tablet where no configuration is possible. The configuration
travels with the device rather than with the host, which is what keeps design
principle 2 substantially intact even though the *act* of configuring is now
host-dependent.

**What this costs:**

- A user whose only computer cannot reach the config page cannot change mode.
  They get whatever the device was last set to, or the factory default.
- Browser support for the underlying APIs is **not universal**. This is being
  verified; see the open item below.

**What preserves the escape hatch:** calibration stays on-device (CAL-2),
invoked by a **stick gesture** rather than a dedicated control, so it costs
nothing in the BOM and works with no computer present. Recalibration is the one
adjustment a user may genuinely need in the field, for example after remounting.

**Alternative rejected:** an installed configuration app. That violates design
principle 2 outright, and on locked-down school and work machines (an explicitly
targeted environment) the user often cannot install anything.

> **Open item.** If browser support turns out to be narrow, the mitigation is not
> to add an app but to make the factory defaults genuinely good, keep the CDC
> fallback CLI, and consider whether a stick-gesture mode toggle should be added
> back at zero BOM cost, since buttons already exist for mouse click (OUT-3).

## D3: Descriptor shape is X/Y, Z/RZ, 4-bit hat, N buttons

This is the mapping the XAC expects, so the stick lands on the left thumbstick
with no licensing. `espp::GamepadInputReport` provides exactly this layout
already (X/Y left stick, Z/RZ right stick, 2 triggers, 4-bit hat, N buttons).

Caveat: `get_descriptor()` returns a fragment the caller must wrap in
`usage_page<generic_desktop>` / `usage(GAMEPAD)` / `collection::application`.

**Current firmware (2026-10-10):** the XAC report is the `espp` hid-rp
`GamepadInputReport` with 12 buttons, 8-bit axes (0 to 255; full deflection
reaches 0 and 254, since hid-rp maps [-1, 1] to center 127 +/- 127), a hat, and
triggers held at 0. It is carried in a gamepad-only descriptor, not alongside a
mouse (see D2). `set_joystick` writes the stick X/Y onto all four axes (X, Y, Z,
RZ) because the XAC's port-to-axis mapping is not yet confirmed. The gap is
tracked in [status.md](status.md#gaps-against-requirements-and-decisions).

## D4: Console support routes through the XAC USB-A port, not direct

Direct connection is closed on all three consoles:

- **Xbox** blocks unlicensed accessories with error `0x82d60002` (since November
  2023), requiring "Designed for Xbox" licensing and an auth IC.
- **PS5** requires DualSense cryptographic auth; PS4 mode needs uploaded
  encryption keys.
- **Switch** requires a proprietary handshake, not generic USB HID.

Microsoft confirmed the accessory block has **"no impact to players using the
Xbox Adaptive Controller regardless of the peripherals plugged into its USB and
3.5mm ports."** The XAC is the legitimate exemption, and the console only ever
sees the licensed XAC.

The `dinput_pluggable` project documents the working mapping and requires **XAC
firmware June 2024 or newer**.

## D5: Steam Input is the documented answer to XInput-only games

Windows never exposes generic HID via XInput; that is reserved for Microsoft's
XUSB descriptor set. Many modern titles are XInput-only and will not see the
stick at all.

Steam Input reads generic HID and injects a virtual Xbox pad into any
Steam-launched game. ViGEmBus is archived (November 2023). True Xbox emulation
requires the proprietary descriptor plus auth silicon and is **not a legitimate
path**.

## D6: All joystick math in raw sensor counts, not millitesla

Physical-unit conversion is a display concern. Working in raw counts also
isolates the feel of the device from the conversion defect recorded as DEF-1 in
[requirements.md](requirements.md).

## D7: Runtime re-centering is mandatory, not optional

The TMAG5273 has **5% typical sensitivity drift** across temperature, and its
on-chip offset registers are a static user trim rather than drift tracking.
Boot capture plus slow idle drift correction.

## D8: Tremor filtering ships available and defaulted off

This is a **documented negative result**, not an absence of evidence.

- The one controlled trial of adaptive (WFLC) filtering **found no performance
  improvement** and possible damping of intended motion (Dicianno et al., JRRD
  2009).
- Fixed low-pass is worse: **MS tremor runs 3.5 to 5.0 Hz**, overlapping
  intentional movement, so a cutoff low enough to remove it introduces phase lag
  and deletes real commands.

Ship it, default it off, make it defeatable, let users A/B it.

## D9: Both displacement and isometric-style response modes, selectable

The literature is genuinely split:

| Study | Finding |
| --- | --- |
| Cooper et al., IEEE TBME 2000 | Isometric gave **fewer movement errors but longer reaction times** |
| Dicianno et al., JRRD 2009 | Isometric **preferred by participants with tremor** |
| Stewart, Aust Occup Ther J 1992 | Displacement **superior for 4 of 5 CP subjects with prior displacement experience** |

Configurability is the evidence-backed answer. Committing to one architecture is
not.

## D10: Free PID under Espressif's VID `0x303A`

USB-IF charges **$6,000** for a VID and forbids sharing it. Espressif allocates
PIDs by pull request for projects using an Espressif chip with a USB interface,
at no cost and with no endorsement implied. pid.codes is the fallback.

**Current firmware (2026-10-10):** the firmware uses the pid.codes VID `0x1209`
with PID `0x0d32`, not an Espressif PID. `main/main.cpp` notes that both should
be replaced with an allocated PID before shipping. The gap is tracked in [status.md](status.md#gaps-against-requirements-and-decisions).

---

## D12: Actuation force is adjustable, defaulting below 10 gf

The device must reach **under 10 gf** and must let the user select a higher
force. Target range **5 to 25 gf**, defaulting below 10.

**Why.** Two independent vendors at the low-force frontier report that too-light
is a real failure mode:

- Celtic Magic states that *"an extremely light force stick can be just too light
  and in practice be harder to use"*, and ships a 4-step 5/10/16/20 gf magnet
  adjustment for exactly this reason.
- mo-vis warns its 5 gf adapted Micro requires *"special attention to mounting,
  shielding and testing."*

Minimum force is therefore the wrong thing to optimise on its own. The usable
range matters more, and which point in it works is a property of the individual
user, not of the device.

**Consequence.** Design principle 1 in [CONTRIBUTING.md](../CONTRIBUTING.md) was
amended. As originally worded it forbade adding mechanical resistance to the
input path, which would have ruled out the adjustment mechanism this decision
depends on. It now constrains the *minimum* achievable force instead.

**Consequence.** The README no longer presents "under 10 grams" as the
differentiator on its own, which matches section 5 of [PRD.md](PRD.md).

## D13: Mouse clicks and scroll come from stick nudge gestures

*Recorded from the implementation on 2026-10-10.*

The stick has no buttons in mouse mode, so clicks and scroll are produced by
"nudge" gestures: a short push past an enter threshold in one cardinal direction,
held for a time window, released when the stick falls below an exit threshold.
In `main/main.cpp`, a forward nudge holds mouse button 1 (primary click), a right
nudge holds button 2 (secondary click), and left and back nudges scroll. The enter threshold, exit threshold,
and time window are tunable from the calibration console and persisted with the
other settings (D11). This is the zero-BOM approach D11 anticipates for mouse
click (OUT-3).

## D14: The configuration page is one static HTML file over WebUSB and Web Serial

*Recorded from the implementation on 2026-10-10.*

The calibration console is a single static HTML file served from
`components/omega_calibration/web`, with no install step. It speaks one framed
protocol over two transports: WebUSB (the vendor interface) and Web Serial (CDC).
It needs a Chromium-based browser, which is the concrete form of the
browser-support risk in D11. Release builds also publish a copy of the page on
GitHub Pages (see [firmware-updates.md](firmware-updates.md#releases-and-the-pages-site)),
which is the same file, not a fork.

## D15: The boot flash window is a developer-only Kconfig option, default 0

*Recorded from the implementation on 2026-10-10.*

`OMEGA_STICK_BOOT_FLASH_WINDOW_S` (range 0 to 60) delays starting USB at boot so
`idf.py flash` can reset the board into the bootloader without holding BOOT.
Developers set it locally in menuconfig (`sdkconfig` is gitignored). The default
is 0 so that anything users run starts responding immediately.

## D16: Control-loop logic lives in a pure header; `main.cpp` is an adapter

*Recorded from the implementation on 2026-10-10.*

Everything in the polling loops that can be reasoned about without hardware
(nudge-click detection, scroll pacing, drift bookkeeping, the recalibration
hand-off) is in `components/omega_control/include/omega_control.hpp`, which
depends only on the standard library and `omega_calibration.hpp`. Time is an
input, and side effects (HID writes, the calibration sweep, flash saves) go
through a small `Ports` interface that `main.cpp` implements. The host tests
drive the same code with a fake `Ports` and a fake clock. New loop behaviour
goes in the header with a test, not in `main.cpp`.

One consequence is deliberate: a neutral HID report is sent before and after
every recalibration, so a nudge-held button cannot stay down for the whole
sweep. The hand-off is a `RecalibrationGate` in the same header: a request
keeps it busy until the sweep finishes, so a second request is refused
(`device_or_resource_busy`) instead of queueing another full sweep.

## D17: Host tests run the real espp code, and the console is checked against firmware-generated frames

*Recorded from the implementation on 2026-10-10.*

The host suite compiles the project's headers against the real espp
components, downloaded from the component registry pinned to
`dependencies.lock` and verified by hash, rather than hand-written stand-ins.
The browser console's protocol code is a separate script (`omega_protocol.js`)
kept inline in the single-file page (D14) by a test that fails if the two
copies differ. The C++ service generates `tests/protocol/golden_frames.json`;
the Node tests decode those frames, so the two sides of the protocol cannot
drift apart unnoticed. A libFuzzer harness covers the frame parser and
service. Known bugs are kept as expected-failure tests until fixed.

## D18: Two-slot OTA with automatic health-check rollback

Firmware is updated over USB from the calibration console into one of two OTA
slots (`ota_0` / `ota_1`), with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` set. A new
image is confirmed by the console once the checks in D22 rule 2 pass. Otherwise
it is rolled back.

The alternative was a manual-only confirmation step. It was rejected because a
reset before confirmation silently reverts to the old image, which hid updates in
the first attempt: the user saw the old version and had no way to tell why. For a
device someone relies on to operate their computer (design principle 4), a bad
image has to undo itself, and a good one must not depend on the user remembering
a second step. The console also shows the previous slot's version and state and
offers "Revert to previous firmware", so the user can step back deliberately.

**Consequence.** The first move to the new partition table needs one serial flash
and loses saved calibration once. See [firmware-updates.md](firmware-updates.md).

**Amended 2026-10-10: crash loops on a confirmed image revert too.** The
bootloader's rollback only covers a pending image. Once an image has confirmed
itself, a bug that panics on every boot (a bad calibration file it cannot parse,
a sensor-init path it never took in the first ten seconds) would leave the user
with a device that reboots forever. So `FirmwareHealthCheck::revert_if_crash_looping()`
runs at boot with this slot's recent resets from the reset log: three crashes in
a row, and an app image in the other slot, switch the boot partition and
restart. Brownouts are not crashes for this purpose. The threshold is three so a
single panic, already captured as a core dump, never changes firmware by itself.
Each reset-log entry names the slot its boot ran in, and the reset that starts
a boot is charged to the previous boot's slot, so a crash in the other image
(which the bootloader hands back to this one) is never counted against this
one and the two slots cannot bounce the device between them. An other slot the
bootloader already rejected is not retried automatically; the console's REVERT
remains the deliberate way to try it again.

*Note 2026-10-25:* the image the handover boots is pending, and with no console
attached the deadline used to reject it and send the device back into the crash
loop for good, so the boot the handover starts now confirms it at once (a marker
in RTC memory, `FirmwareHealthCheck::accept_after_handover()`), and at most one
handover happens per power-on. See [firmware-updates.md](firmware-updates.md#rollback-and-the-health-check).

**Amended 2026-10-12: the console confirms the new image, not the firmware.**
The original design had the image confirm itself after USB enumerated, the
sensor answered and 10 s had passed. It now never confirms itself. After the
reboot the console reconnects, checks the running version and SHA, a GET_STATE
round trip and GET_DIAGNOSTICS, and sends MARK_VALID (rules in D22 rule 2). The
manual-only objection above (a reset before confirming silently reverts, and the
user is not told) is answered by the console confirming within seconds of
reconnecting and reporting the outcome, including which check failed, and by the
deadline: the firmware still marks an image that is pending at
`ROLLBACK_DEADLINE` (now 120 s, was 90 s) invalid and reboots into the previous
slot, so a bad image does not stay. `HEALTHY_UPTIME` is gone. `reject_if_pending`
on a fatal init failure and the crash-loop revert are unchanged.

Why: William Emfinger (author of espp), who did it this way on the Backbone
controller, pointed out two things. Confirming from the host proves the whole
remote path still works after the update (vendor enumeration, the protocol, OTA
itself), so a bad image can never lock the user out of the next update. And the
health checks are defined outside the firmware, so they can be added to or
changed without shipping new firmware just to change what counts as healthy.
We keep a firmware deadline, which Backbone did not, because some users depend
on the stick to operate their computer and may not be able to unplug or
power-cycle it themselves; a bad image that broke the console link must not
stay running until someone else does that. The deadline is generous so the
console's confirm always lands first in normal use.

**Consequence.** The console is now the only thing standing between an update
and a rollback. A new firmware paired with an old console that still waits for
self-confirmation would never be confirmed and would roll back at the deadline,
so the console change ships first (it is harmless against self-confirming
firmware, whose health check tolerates an external confirm). No protocol change:
MARK_VALID (0x5D) already existed and `kProtocolVersion` stays 1.

## D19: `version.txt` plus a commit suffix, not `git describe` strings

`version.txt` holds `MAJOR.MINOR.PATCH`, bumped by hand in the PR that warrants
it. Builds embed that number alone for a release (clean tree at tag `vX.Y.Z`),
and `X.Y.Z+<7-char sha>` otherwise, with `.dirty` appended for uncommitted
changes.

`git describe` output is traceable but not something a person quotes in a bug
report or reads off the console. A plain number is, and the exact commit is still
recoverable from the suffix. CI refuses to publish a release whose tag does not
match `version.txt`, so a release number cannot drift from the tag.

## D20: Adopt espp 1.3.6 across the board

Adding `espp/ota` to the manifest makes the component manager re-solve every
dependency, so the lock could not stay at the old mix of 1.3.3 to 1.3.5. A fresh
solve picked espp 1.3.7 together with tinyusb 2.4.0; the `ota-implementation`
branch's lock, with every espp component at 1.3.6 and tinyusb unchanged, was the
smaller jump and had already run on hardware, so that is the one adopted. The
1.3.6 dispatcher adds `ModuleInfo::protocol` and `protocol_version`; see D24.
The host tests pin the same release (see [tests/host](../tests/host/README.md));
the XAC report-layout tests give identical results on hid-rp 1.3.3 and 1.3.6.

Moved to 1.3.8 on 2026-10-18, one release across the board as before. Between
1.3.6 and 1.3.8 the components this firmware uses changed only in `hid-rp`
(the bundled hid-rp headers gained their missing `<bit>` and `<cstdint>`
includes, so the host test no longer has to include `<bit>` first) and in
comments; `dispatcher.hpp` is byte-identical. tinyusb and esp_tinyusb stay
pinned at the same versions.

## D21: A sensor fault centers the stick, after three failures, not one

When the TMAG5273 stops answering, the firmware reports a centered stick with
no buttons held until the sensor is back. The HID report keeps going out every
tick, so the host sees a live but idle device rather than a frozen one.

**Why.** Before this, a failed read left `espp::Joystick` holding its last
mapped position (its `update()` returns early on a failed `get_values`). A
sensor that dropped off the I2C bus while the stick was deflected kept the host
moving in that direction until the device was unplugged, which is exactly the
failure SEN-6 in [requirements.md](requirements.md) names as actively harmful.
The sibling motor-control firmware in the RAMMP organization treats "no fresh,
valid input" as a fault that drives the output to a safe state; see
[research/05-sibling-firmware-lessons.md](research/05-sibling-firmware-lessons.md).

**The rules**, implemented by `OmegaCalibration::SensorFaultMonitor` and applied
inside `mouse_tick` / `xac_tick` in `components/omega_control` (D16), so the
host tests cover the behaviour in both modes. The thresholds below are counts of
ticks. The mouse loop ticks every 10 ms and uses the defaults; the XAC loop
ticks every 5 ms and doubles each count (6, 1000, 20), so both keep the same
30 ms, 5 s and 100 ms in real time:

- **Three consecutive failed reads trip the fault.** One bad I2C transaction is
  a glitch, and tripping on it would make the stick stutter on a noisy bus.
  Three mouse ticks (six XAC ticks) is 30 ms, well inside SEN-5's 500 ms budget.
- **A reading that has not changed at all for 500 ticks (1000 for XAC) also trips.** Real hall
  readings carry noise, so an exactly identical X and Y pair for five seconds
  means the conversion engine has stopped, not that the hand is still. The
  window is deliberately long: the sensor runs with 32-sample averaging, which
  can flatten the noise at rest, and a false trip while the user holds a
  deflection would stop the cursor for no reason. A stuck sensor at rest is
  harmless either way, since the output is already centered. The window is
  configurable and `0` disables the check.
- **Recovery needs ten consecutive good reads (twenty for XAC)** (and, for a stuck fault, a
  changed value first). A sensor that flaps between working and failing stays
  centered rather than jittering.
- **The conversion counter is the fast stuck check.** The TMAG5273's
  `CONV_STATUS` register carries a 3-bit count of completed conversion sets,
  and the driver reads it in the same burst as X, Y and Z (7 bytes instead of
  6, no extra transaction). It normally advances several steps per 10 ms
  poll; 50 polls (500 ms, SEN-5's budget) with no movement trip the fault.
  This assumes the poll period is not an exact multiple of eight conversion
  periods, which at 32-sample averaging it is not.
- **The first fault is latched** with a count of every trip since boot, and
  the calibration console shows it under Diagnostics (`GET_DIAGNOSTICS`),
  together with the reset log (D23). An LED status (STA-1, STA-2) is still
  open.

**Consequence.** Drift correction and nudge detection do not run on faulted
ticks; a dead sensor must not nudge the stored center. The nudge timing resets
on recovery, the same way it does after a recalibration.

## D22: Update safety rules, and a register of deferred security controls

These rules bind the two-slot OTA update path (D18, documented in
[firmware-updates.md](firmware-updates.md)). They come from the OTA design
study in the ATOS repository, the most thorough treatment of OTA among the
sibling projects; see
[research/05-sibling-firmware-lessons.md](research/05-sibling-firmware-lessons.md).

**Rules.**

1. **An update is only applied from a safe state.** Before the firmware
   accepts a new image, the output must be neutral and must stay neutral until
   the reboot: the host is talking to the console, not playing. A user driving
   a pointer must never have the device vanish mid-motion.
   *Implemented.* `CalibrationService` raises `Config::output_hold` from an
   accepted `OTA_BEGIN`, and also before `OTA_FINISH`, `REVERT`, `RESTART` and
   an input-mode switch defer their reboot. It lowers the hold when an upload
   ends without one (`OTA_ABORT`, the 30 s idle expiry, a failed write or
   finish, a lost transport) and when the reply that was to precede the reboot
   was not queued. Each service reports only its own edges, and `main.cpp`
   counts them in an atomic, so two services sharing the OTA engine never
   release each other's hold. The polling loop passes "count above zero" to
   `mouse_tick` / `xac_tick` as `output_hold`, which send a neutral report on
   every tick while it is set (stick centered, no motion, buttons released) and
   hold back a pending recalibration.
2. **A new image is confirmed by the console, not by itself.** After a reboot
   into a pending image the console reconnects and checks it: the running
   image's version and SHA match what it flashed and it is pending, a
   GET_STATE round trip succeeds, and GET_DIAGNOSTICS reports a sensor that is
   healthy and has been observed (skipped if the device does not support
   diagnostics). The sensor status byte is 3 ("not observed") from boot until
   the polling loop has read the sensor 10 times in a row, so the boot-time
   zero cannot be mistaken for health; the console waits up to 10 s for that
   and needs a healthy reply twice, 1.5 s apart. Only then does it send
   MARK_VALID. The same applies after REVERT. If a check fails the console does
   not confirm, says which one failed and that the stick will go back to the
   previous firmware by itself. The firmware keeps one backstop: an image still
   pending at `ROLLBACK_DEADLINE` (120 s) marks itself invalid and reboots into
   the previous slot. A fatal init failure still rolls back at once. The manual
   "Mark valid" button stays as a deliberate override that skips the checks.
   See the amendment to D18. The reset log (D23) already feeds the crash-loop
   revert for confirmed images (D18); using it to judge a pending image's first
   boots, where an image whose first boots were abnormal resets has not shown
   it works, is still open.
3. **Downgrades stay possible.** The update path does not reject an older or
   equal version. Recovery from a bad release must not depend on the bad
   release's own good behaviour.
4. **Anti-rollback stays off until the release process manages version
   numbers.** The ESP32-S3 can burn a `secure_version` into eFuses so an older
   image can never boot again. That is irreversible per device. Enabling it
   while versions are bumped by hand would brick devices on the first
   legitimate downgrade or the first forgotten bump.

**Deferred security controls.** Each entry says why it is off and what would
make us revisit it, so a security review does not re-raise it from scratch.

| Control | Status (2026-10-10) | Why deferred | Revisit when |
| --- | --- | --- | --- |
| Secure boot v2 | Off | Needs a signing key with a custody story; irreversible per device; makes community builds unflashable without the key, against the open-hardware goal | A production run ships to users who do not build their own firmware |
| Flash encryption | Off | The flash holds only calibration, settings and the firmware itself, all of which are public or user-owned; nothing secret to protect | Credentials or a per-device identity land in flash |
| Anti-rollback (`secure_version`) | Off | Rule 4 above | Releases are tagged and versioned by CI, and a release has shipped that must never run again |
| Image signing without secure boot | Off | Images come from the GitHub release page or the Pages site over TLS. The console checks a downloaded image's size and SHA-256 against a feed on the same site, which catches corruption but not a compromised site, and nothing installs without the user's confirmation | An update installs without the user confirming it, or the images and the feed are served separately |

## D23: Reset reasons are logged across boots

Every boot records `esp_reset_reason()` in a small file next to the
calibration, together with a boot counter and whether the reason was abnormal
(panic, any watchdog, brownout, power glitch, eFuse error, CPU lockup). The
last sixteen are kept and the abnormal ones are printed at boot.

**Why.** A core dump only exists for a panic. A brownout from a weak USB port,
a watchdog reset from a stalled I2C transaction, or a loop of short-lived
boots leaves nothing behind today, and a user's report of "it sometimes
restarts" cannot be checked. The sibling medical-device firmware keeps exactly
this log and it is the first thing read when a unit comes back from the field.

**Consequence.** `ResetLog` in `components/omega_calibration/include/
reset_log.hpp` needs nothing from ESP-IDF, so the host tests cover it. The
reason is stored as the platform's integer plus a name, so a new reason code
in a later ESP-IDF still logs. The calibration console shows the log under
Diagnostics (`GET_DIAGNOSTICS`). Feeding the log into the OTA path is the
crash-loop revert amendment under D18.

## D24: Identify services by dispatcher protocol id, not module id

The espp dispatcher was experimental through 1.3.x, and the espp maintainer
says its protocol-id API is the planned long-term-support one, so the firmware
uses it. `CalibrationService` advertises `kProtocol = "omega.calibration"` and
`kProtocolVersion = 1` in its `ModuleInfo`, as espp's own services do, and the
calibration console finds the service by that id (discovery version 2) instead
of assuming module id 5, and then uses the module id discovery reports for all
its traffic. Discovery runs before the first state and firmware-info requests,
and a device that only speaks discovery version 1 (no protocol field) is
assumed to be on module 5. The `omega.` namespace is ours because the wire
format is ours; an earlier revision of D20 recorded `"espp.calibration"`, which
is replaced. Module id 5 stays the routing default. espp 1.3.6 already ships
this API, so no dependency version changed. Change the id only for an
incompatible wire format; otherwise bump the version.

## D25: v0.1.0 ships with firmware updates over USB turned off

The first tagged release is built with `CONFIG_OMEGA_STICK_OTA` off (the
`OMEGA_STICK_OTA` option in `main/Kconfig.projbuild`, default off). The OTA
code, the protocol messages and the console's update section all stay in the
tree, and updates return in a later 0.x release. A build with the option off
does not construct the update engine or the post-update health check, passes no
engine to `CalibrationService` (so the update messages reply
`function_not_supported`), and logs once at boot that updates are off. The
console treats that reply to GET_FW_INFO as "this device takes no updates over
USB": it hides the update controls, skips the automatic latest-release check
and points at the first-flash page. Boards are installed and reinstalled from
that page, which erases calibration and settings by default.

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` and `partitions.csv` (two OTA slots)
do not change. The bootloader cannot be updated over USB, so a board flashed
without a rollback-capable bootloader could never take a later OTA-on release
safely; keeping both identical means any later build can be installed over
this one without reinstalling the bootloader. An image flashed over serial is
never pending verification, so rollback does not trigger in the meantime. An OTA-off
image that arrives by update from an OTA-on build boots pending, and since nothing
in it can confirm it, it marks itself valid at boot; the console then reports it as
installed and running, without further USB updates.
Message ids and the wire format are unchanged. CI builds the option on as
well (`sdkconfig.ota`) so the code keeps compiling.

This narrows D18 and D22 for this release; it does not reverse them. Their
rules apply unchanged to any build with the option on.

## D26: No usable calibration: boot quiet and calibrate from the console

A stick without a usable calibration boots quiet. It does not run the
interactive sweep at boot and does not halt: USB, the console service and the
control loop all start, and the output is held neutral, through the same path
as an update hold (`omega_control::holds_output()`, fed a `needs_calibration`
flag), until a calibration started from the console (`START_CALIBRATION`)
succeeds. The result is then applied and saved and the hold ends; a failed or
unusable sweep keeps the hold. GET_DIAGNOSTICS reports the state in a trailing
`u8 calibration_state` (0 usable, 1 needs calibration), and the console shows a
notice next to "Re-run calibration" while it is 1. A missing calibration is not
an init failure: a pending image still boots, can be checked and confirmed.

A calibration is usable only if its usable radius is at least
`OmegaCalibration::MIN_USABLE_TO_NOISE_RATIO` (3.0) times its center noise
radius. 3.0 is `CENTER_MARGIN / MAX_CENTER_DEADZONE`: below it the auto center
deadzone would have to exceed its 0.5 cap to cover the noise. The rule applies
to every sweep (`range_calibration()` refuses the result) and to the saved file
(`load()` reports `calibration_unusable` and leaves the file in place). The
#44 rule (at least half the 32 sectors covered) stays.

**Why.** On 2026-10-25 the maintainer's board ran its first-boot calibration
with the stick untouched. Noise around the center reached every sector, so the
#44 coverage rule passed, and the board saved a usable radius of 0.049 to 0.060
against a center noise radius of 0.074 to 0.086. In mouse mode every noise
sample then became cursor movement; no deadzone override could hide it (the
center override caps at 0.5), and the maintainer could not use the computer.
The boot sweep's prompts only reach a serial log that is gone once USB starts,
so a user cannot follow them, and the previous alternative, halting before USB
on a failed sweep, leaves a board that never enumerates. Quiet output with a
working console is the state the user can recover from. This resolves the open
"first-boot halt" question raised on #44.

**Consequence.** A first flash, or an older board whose saved calibration fails
the rule, needs one calibration from the console before the stick moves
anything. The manual trim constants in `main.cpp` are applied to the empty
calibration so the first console calibration keeps them, as the boot sweep did.
The DIAGNOSTICS field is additive: older consoles stop after the reset list,
and the console reads a payload without it as "not reported".

---

## Open question: compete with Willow, or collaborate?

**Willow** (Makers Making Change) already occupies most of the position this
project is aiming at: open source (CERN-OHL-W-2.0 + GPL-3.0), OSHWA-certified,
contactless Hall-effect sensing, USB HID mouse and gamepad, ~$180 BOM, and backed
by an established volunteer fabrication network that can actually get devices to
individuals.

It sits at **25 gf**. That is the gap, and on current evidence it is the only
substantial one.

Before committing to differentiation, the honest question is whether the people
this project exists to serve are better off with a competing open joystick or
with a low-force contribution to an ecosystem that already has distribution,
certification, and a maker network. Options worth weighing:

- Ship independently and differentiate on force range, the measurement protocol,
  and price.
- Contribute a low-force variant or return mechanism into the OpenAT family.
- Ship independently but deliberately match their mounting, licensing, and
  fabrication conventions so parts and knowledge transfer between the two.

Deciding this late is more expensive than deciding it now, because it affects
licence choice, mechanical interfaces, and how the project introduces itself.

## Out of scope for v1

Explicitly excluded, with the reason, so these are not relitigated per pull
request.

| Item | Reason |
| --- | --- |
| **Wheelchair drive control** | Safety-critical part of a mobility system. A different product category with a different obligation set, not a feature. See [compliance.md](compliance.md) |
| **BLE / wireless** | D1. Compliance cost with no platform gain |
| **Direct console connection** | D4. Requires licensing or auth silicon |
| **Analog TRRS output to XAC X1/X2 ports** | Viable (Tip = axis 1, Ring1 = axis 2, Ring2 = GND, Sleeve = 3.3 V ref) but needs a DAC and a hardware variant. Revisit for v2 |
| **Companion configuration app** | Design principle 2. Optional CDC CLI only (OUT-8) |
| **Head / eye / sip-and-puff / voice input** | Different product |
| **Battery power** | HW-5. Triggers UN 38.3 and IEC 62133-2 for no v1 benefit |
| **Any medical or therapeutic claim** | See [compliance.md](compliance.md). Changes what category the product is treated as |
