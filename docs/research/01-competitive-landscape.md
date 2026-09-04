# 01: Competitive landscape

Captured 2026-09-04. See [README](README.md) for method and caveats.

**Question asked:** What adaptive, clinical, and consumer joysticks already
exist, what force do they actually require, what do they cost, and what is not
served today?

---

## Headline finding

**Sub-10 gram actuation is already claimed by shipping products.** Three beat
10 gf: mo-vis Micro (8 to 8.5 gf), ASL 128 Molecule (8 gf), and Celtic Magic
Feather (adjustable to 5 gf).

None of them is open source. None is cheap. Only one is a native USB HID gaming
device.

> **The gap is not force. It is force + USB HID gamepad + open source +
> a published price under ~$200.**

---

## Product table

| Product | Vendor | Price USD | Force to full deflection | Throw | Connectivity | Hosts | Open? |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **mo-vis Micro Joystick HID** (P025-61) | mo-vis (BE) | Dealer-gated, no public price | **8.5 gf** | **3.3 mm** | USB composite HID (mouse + kbd + **gamepad**) | Win/macOS/Android/iOS/Xbox/XAC | No |
| mo-vis Multi Joystick HID (P025-62) | mo-vis | Dealer | 50 gf | 8 mm | Same | Same | No |
| **Celtic Magic Feather** CM/FJ | Celtic Magic (UK) | **£420 (~$530)** | **5 / 10 / 16 / 20 gf** user-selectable (1 to 4 magnets) | ±6° | USB, 2x 3.5 mm | PC, XAC | No (all rights reserved) |
| Celtic Magic Dangle | Celtic Magic | £190 to £260 | 10 gf | Not published | USB + 3.5 mm | PC, XAC | No |
| **ASL 128 Molecule** | ASL | Dealer-only, UNVERIFIED | **8 gf** | **3 mm** | R-net / Q-Logic / LiNX + built-in BT mouse mode | Wheelchair-mediated | No |
| ASL 130 MEC | ASL | Dealer-only | 18 gf | 13 mm | As above | As above | No |
| Permobil / HMC Mini Joystick | Permobil | Dealer-only | 13 gf | 3 mm | Chair bus + BT module | As above | No |
| Switch-It MicroPilot | Sunrise | ~$3,295 start (reseller) | 10 to 50 gf adjustable, **isometric** | **0 mm** | Chair bus | As above | No, **discontinued** |
| Switch-It MicroGuide | Sunrise | Dealer | 25 gf (NRRTS) / ~40 gf (ATandMe) | 6 mm | Chair bus | As above | No |
| Stealth Precision Mini Proportional | Stealth i-Drive | Dealer | 43 gf | 7 to 14.4 mm | i-Drive CPU | As above | No |
| *Standard wheelchair joystick* | (reference) | | **203 to 336 gf** | 19.5 to 28 mm | | | |
| **Willow Joystick** | Makers Making Change | **~$180 BOM (~$135 at volume)** | **25 gf** | ±7.5° / ±5 mm | USB HID mouse + gamepad, BLE | PC/Mac/Android/iOS/XAC | **Yes.** CERN-OHL-W-2.0 + GPL-3.0, OSHWA CA000062, Hall-effect (TLV493D) |
| MMC Spruce / Birch / Oak | Makers Making Change | DIY | 75 / 115 / 525 gf | Not published | 3.5 mm TRRS, USB | XAC, PC | Yes |
| LipSync (Gaming) | MMC / Neil Squire | ~$250 built, ~$325 BOM | 50 gf | Not published | USB HID + BT | PC, XAC | Yes |
| **Xbox Adaptive Joystick** | Microsoft | **$29.99** | **Stick force NOT published** (buttons 130 gf) | Not published | USB-C wired only | Xbox + **Windows 11 x64 only** | No |
| Xbox Adaptive Controller | Microsoft | $99.99 | n/a (hub) | n/a | 19x 3.5 mm, 2x USB | Xbox, Windows | No |
| Logitech G Adaptive Gaming Kit | Logitech | $99 | Buttons 56 to 59 gf | n/a | 3.5 mm into XAC | Requires XAC | No |
| Hori Flex | Hori | $249 | Not published | Not published | USB-A | Switch (docked only), Windows | No |
| **QuadStick FPS / Singleton** | QuadStick | **$549 / $449** (+$105 to $249 mount) | 25 gf light springs / 60 to 70 gf strong | Not published | USB HID + optional BT | PS / Switch / PC / Mac / Android | Partial (3D files only) |
| Pretorian Optima | Pretorian (UK) | $295 / £192 ex-VAT | **0.5 N ≈ 51 gf** | Not published | USB / PS2 + 3.5 mm | PC/Mac/Chromebook/iOS, XAC mode | No |
| Pretorian n-ABLER | Pretorian | £275 ex-VAT | Not published ("light touch") | Not published | USB + 3.5 mm | PC/Mac/iPad/Android | No |
| GlassOuse PRO / V1.4 / Link | GlassOuse | $799 / $599 / $499 | Not published (head-tilt) | n/a | BT 4.0 + 3.5 mm | Win/Mac/**Linux**/iOS/Android | No |
| TetraMouse TMXA2 / TMXS | TetraMouse | $449 / $349 | Not published (full manual checked) | Not published | USB HID mouse only | Windows, Mac | No |
| Penny+Giles JC2000 | Curtiss-Wright | OEM only | 1 to 3 N breakout / 2 to 4.5 N full (~102 to 204 gf) | ±20° | OEM analog, **not USB** | Component | No. **Hall, contactless**, 15M operations |
| GuliKit / 8BitDo Hall and TMR modules | Various | $18 to $60 | **Never published** | Not published | Replacement modules | Consoles | No |
| ALPS RKJXV (incumbent potentiometer) | Alps Alpine | $3 to $6 | 14 ±10 mN·m torque | 23° | Analog | Component | No |

---

## The force spectrum

- The field spans roughly **130x**: from 5 gf (Feather minimum, and a mo-vis
  5 gf adapted Micro, P002-55) up to 650 gf (mo-vis All-Around Heavy Duty).
- **Standard wheelchair joystick: 203 to 336 gf.**
- Standard gaming thumbstick: **~65 to 150 gf equivalent**, community-measured,
  **UNVERIFIED**.
- **Sub-10 gf is occupied.** mo-vis Micro (8.5 gf / 3.3 mm) and ASL Molecule
  (8 gf / 3 mm) are the displacement-based floor. Feather is adjustable to 5 gf.
  mo-vis ships a 5 gf adapted Micro but warns it requires "special attention to
  mounting, shielding and testing."

### Two architectures

| | Displacement | Isometric / zero-throw |
| --- | --- | --- |
| Example | mo-vis Micro, 8 gf @ 3.3 mm | Switch-It MicroPilot, 10 to 50 gf @ 0 mm |
| Also | ASL Molecule, 8 gf @ 3 mm | ASL 136 Micro Mini |

See [document 02](02-user-needs-and-evidence.md) for the clinical evidence on
which performs better. It is genuinely split.

### There is no standardized measurement protocol

Vendors and third parties disagree on the same products:

| Product | Vendor claim | Third party |
| --- | --- | --- |
| Switch-It MicroGuide | 25 gf (NRRTS chart) | ~40 gf (ATandMe) |
| ASL 130 MEC throw | 9 mm | 13 mm |

**Gaming vendors publish nothing at all.** Microsoft, Sony, Nintendo, GuliKit,
8BitDo, Hori, and Logitech publish zero stick actuation force. Microsoft
publishes button force in gf for the Xbox Adaptive Joystick but not stick force.

> Publishing a documented, repeatable force-test method is itself a
> differentiator. This became PRD requirement HW-2.

### Counter-signal: adjustable beats minimum

Celtic Magic states directly that *"an extremely light force stick can be just
too light and in practice be harder to use."* That is why the Feather ships a
4-step 5/10/16/20 gf magnet adjustment rather than simply going as light as
possible.

Combined with the mo-vis warning about its 5 gf variant, two independent vendors
at the low-force frontier report that too-light is a real failure mode. This
directly informed the proposed amendment to design principle 1 in the PRD.

---

## Price spectrum and the gap

| Band | Products | What it misses |
| --- | --- | --- |
| **$30 to $100** mainstream adaptive | Xbox Adaptive Joystick $29.99, XAC $99.99, Logitech kit $99 | No published force spec, host-locked, companion-not-controller |
| **$180 to $250** open DIY | Willow (~$180 BOM), LipSync | Open source, but 25 gf floor and maker-network distribution only |
| **$300 to $800** AT computer access | Optima $295, TetraMouse $449, QuadStick $449 to $549, Feather ~$530, GlassOuse $499 to $799 | 20 to 51 gf except Feather; mostly mouse emulation |
| **$3,000+** dealer-gated clinical | ASL, Permobil, Switch-It | Sub-10 gf exists here, unbuyable without a chair, an ATP, and insurance authorization |

> **The empty cell: a purchasable, priced, open-source, sub-10 gf USB HID
> gamepad in the $150 to $250 band. Nothing occupies it.**

---

## What is not served today

1. **Sub-10 gf without a wheelchair.** ASL, Permobil, and Switch-It sub-10 gf
   devices ride proprietary R-net / Q-Logic / LiNX buses. mo-vis' USB on the
   *drive* Micro is configuration-only, not an input path.

2. **Native gamepad rather than mouse emulation.** AT joysticks are
   overwhelmingly mouse devices. Optima's "gaming mode" is XAC-only. n-ABLER has
   no gamepad output at all. NRRTS documents that R-Net Bluetooth mouse
   emulation introduced *"an unexpected lag to the pointer movements"* that speed
   settings did not fix.

3. **Host lock-in.** The Xbox Adaptive Joystick is Xbox + Windows 11 x64 only,
   with no Mac, Linux, iOS, or Android. Hori Flex is docked-Switch only.
   **Linux is essentially unclaimed**: only GlassOuse, Origin, and BJOY list it.

4. **Availability.** QuadStick sells in weekly Monday drops and was fully sold
   out at capture. mo-vis and Celtic Magic's US reseller are quote-only. Ability
   Drive is defunct. Traxsys exited AT. MicroPilot is discontinued.

5. **No commercial open source at any force point.** QuadStick's repository
   holds 3D files only. Celtic Magic reserves all rights. **Zero commercial AT
   joystick ships schematics or firmware.**

6. **Drift and wear.** Potentiometer-based AT joysticks degrade in 1 to 2 years.
   Even the magnetic Feather's own manual documents the stick *"creeping on its
   own"* and requiring recalibration. Contactless sensing plus documented
   calibration is a defensible claim.

7. **Cost and insurance.** A working XAC setup runs *"upwards of $270"*.
   QuadStick runs ~$600 plus a $105 to $229 mount. Gaming devices are rarely
   funded as medically necessary (see
   [document 04](04-regulatory-funding-compliance.md)).

8. **Fatigue is the real complaint, not capability.** A gamer with cerebral
   palsy reports that 30 minutes of play leaves them *"exhausted for at least a
   few hours."*

---

## Unverified in this document

- ASL, Permobil, and Stealth **prices**: no vendor publishes MSRP
- 8BitDo Lite SE price and Hall-effect claim: official page returned 403
- Inclusive Versatility v5 Microlite "5 to 10 gf / 5 mm": sourced once, page
  would not re-fetch
- Evil Controllers $150 to $500
- Community-measured mainstream thumbstick force
- Freedom2Move: site serves a placeholder, could not verify it ships a product

---

## Sources

[NRRTS mini-joystick comparison chart](https://nrrts.org/wp-content/uploads/2019/09/Mini-Joystick-Comparison-Chart-Final.pdf)
(the best cross-vendor force and throw table found; extracted and verified
directly) ·
[Quantum proportional drive controls](https://www.quantumrehab.com/quantum-rehab-clinicians/pdf/quantum-proportional-drive-controls.pdf) ·
[mo-vis HID Joystick](https://www.mo-vis.com/products/other-assistive-devices/hid-joystick) ·
[mo-vis Micro Joystick](https://www.mo-vis.com/products/special-controls/micro-joystick) ·
[Celtic Magic Feather](https://www.celticmagic.org/feather) ·
[Celtic Magic buy options](https://www.celticmagic.org/feather-buy-options) ·
[Willow Joystick](https://github.com/makersmakingchange/Willow-Joystick) ·
[OpenAT Joysticks](https://github.com/makersmakingchange/OpenAT-Joysticks) ·
[Xbox Adaptive Joystick specs](https://www.xbox.com/en-US/accessories/controllers/xbox-adaptive-joystick) ·
[QuadStick shop](https://www.quadstick.com/shop) ·
[Pretorian Optima](https://www.pretorianuk.com/optima-joystick) ·
[Switch-It MicroPilot](https://www.sunrisemedical.com/power-wheelchairs/switch-it-electronics/joysticks/micropilot) ·
[ATandMe low-force joysticks](https://www.atandme.com/low-force-wheelchair-joysticks/) ·
[SpecialEffect GameAccess](https://gameaccess.info/low-force-joysticks-and-switches/) ·
[Access-Ability XAJ review](https://access-ability.uk/2025/03/25/xbox-adaptive-joystick-accessibility-review/) ·
[Can I Play That? XAC review](https://caniplaythat.com/2020/09/24/xbox-adaptive-controller-review-xbox-pc/) ·
[NRRTS R-Net Bluetooth lag](https://nrrts.org/wp-content/uploads/2019/06/DIRECTIONS_2016.3_RCS.pdf) ·
[Kotaku, cerebral palsy gaming fatigue](https://kotaku.com/the-unpredictable-challenges-of-gaming-with-cerebral-pa-1841668450)
