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
browser-support risk in D11.

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
sweep.

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
image confirms itself once USB HID has enumerated, the TMAG5273 is answering, and
10 s of polling have passed. Otherwise it is rolled back.

The alternative was a manual-only confirmation step. It was rejected because a
reset before confirmation silently reverts to the old image, which hid updates in
the first attempt: the user saw the old version and had no way to tell why. For a
device someone relies on to operate their computer (design principle 4), a bad
image has to undo itself, and a good one must not depend on the user remembering
a second step. The console also shows the previous slot's version and state and
offers "Revert to previous firmware", so the user can step back deliberately.

**Consequence.** The first move to the new partition table needs one serial flash
and loses saved calibration once. See [firmware-updates.md](firmware-updates.md).

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
1.3.6 dispatcher adds `ModuleInfo::protocol`, now set to `"espp.calibration"`.
The host tests pin the same release (see [tests/host](../tests/host/README.md));
the XAC report-layout tests give identical results on hid-rp 1.3.3 and 1.3.6.

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
