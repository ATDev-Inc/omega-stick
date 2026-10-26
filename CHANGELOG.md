# Changelog

All notable changes to Omega Stick are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html). The
firmware version lives in [version.txt](version.txt); see
[docs/firmware-updates.md](docs/firmware-updates.md#firmware-versioning) for
what each part of the number means and how a release is cut.

## [Unreleased]

Nothing yet.

## [0.1.0] - 2026-10-25

The first release. Everything below is new in it.

### Added

- **On/off switch for nudge clicks.** A "Nudge clicks" checkbox in the
  console's nudge-click block turns nudge clicks and scrolling off or on live
  (new request `SET_NUDGE_ENABLED`), so the stick only moves the cursor without
  having to max out the enter threshold. A nudge held at that moment is released
  at once, the tuning values are kept, and "Save current state to flash" keeps
  the choice (`nudge_enabled` in `omega_settings.cfg`; existing settings load
  with nudge clicks on). XAC mode is unaffected. Older firmware shows the box
  checked and disabled. ([#60](https://github.com/ATDev-Inc/omega-stick/pull/60))
- **Schema version in the saved files.** Calibration and settings now each
  carry `schema=1`, so future firmware can migrate them or refuse a file it
  cannot read and fall back to recalibrating or defaults. Existing boards keep
  their calibration and settings, since a file without the key counts as schema 1. ([#49](https://github.com/ATDev-Inc/omega-stick/pull/49))
- **Simulator `--no-ota` option.** `omega_sim --no-ota` behaves like the v0.1.0
  firmware, built with updates over USB off, so the console's no-updates view
  can be tried and tested without a board. ([#41](https://github.com/ATDev-Inc/omega-stick/pull/41))
- **Simulator connection in the console.** "Connect Simulator" links the
  console to `omega_sim` over a localhost WebSocket (works even in browsers
  without WebUSB or Web Serial) and shows a Simulator panel: a keyboard and
  pointer accessible virtual stick, motion patterns, sensor faults, drift and
  noise, next-image behavior, power-cycle and wipe, script upload with
  validation, and a live view of the simulated HID output. The page
  reconnects by itself after a simulated restart or update. ([#35])
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
  by the console after its checks pass (see Changed); otherwise it rolls back by
  itself after 120 s, or at once if the sensor, calibration or USB fails to
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

- **Firmware updates over USB are on in every build.** `OMEGA_STICK_OTA` now
  defaults to on, so the console can install new firmware from the first
  release (this reverses the "off" choice in D25). The first-flash page remains
  the way to install the first OTA-capable firmware and to recover a stick. A
  build can still opt out with the `sdkconfig.no_ota` fragment, which CI builds.
  ([#62](https://github.com/ATDev-Inc/omega-stick/pull/62))
- **A stick with no usable calibration stays still until it is calibrated
  from the console.** After a first flash, or with a saved calibration whose
  range is too small for the stick's resting noise (as a first-boot sweep of
  an untouched stick produces), the firmware no longer runs the sweep at boot
  or halts. USB and the console start, the output is held neutral, and the
  console shows a notice next to "Re-run calibration". A calibration is only
  accepted when its usable range is at least three times its center noise, so
  an untouched sweep is refused instead of turning noise into cursor
  movement. GET_DIAGNOSTICS ends with a new calibration state byte (D26).
  ([#59](https://github.com/ATDev-Inc/omega-stick/pull/59))
- **Each stick now has its own USB serial number.** It is the chip's factory
  MAC address as 12 lowercase hex digits, where every stick used to report the
  same placeholder. With two sticks plugged in, the console can now tell them
  apart when it reconnects after an update, and the browser remembers its USB
  permission per stick. Your computer will see an existing stick as a new USB
  device once, so the browser may ask you to grant access again.
  ([#55](https://github.com/ATDev-Inc/omega-stick/pull/55))
- **espp 1.3.8 throughout.** Every espp component moves from 1.3.6 to 1.3.8,
  still one release across the board (D20). For this firmware the only code
  change is in the bundled hid-rp headers, which now include what they use, so
  the host test workaround for that is gone. Nothing changes for the user of
  the stick. ([#42](https://github.com/ATDev-Inc/omega-stick/pull/42))
- **`OMEGA_STICK_OTA` build switch.** The update engine can be built out
  (`OMEGA_STICK_OTA` off), so the console shows that the firmware takes no
  updates over USB and links to the first-flash page. This was first meant to
  ship off in v0.1.0; it now ships on (see the entry above), and the switch
  remains as an opt-out. The bootloader and partition table are the same either
  way. ([#39](https://github.com/ATDev-Inc/omega-stick/pull/39))
- **The console confirms a new firmware image, not the firmware.** After an
  update or a revert the console reconnects, checks that the stick is running
  the image it flashed, answers requests and reports no sensor fault, then
  confirms it; the sensor must have been read successfully, not just be
  free of a fault, so an image whose sensor fails right after boot is not
  confirmed. If a check fails it says which one and the stick goes back to
  the previous firmware by itself, and the console reconnects to report it. The firmware only rolls back an image still
  unconfirmed after 120 s (was 90 s), and the "Mark valid" button still
  confirms without the checks. Reload the console before updating so an old
  copy of the page does not leave the new image to roll back. Not yet tested
  on hardware. ([#37](https://github.com/ATDev-Inc/omega-stick/pull/37))
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

- **A crash-loop handover is no longer undone by the rollback deadline.** When a
  confirmed image crashed three boots in a row, the stick switched to the other
  image, but with no console attached that image was rejected 120 s later and
  the stick went back to the crashing one for good. The image the handover
  boots is now accepted at once, and at most one handover happens per power-on.
  Builds with updates over USB on only. ([#57](https://github.com/ATDev-Inc/omega-stick/pull/57))
- **Firmware upload refused with a clear reason while an update is unconfirmed.**
  Starting an upload while the running image still waits for confirmation used
  to fail with a bare "io error"; it now says to confirm the image or wait for
  the rollback. ([#57](https://github.com/ATDev-Inc/omega-stick/pull/57))
- **A half-finished upload is no longer offered as the previous firmware.** After
  an aborted or expired upload, GET_FW_INFO reported the other slot's version
  as if it could be reverted to, and REVERT then failed. It now reports the slot
  as `partial`. ([#57](https://github.com/ATDev-Inc/omega-stick/pull/57))
- **Neutral output around more reboots and flash erases.** The output hold now
  goes up before an upload erases the inactive slot, the deadline rollback sends
  neutral reports for 100 ms before it reboots, the mode switch waits the same
  750 ms as the other console reboots, and a reboot reply that could not be sent
  no longer drops the hold of an upload still running. ([#57](https://github.com/ATDev-Inc/omega-stick/pull/57))

- **Automatic reconnect after an update now works over WebUSB and Web Serial.** The console handed its own candidate record to the USB and serial open calls instead of the device, so every reconnect attempt failed with "device.open is not a function" and a good update rolled back at the deadline. Found on the first hardware update. ([#56](https://github.com/ATDev-Inc/omega-stick/pull/56))
- **The console keeps a firmware update locked until it is settled.** Connect,
  Disconnect, Flash, Revert, Restart and the input mode switch stay off from
  the upload until the new firmware is confirmed or the stick has gone back,
  and the page asks before it is closed. Before, they came back between the
  restart and the confirm step, where any of them forced a rollback, and a
  Disconnect while confirming left the update stuck on "Checking the sensor".
  Now a reconnect picks the update up again, and the page says the stick goes
  back by itself unless it is confirmed. ([#56](https://github.com/ATDev-Inc/omega-stick/pull/56))
- **The console waits longer for the stick after an update.** It keeps looking
  for about 110 seconds, with the time left on screen, instead of giving up
  after 40, so a stick held back by the boot flash window or slow USB on
  Windows is still found and confirmed before its rollback at 120 seconds.
  With two sticks plugged in it asks you to pick one straight away.
  ([#56](https://github.com/ATDev-Inc/omega-stick/pull/56))
- **A lost reply at the end of an update is no longer reported as a failure.**
  If the stick already switched to the new firmware but its reply did not
  arrive, the console says the firmware is installed and offers Restart now,
  then checks and confirms it as usual. ([#56](https://github.com/ATDev-Inc/omega-stick/pull/56))
- **Flash is off while the running firmware waits to be confirmed.** The
  reason is shown under the button, and if the firmware refuses an update for
  this reason the console shows its message. A previous slot left `partial` by
  an interrupted upload turns Revert off with a note saying a new upload is the
  way forward. ([#56](https://github.com/ATDev-Inc/omega-stick/pull/56))
- **Smaller console update fixes.** A firmware file that ends inside its
  description block is refused instead of stopping the page; the firmware
  record is read again on every connect, so Flash and Revert never plan from
  another stick; a stalled download is cancelled after 20 seconds; a blocked
  release check is no longer called "offline"; and a revert that did not take
  says so instead of "the new firmware was not confirmed".
  ([#56](https://github.com/ATDev-Inc/omega-stick/pull/56))
- **TMAG5273 XY and Z range bits were swapped.** The driver wrote the X/Y range
  to the Z range bit and the reverse. The firmware sets both ranges to HIGH, so
  the stick's behaviour does not change, but a build that set only one range
  would have configured the other axis. ([#51](https://github.com/ATDev-Inc/omega-stick/pull/51))
- **TMAG5273 angle-conversion setting now reads back correctly.** The getter
  read the wrong register bits, so it never returned what had been set. The
  stick does not use it, so nothing changes in use.
  ([#47](https://github.com/ATDev-Inc/omega-stick/pull/47))
- **TMAG5273 field readings are now correct for either part variant.** The
  driver assumed the A2 full-scale values, so an A1 (x1) would have reported
  readings about 3.3x too high. It now reads the version from the sensor's
  DEVICE_ID register at start-up, so no configuration is needed (a setting can
  still force a variant). Stick feel is unchanged, since the joystick runs on
  raw counts. ([#45](https://github.com/ATDev-Inc/omega-stick/pull/45))
- **A calibration sweep that covers only a small arc is now refused.** The
  range calibration never checked that the stick was moved around at least half
  of the circle, so a short back-and-forth sweep could be saved and leave the
  range wrong. It now fails with the no-range-samples error and the console
  reports it, so you can sweep a full slow circle and try again. ([#44](https://github.com/ATDev-Inc/omega-stick/pull/44))
- **Out-of-range mouse button numbers are ignored.** A button index far outside
  1 to 3 (such as 65537) used to wrap around and press the left button; it is
  now rejected. The stick's own nudge clicks only use buttons 1 and 2, so
  normal use is unchanged. ([#46](https://github.com/ATDev-Inc/omega-stick/pull/46))
- **The console shows the running firmware version when updates are off.** The
  firmware now reports its version in the discovery reply, which is the only
  place the console can read it from in a build without updates over USB.
  ([#40](https://github.com/ATDev-Inc/omega-stick/pull/40))
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

- **Flashing without the BOOT button.** The flash page, the recovery doc and
  the README now say that firmware built with the boot window can be reflashed
  by replugging and connecting within the window, that `BOOT` is the button
  nearer the USB-C connector on the QT Py ESP32-S3, and that a full serial
  flash replaces the saved calibration while `idf.py app-flash` keeps it.
  ([#53](https://github.com/ATDev-Inc/omega-stick/pull/53))
- **Status, roadmap and task list reconciled with the code.** [docs/status.md](docs/status.md)
  records the fixed driver defects, the cleared known-bug tests, the saved-file
  schema key, the simulator and the end-to-end CI job, and lists the open
  first-boot calibration decision; TODO.md carries that decision and the
  non-atomic save as new items. ([#52](https://github.com/ATDev-Inc/omega-stick/pull/52))
- **Flashing instructions corrected.** The first-flash page and docs no longer
  tell you to leave an erase box ticked that starts unticked: the installer now
  erases without asking. They say what the first start does (the board
  calibrates by itself, with prompts nobody can see) and to run calibration
  from the console afterwards, and the README says the serial monitor loses the
  board once USB starts and that a running board needs `BOOT` held to flash.
  ([#43](https://github.com/ATDev-Inc/omega-stick/pull/43))
- **Hardware handling.** The README now warns never to plug or unplug Qwiic
  or similar connectors while the board is powered.
- **Update docs match the two-slot layout.** Removed the stale "factory app"
  wording from the README.
- **Roadmap and task list.** [ROADMAP.md](ROADMAP.md) sets out the milestones
  from the first tagged release to a stick others can build, and
  [TODO.md](TODO.md) is the maintainers' working list behind it.

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

[Unreleased]: https://github.com/ATDev-Inc/omega-stick/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/ATDev-Inc/omega-stick/releases/tag/v0.1.0
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
[#35]: https://github.com/ATDev-Inc/omega-stick/pull/35
