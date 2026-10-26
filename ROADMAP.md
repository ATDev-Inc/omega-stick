# Roadmap

Where the Omega Stick firmware is going, in the order the work is planned.
This is the view for contributors and users deciding what to wait for. The
day-to-day task list is [TODO.md](TODO.md); what is implemented today is in
[docs/status.md](docs/status.md); the reasoning behind the plan is in
[docs/PRD.md](docs/PRD.md) and [docs/decisions.md](docs/decisions.md).

Dates are targets, not promises. Milestones are ordered by dependency: each one
builds on the hardware verification and release machinery of the one before.
Nothing here is a medical, therapeutic, or compliance claim (see
[docs/compliance.md](docs/compliance.md)).

Last reviewed: 2026-10-12.

## Where things stand

The firmware has a working mouse mode, an XAC gamepad mode that has not been
tried on a real Xbox Adaptive Controller, calibration persisted to flash, a
browser console, firmware updates over USB with rollback (on in the first
release, D25 as amended), and a simulator for
working without a board. All of it is covered by host, web and fuzz tests in
CI, and none of the recent work has run on hardware. There is no tagged release
yet. See [README.md](README.md#project-status) for the short version.

## Milestones

### M0: first tagged release, v0.1.0 (October 2026)

The goal is a build a tester can install, and reinstall, from the browser, and
a `main` branch that reflects it. Firmware updates over USB are on in this release (D25, amended
2026-10-25); the first-flash page installs the first one and recovers a stick
that will not connect.

- Land the `owen-updates` integration branch on `main`, including the
  simulator and the `OMEGA_STICK_OTA` build switch (on by default).
- Run the first hardware pass on everything that has only been host-tested:
  mouse mode under slow movement, held positions and rapid reversals; the
  console over WebUSB and Web Serial; the first-flash page's BOOT and RESET
  steps, on a new board and over an existing install.
- Tag `v0.1.0`, which publishes the Pages site, the release feed and the
  first-flash page. Closes issue #5.

### M1: hardware-verified and demo-ready, v0.1.x (late October 2026)

A stick that can be handed to someone else for a session without a developer
beside it.

- Verify XAC mode on a real Xbox Adaptive Controller and pin down the
  port-to-axis mapping, which today writes X/Y to all four axes. Closes
  issue #2.
- Done: the TMAG5273 driver defects (DEF-1 to DEF-3 in
  [requirements.md](docs/requirements.md#11-known-defects-to-fix-first)) and
  the three known-bug tests are fixed; what remains is confirming them on the
  board in the hardware pass.
- Decide what a failed first-boot calibration does instead of halting the
  firmware before USB starts (see [TODO.md](TODO.md)).
- Status LED for sensor fault and update states (STA-1, STA-2), so a user can
  tell what is wrong without a console attached (goal G5, decision D21).
- Measure and publish the prototype's actuation force and throw. This is the
  one claim the project's positioning rests on and it is not blocked by
  firmware. Closes issue #3.

### M2: adjustable to the individual, v0.2.0

The tuning layer that goal G4 promises. Each item is body-dependent, so each
is configurable from the console and persisted on the device, with the defaults
chosen so the stick works untouched.

- Response curves, and displacement versus isometric response modes (D9).
- Per-axis gain and inversion.
- Tremor filter, available and default off (D8).
- Named profiles, so one stick can serve more than one person or task.
- Migration code for the saved-file schema when the first bump happens; the
  `schema=1` key is already written (PER-3).
- On-device calibration gesture, for users who cannot reach the console (CAL-2).
- XAC port selection (left or right stick) instead of the current fixed mapping.

### M3: USB identity and composite output, v0.3.0

The changes that touch how the host sees the device, grouped because each one
re-enumerates and is best shipped once.

- Composite HID descriptor with gamepad and mouse on one interface (D2, D3,
  OUT-2), so a mode switch no longer reboots the stick (OUT-4).
- 1 ms poll interval (OUT-5), after measuring the loop timing on hardware.
- A USB PID of the project's own, replacing the borrowed placeholder (D10,
  [compliance.md](docs/compliance.md)).
- Move to the newer espp dispatcher protocol and drop the `==1.3.8` pin, as
  its author recommends; the pin is temporary.

### M4: reproducible by others, v1.0.0

What makes the stick buildable without the maintainers (goal G6, PRD success
metrics).

- Publish editable CAD, the PCB, wiring and a bill of materials, not only the
  STLs. Closes issue #4.
- A documented force-measurement protocol (HW-2, HW-2a), the only quality
  control a printed, third-party-fabricated device has.
- Print-tolerance guidance against the force budget (HW-9).
- Feedback from at least ten users with disabilities, and at least three
  independent builds from the published files.
- Revisit the deferred security controls (secure boot, flash encryption,
  anti-rollback, image signing) against the conditions recorded in D22.

### Later, and v2

- Analog TRRS output to the XAC's X1 and X2 ports (out of scope for v1, revisit
  for v2).
- Hardware-in-the-loop tests. Today there are none; the simulator and host
  tests cover the logic, and a person covers the board.

## Decisions that shape the roadmap

These are open and belong to the maintainers, not to a pull request. They are
listed in [docs/status.md](docs/status.md#open-items-requiring-a-decision).

- Compete with Willow or collaborate. Affects the licence and the mechanical
  interfaces, and deciding late costs more.
- Whether a browser-based configuration page reaches every user. If not, the
  factory defaults must carry more weight and a stick-gesture mode toggle comes
  back (D11).
- Print tolerance against the force budget (HW-9).

## Not planned

Out of scope for v1, per the PRD: wheelchair drive control, direct console
connection, wireless or battery operation, any installed application as a
dependency, and head, eye, sip-and-puff or voice input.

## Keeping this file current

Update it in the pull request that moves a milestone, not afterwards. When a
milestone ships, move its items to [CHANGELOG.md](CHANGELOG.md) and bump the
"last reviewed" date. If this file and the code disagree, the code is right.
