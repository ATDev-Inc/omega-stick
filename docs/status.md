# Omega Stick: project status

Working status, kept out of [PRD.md](PRD.md) deliberately: the PRD specifies what
the product must be, not where the work has reached.

**Updated:** 2026-10-10

| Component | Status |
| --- | --- |
| Physical prototype | **Built** |
| Sensor driver | Implemented. Two defects still present, recorded in [requirements.md](requirements.md#11-known-defects-to-fix-first) |
| Calibration (centre and range capture, idle drift correction) | Implemented in `components/omega_calibration`. Persisted as files on the LittleFS partition, not NVS (see the persistence note in [decisions.md](decisions.md#d11-mode-and-configuration-are-set-from-the-host-then-persisted)) |
| Mouse mode | Implemented: relative X/Y, wheel, and nudge gestures for clicks and scroll. 10 ms HID poll interval, not the 1 ms of OUT-5 |
| XAC mode | Implemented, **not verified on real Xbox Adaptive Controller hardware** |
| Calibration console (WebUSB and Web Serial) | Implemented: `CalibrationService` with a browser page, each USB byte stream routed through a `DispatcherWorker`. Persists input mode, manual offsets, deadzone overrides and nudge tuning |
| Host unit tests and CI | Implemented: `tests/host` (calibration, store, service, XAC report layout) run by the Host tests workflow on pull requests. The harness supports expected-failure markers for known bugs; none are open |
| Firmware update over USB, automatic rollback, revert to previous, build versioning | Implemented: OTA messages in `CalibrationService`, `FirmwareHealthCheck` in `firmware_health_check.hpp`, see [firmware-updates.md](firmware-updates.md). Host-tested, **not yet hardware-tested** |
| Response curves, per-axis inversion and gain, tremor filter, profiles, status LED, schema version | Not implemented |
| Mechanical files for 3D printing | STL files for the printed parts are in [models/](../models/). Editable CAD source, PCB, and wiring are not published |
| Force and throw measurement | **Not yet performed or published** |

## Gaps against requirements and decisions

| Item | Requirement or decision | Current firmware |
| --- | --- | --- |
| Descriptor shape | OUT-2 ([requirements](requirements.md#4-host-output)), [D3](decisions.md#d3-descriptor-shape-is-xy-zrz-4-bit-hat-n-buttons) | Gamepad-only descriptor in XAC mode, mouse-only in mouse mode. No composite interface |
| Mode switch re-enumerates | OUT-4 ([requirements](requirements.md#4-host-output)), [D2](decisions.md#d2-composite-hid-gamepad-and-mouse-on-one-interface-two-report-ids) | Switching mode saves settings and calls `esp_restart()`, so the host re-enumerates |
| Poll interval | OUT-5 ([requirements](requirements.md#4-host-output)) | 10 ms in both modes, not 1 ms |
| On-device calibration gesture | CAL-2 ([requirements](requirements.md#2-calibration-and-centering)) | No stick gesture. Calibration runs at first boot (no saved calibration) or when requested from the console |
| USB VID and PID | [D10](decisions.md#d10-free-pid-under-espressifs-vid-0x303a) | pid.codes VID `0x1209`, PID `0x0d32`, not an Espressif PID |
| XAC hardware verification | [D3](decisions.md#d3-descriptor-shape-is-xy-zrz-4-bit-hat-n-buttons) | XAC mode is not verified on real XAC hardware. The port-to-axis mapping is unconfirmed, so stick X/Y is written to all four axes |

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
| Compete with Willow or collaborate. Affects licence and mechanical interfaces | [decisions.md](decisions.md#open-question-compete-with-willow-or-collaborate) |
| Whether a browser-based configuration page is reachable for all users | [decisions.md](decisions.md#d11-mode-and-configuration-are-set-from-the-host-then-persisted) |
| Print tolerance against the force budget | [requirements.md](requirements.md) (HW-9) |
| Unverified firmware and host assumptions, resolved by bench measurement | [requirements.md](requirements.md) |

## Note

[README.md](../README.md) also carries a "Project status" section aimed at
contributors arriving at the repository. This file is the fuller internal view.
If they drift apart, the README is the one users see.
