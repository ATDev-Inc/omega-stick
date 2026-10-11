# Changelog

All notable changes to Omega Stick are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html). The
firmware version lives in [version.txt](version.txt); see
[docs/firmware-updates.md](docs/firmware-updates.md#firmware-versioning) for
what each part of the number means and how a release is cut.

No release has been tagged yet. Everything below ships as 0.1.0, the version
in `version.txt`.

## [Unreleased]

### Added

- **Mouse mode.** The stick drives a standard USB HID mouse: relative X/Y
  movement, with short nudges of the stick for clicks and scrolling. No host
  software is needed. ([#10])
- **Xbox Adaptive Controller mode.** A gamepad descriptor for the XAC's USB
  ports, selectable from the calibration console. Not yet verified on real XAC
  hardware. ([#10], [#16])
- **Calibration that survives power cycles.** Center and range capture on
  first boot, a noise-based center deadzone, and slow idle drift correction.
  Calibration and settings are stored as two small text files on the littlefs
  partition. ([#10])
- **Calibration console.** A single HTML page, opened from disk in a
  Chromium-based browser, that talks to the device over WebUSB or Web Serial:
  live state, manual center offset, deadzone overrides, nudge-click tuning,
  input mode, save to flash, and recalibrate. ([#16])
- **Firmware updates from the console, with automatic rollback.** Upload
  `omega_stick.bin` over USB into the inactive slot. A new image is confirmed
  only after 10 s of working USB and sensor reads; otherwise it rolls back by
  itself after 90 s, or at once if the sensor, calibration or USB fails to
  start. A "Revert to previous firmware" button switches back deliberately,
  and an upload interrupted by a disconnect never needs a restart to retry.
  ([#19])
- **Readable firmware versions.** Every build reports `MAJOR.MINOR.PATCH`,
  plus the commit for development builds, at boot and in the console, with a
  one-line Copy button for bug reports. ([#19])
- **Host, web and fuzz tests in CI.** Desktop unit tests that run the real
  espp code (calibration, store, console service, control loop, sensor
  driver, HID reports and descriptors, health check), Node tests for the
  console's protocol code checked against firmware-generated frames, and a
  libFuzzer harness for the frame parser. Known bugs are tracked as
  expected-failure tests. ([#13], [#20])
- **Project documentation.** Product requirements, settled decisions,
  compliance notes and research snapshots ([#6]), contributor and security
  guides ([#1]), brand assets ([#8]), printable STL files for the mechanical
  parts, and an AGENTS.md for AI coding agents ([#17]).

### Changed

- **Partition table: two firmware slots.** The single factory app partition
  is replaced by two OTA slots of 1920 KB each. **Breaking:** the first
  install of this layout needs one serial flash (`idf.py flash`) and loses the
  saved calibration once; later updates go through the console. ([#19])
- **espp 1.3.6 throughout.** All espp components move to 1.3.6, which also
  brings the 8-bit-axis report fix to `hid-rp`. ([#19])
- **Control-loop logic is host-tested.** The mouse and XAC loops in
  `main/main.cpp` now drive a pure control core
  (`components/omega_control`), so their behaviour is covered by unit tests.
  ([#20])
- **Console replies leave the USB receive callback.** Each USB byte stream is
  handled on its own task, so long replies are no longer cut off. ([#16])
- **No boot delay by default.** The developer-only flash window before USB
  starts defaults to 0 s. ([#16])

### Fixed

- **A second recalibration request mid-sweep is refused.** The device used to
  accept a RECALIBRATE while a sweep was running and then run another full
  sweep right after it. It now stays busy until the sweep finishes, and the
  console keeps its Re-run button disabled until the result arrives.
- **Console messages after a change tell the truth.** Switching input mode
  no longer logs a failure for a switch that worked (the device restarts
  after its OK, so there was never a state to read back), clicking the
  active mode says so instead of claiming a switch, and a failed read-back
  after an applied setting is reported as a refresh failure rather than as
  the setting failing. The OK for a mode switch now says whether the device
  is restarting, so the console reports what the device did rather than what
  it guessed. On the device, a mode switch, restart, revert or update whose
  OK could not be queued is dropped instead of acting on a change the host
  never heard about. ([#27])
- **A sensor fault no longer holds the stick deflected.** If the TMAG5273
  stops answering or freezes, the device reports a centered stick until it
  recovers, instead of repeating the last movement until it is unplugged.
  ([#21])
- **Switching input mode from the console.** The device now replies before it
  restarts into the new mode, and re-selecting the current mode does nothing.
  ([#16])
- **Manual center offset kept.** A trim set from the console is no longer
  undone by idle drift correction or a recalibration. ([#16])

### Known issues

- XAC mode has not been tested on a real Xbox Adaptive Controller.
- Actuation force and throw have not been measured yet, so force figures are
  design targets.
- Open defects are listed in
  [docs/requirements.md](docs/requirements.md#11-known-defects-to-fix-first)
  and as expected-failure tests in
  [tests/host/README.md](tests/host/README.md#known-bug-tests-xfail_test).

[Unreleased]: https://github.com/ATDev-Inc/omega-stick/commits/owen-updates
[#1]: https://github.com/ATDev-Inc/omega-stick/pull/1
[#6]: https://github.com/ATDev-Inc/omega-stick/pull/6
[#8]: https://github.com/ATDev-Inc/omega-stick/pull/8
[#10]: https://github.com/ATDev-Inc/omega-stick/pull/10
[#13]: https://github.com/ATDev-Inc/omega-stick/pull/13
[#16]: https://github.com/ATDev-Inc/omega-stick/pull/16
[#17]: https://github.com/ATDev-Inc/omega-stick/pull/17
[#19]: https://github.com/ATDev-Inc/omega-stick/pull/19
[#20]: https://github.com/ATDev-Inc/omega-stick/pull/20
[#21]: https://github.com/ATDev-Inc/omega-stick/pull/21
[#27]: https://github.com/ATDev-Inc/omega-stick/issues/27
