# Omega Stick research notes

These are the research notes behind [docs/PRD.md](../PRD.md). The PRD states
conclusions; these documents preserve the findings, the numbers, the sources,
and the things that could not be verified.

| | |
| --- | --- |
| **Captured** | 2026-09-04 |
| **Method** | Four parallel web research passes, primary sources preferred over roundups |
| **Status** | **Point-in-time snapshot. Not maintained.** |

## Contents

| Document | Covers |
| --- | --- |
| [01: Competitive landscape](01-competitive-landscape.md) | Adaptive and clinical joysticks, force and price spectrum, the unoccupied gap |
| [02: User needs and evidence](02-user-needs-and-evidence.md) | Populations and prevalence, force and fatigue literature, AT abandonment, mounting |
| [03: HID and platform compatibility](03-hid-platform-compatibility.md) | HID class choice, console paths, ESP32-S3 constraints, espp capabilities, latency budget |
| [04: Regulatory, funding, compliance](04-regulatory-funding-compliance.md) | FDA status, reimbursement, FCC/CE, accessibility standards, open source |

## How to read these

**Prices and specifications drift.** Vendor pricing in document 01 was captured
on the date above. Several vendors are dealer-gated and publish no MSRP at all,
which is itself a finding rather than a gap in the research.

**Claims that could not be confirmed against a primary source are marked
`UNVERIFIED` inline.** They are kept rather than deleted, because knowing that a
figure is uncertain is more useful than not having it, but they must not be
cited as established. Anything load-bearing in the PRD should trace to a source
link here.

**Where a vendor claim conflicts with a third-party measurement, both are
recorded.** This happens more than expected in this field (see the force
measurement discussion in document 01) and is part of why the PRD makes a
published measurement protocol a requirement.

## Known gaps in this research

- **Reddit was API-blocked throughout.** Community complaints in document 01 are
  therefore sourced from AT journalism, vendor forums, and clinical literature
  rather than from user forums directly. Direct community-thread evidence for
  drift, dead zone, and repairability complaints remains **UNVERIFIED**.
- **Dealer-gated pricing.** ASL, Permobil, Stealth, and mo-vis publish no public
  prices. Figures shown for those vendors come from resellers or third-party
  comparison charts where available, and are marked accordingly.
- **TMAG5273 RMS noise figures** were not extractable from the datasheet during
  this pass. This is an open item (PRD risk R10) requiring bench measurement.
- **Sustained 1 kHz HID throughput on ESP32-S3 + TinyUSB**: no published
  benchmark exists. Open item (PRD risk R2).
- **Analog stick actuation force for mainstream gamepads** is published by no
  vendor, including Microsoft, Sony, Nintendo, GuliKit, 8BitDo, Hori, and
  Logitech. Community measurements exist but are UNVERIFIED.

## Corrections log

Product names that were used in the research briefs but **do not exist**, or
were misattributed. Recorded so nobody chases them again:

| Name used | Reality |
| --- | --- |
| QuadStick "Nexus", QuadStick "Singularity" | Do not exist. Actual models: FPS, Singleton |
| TetraMouse "XL", TetraMouse "XT" | Do not exist. Actual models: TMXA2, TMXS |
| Penny+Giles "JC200" | Does not exist. Actual part: JC2000 |
| ASL "105" as the MEC | ASL 105 is a head array. The MEC is ASL 130. Actual joystick line: 125 Orbit, 128 Molecule, 130 MEC, 136 Micro Mini, 138 |

Products found to be **defunct or discontinued** during this research:

- **Switch-It MicroPilot**: discontinued (it was the only sub-10 gf isometric
  option found)
- **Ability Drive**: defunct, domain parked
- **Traxsys**: exited the assistive technology market
- **Freedom2Move**: site serves a placeholder; could not verify it exists as a
  shipping product (UNVERIFIED)
