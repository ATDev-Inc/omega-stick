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

- **Simulator connection in the console.** "Connect Simulator" links the
  console to `omega_sim` over a localhost WebSocket (works even in browsers
  without WebUSB or Web Serial) and shows a Simulator panel: a keyboard and
  pointer accessible virtual stick, motion patterns, sensor faults, drift and
  noise, next-image behavior, power-cycle and wipe, script upload with
  validation, and a live view of the simulated HID output. The page
  reconnects by itself after a simulated restart or update.
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
- **Safer, clearer console updates.** Before flashing, the console reads the
  image header and app descriptor and blocks `.elf` files, partition tables,
  bootloaders, images for another chip or project, truncated files and files
  over the 1920 KB slot. A confirm panel shows "Running X -> New Y" and
  classifies it (upgrade, downgrade, same version, same build, development
  build); downgrades and reflashing the same build stay allowed but are
  warned. A status area walks through checking, erasing, writing, verifying,
  restarting, reconnecting and the result, the page reconnects by itself
  after Flash or Revert, errors are in plain language with the raw detail in
  the log, and a `.bin` can be dragged onto the Firmware section. ([#29])
- **Neutral output during an update.** From an accepted `OTA_BEGIN` until the
  reboot, and before every console-initiated reboot (finish, revert, restart,
  input-mode switch), the device sends neutral reports; it resumes normal
  output if the upload ends without a reboot. (D22 rule 1) ([#29])
- **Check for latest firmware.** The console reads the release feed, says
  whether an update is available, links the release notes, and "Download and
  install" verifies the image's size and SHA-256 against the feed before the
  usual checks and confirm panel. An optional check runs once after
  connecting, on by default. ([#29])
- **Pages site and release feed.** Publishing a full release builds a merged
  full-flash image and publishes https://atdev-inc.github.io/omega-stick/: a
  landing page, a copy of the console, the first-flash page, and
  `omega_stick-X.Y.Z.bin`, `omega_stick-X.Y.Z-full.bin`, `manifest.json`,
  `latest.json` and `SHA256SUMS`, also attached to the release. Pages is used
  because release downloads send no CORS headers. Pages is enabled for this
  repository (Source: GitHub Actions, and the `github-pages` environment
  allows `main` and `v*` tags); a fork needs the same two settings. The site
  goes live with the first full release whose tag contains this workflow.
  ([#29])
- **First flash from the browser.** `flash.html` installs the full image with
  ESP Web Tools 10.4.0. A first flash erases calibration and settings. The
  download-mode steps are the standard ESP32-S3 procedure and are not yet
  confirmed on the Omega Stick. The console links to it from its Device and
  Firmware update sections for a board that will not connect. ([#29], [#34])
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

- **The console finds the calibration service by protocol id.** The service
  advertises `omega.calibration` version 1 through dispatcher discovery, the
  console selects it by that id and uses the module id discovery reports for
  all its traffic, and a device that only speaks discovery v1 falls back to
  module 5. Discovery now runs before the first state and firmware-info
  requests. Module 5 stays the default and the wire format is unchanged.
  (D24, [#30])
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

- **The console reconnects to the stick it just flashed.** After Flash or
  Revert it used to reconnect to any previously granted device. It now takes
  the exact saved port first, then a single matching USB id (using the USB
  serial number when it knows it), and never guesses between several sticks:
  with more than one candidate it does not pick one, and after about 40
  seconds offers a Reconnect button instead. ([#29])
- **Out-of-range deadzone overrides are rejected.** Typing 2 in the console's
  center deadzone box was stored and saved, and shrank the stick's usable
  travel until it was cleared. The device now refuses values above 0.5
  (center) or 0.1 (range), the console checks the same limits before
  sending, and an out-of-range override already saved on a device is ignored
  and logged at boot. ([#31])
- **A second recalibration request mid-sweep is refused.** The device used to
  accept a recalibration request while a sweep was running and then run another full
  sweep right after it. It now stays busy until the sweep finishes, and the
  console keeps its Re-run button disabled until the result arrives. ([#31])
- **Deadzone inputs and the firmware picker have accessible names.** Screen
  readers announce them as "Center deadzone value", "Range deadzone value" and
  "Firmware file (.bin)". ([#31])
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

### Documentation

- **Hardware handling.** The README now warns never to plug or unplug Qwiic
  or similar connectors while the board is powered.
- **Update docs match the two-slot layout.** Removed the stale "factory app"
  wording from the README.

### Known issues

- XAC mode has not been tested on a real Xbox Adaptive Controller.
- The firmware update changes (neutral output, image checks, reconnect, the
  latest-release check), the protocol id lookup and the deadzone and
  recalibration changes are covered by host and Node tests only. None has been
  tried on hardware.
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
[#29]: https://github.com/ATDev-Inc/omega-stick/pull/29
[#30]: https://github.com/ATDev-Inc/omega-stick/pull/30
[#31]: https://github.com/ATDev-Inc/omega-stick/pull/31
[#34]: https://github.com/ATDev-Inc/omega-stick/pull/34
