# Omega Stick: project status

Working status, kept out of [PRD.md](PRD.md) deliberately: the PRD specifies what
the product must be, not where the work has reached.

**Updated:** 2026-09-04

| Component | Status |
| --- | --- |
| Physical prototype | **Built** |
| Sensor driver | Implemented. Two defects identified, recorded in [requirements.md](requirements.md#11-known-defects-to-fix-first) |
| Sensor bring-up firmware | Implemented, diagnostic only |
| Joystick behaviour, output, calibration, configuration | Not implemented |
| Mechanical files for 3D printing | **Not yet published** |
| Force and throw measurement | **Not yet performed or published** |

## Highest-value next action

**Measure and publish the prototype's actuation force and throw.**

The market positioning in the PRD rests on a force figure. A prototype now exists,
so that figure is obtainable, and it is the one claim the whole document depends
on that has not been substantiated. It also gates the measurement protocol
(HW-2 / HW-2a), which the design needs anyway as the only quality control a
3D-printed, third-party-fabricated device has.

It is not blocked by firmware progress and can be done independently.

## Firmware critical path

Signal path → joystick maths → controller output → mouse mode → calibration and
persistence → Xbox Adaptive Controller validation → user feedback.

The middle of that sequence has no precedent in the library the project builds
on, so it is original work rather than integration.

## Open items requiring a decision

| Item | Where it is recorded |
| --- | --- |
| Adjustable force versus lightest-possible force. **Blocks mechanical design freeze** | [decisions.md](decisions.md#open-decision-design-principle-1) |
| Compete with Willow or collaborate. Affects licence and mechanical interfaces | [decisions.md](decisions.md#open-question-compete-with-willow-or-collaborate) |
| Whether a browser-based configuration page is reachable for all users | [decisions.md](decisions.md#d11-mode-and-configuration-are-set-from-the-host-then-persisted) |
| Print tolerance against the force budget | [requirements.md](requirements.md) (HW-9) |
| Unverified firmware and host assumptions, resolved by bench measurement | [requirements.md](requirements.md) |

## Note

[README.md](../README.md) also carries a "Project status" section aimed at
contributors arriving at the repository. This file is the fuller internal view.
If they drift apart, the README is the one users see.
