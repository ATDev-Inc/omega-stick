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

**Risk:** Apple forum reports suggest exotic or composite descriptors can make
iOS ignore a device entirely. Bench-verify on iPadOS early, since that is the
platform where mouse mode is the only option.

## D11: Mode and configuration are set from the host, then persisted

**Decision:** there is **no physical mode control**. Output mode, deadzone,
response curve, gain, and axis inversion are all set from a browser-based
configuration page that installs nothing, and **every setting persists to NVS**.

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
