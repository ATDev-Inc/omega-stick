# TODO

Working task list for the maintainers. The milestone view is
[ROADMAP.md](ROADMAP.md); user-facing bugs and feature requests belong in
GitHub issues. Tick items here in the pull request that does them, and move
anything that stops being true.

Last reviewed: 2026-10-17.

## Now: get to v0.1.0

- [ ] Merge the open pull requests into `owen-updates`: #36 (simulator in
      AGENTS.md and CONTRIBUTING.md), #37 (console confirms the new image) and
      #39 (the `OMEGA_STICK_OTA` build switch that ships v0.1.0 with USB
      updates off, D25; stacked on #37).
- [ ] Decide #12 (`marshall-updates` to `main`) and #15 (`fix/xac-descriptor`
      to `main`). Both predate `owen-updates`; #15 is probably covered by the
      espp 1.3.6 adoption (D20). Check before merging or closing.
- [ ] Promote `owen-updates` to `main`.
- [ ] First hardware pass. Nothing since the sibling-firmware survey has run on
      a board. Never hot-plug the Qwiic connector while powered.
  - [ ] Mouse mode: slow movements, held positions, rapid direction changes,
        sensor unplugged mid-session (expect a centered stick, then recovery).
  - [ ] Console over WebUSB and over Web Serial: state, diagnostics,
        recalibration, deadzone overrides, mode switch and reconnect.
  - [ ] Release build with updates off: the console shows the first-flash note
        in place of the update controls, and the first-flash page reinstalls
        over an existing install.
  - [ ] Gamepad mode plugged straight into a Windows PC, no XAC: it appears in
        the Game Controllers panel (`joy.cpl`), X/Y track the stick, and it
        rests at center.
  - [ ] First-flash page: the BOOT and RESET steps are unverified on the board.
  - [ ] Loop timing after the stack protector was enabled.
- [ ] Tag `v0.1.0` on a clean tree, confirm the Pages site, the release feed and
      that the first-flash page installs the tagged build. Closes #5.
- [ ] Delete the merged and superseded local branches, and drop the leftover
      `stash@{0}` ("WIP on feat/update-ux") in the main checkout.

## Firmware defects

- [ ] DEF-1 and DEF-2 in the TMAG5273 driver, fixed together
      ([requirements.md](docs/requirements.md#11-known-defects-to-fix-first)).
      Wire the part variant into one place; `range_xy_mT()` and `range_z_mT()`
      stop being dead code.
- [ ] XFAIL: `MIN_COVERED_SECTORS` is never checked, so a sweep covering 8 of
      32 sectors is accepted.
- [ ] XFAIL: `set_angle_conversion` writes `SENSOR_CONFIG_2` bits 3:2 but the
      getter reads bits 7:6.
- [ ] XFAIL: the mouse button index narrows from `int` to `uint16_t`, so 65537
      acts as button 1.
- [ ] Flip each `XFAIL_TEST` to `TEST` in the pull request that fixes it, and
      update the list in [tests/host/README.md](tests/host/README.md).
- [ ] XAC: confirm the port-to-axis mapping on a real Xbox Adaptive Controller,
      then stop writing X/Y to all four axes. Closes #2.
- [ ] XAC: the `TODO` in `components/xac/include/xac.hpp` asks for a left or
      right stick option. Lands with the mapping fix or in M2.
- [ ] Status LED for sensor fault and update states (STA-1, STA-2). Still open
      under D21.
- [ ] Give `ResetLog::Entry` a proper slot field instead of encoding the slot in
      the reason text.
- [ ] D22 rule 2: whether the reset log should judge a pending image's first
      boots, now that the console does the confirming.

## Firmware updates over USB (M2, v0.2.0)

The code is in the tree but off in v0.1.0 builds (D25). To test it, build into
a separate directory with its own sdkconfig, so the default build's
`CONFIG_OMEGA_STICK_OTA` setting is not carried over (defaults never override
a value an existing sdkconfig already has):
`idf.py -B build-ota -DSDKCONFIG=build-ota/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ota" build`.
Check `build-ota/sdkconfig` reads `CONFIG_OMEGA_STICK_OTA=y`. See
[docs/firmware-updates.md](docs/firmware-updates.md).

- [ ] Update flow, happy path: upload, the console's checks, MARK_VALID, and
      the image stays confirmed across a power cycle.
- [ ] Bootloader rollback: unplug and replug before MARK_VALID. The board
      should come back on the previous image.
- [ ] Deadline rollback: keep the board powered and withhold MARK_VALID (close
      the console before it confirms). The board should reboot into the
      previous image at about 120 s. An unplug does not test this; it takes the
      bootloader path above.
- [ ] Update from a v0.1.0 board: install the first OTA-on release from the
      first-flash page, then update over USB from there.
- [ ] Turn `OMEGA_STICK_OTA` on by default and check the "Check for latest
      firmware" button against the live feed.

## Features (M2, v0.2.0)

- [ ] Response curves, and displacement versus isometric modes (D9).
- [ ] Per-axis gain and inversion.
- [ ] Tremor filter, default off (D8).
- [ ] Profiles.
- [ ] Settings schema version with migration (PER-3). Do this before the first
      field that changes meaning, or it is a MAJOR bump.
- [ ] On-device calibration gesture (CAL-2).
- [ ] Wire the unused TMAG5273 registers that the driver already models:
      `INT_CONFIG_1` (data-ready interrupt, SEN-4), `MAG_OFFSET_CONFIG_1/2`,
      `MAG_GAIN_CONFIG`.

## USB and dependencies (M3, v0.3.0)

- [ ] Composite HID descriptor, gamepad and mouse on one interface (D2, D3).
      Removes the reboot on mode switch.
- [ ] 1 ms poll interval (OUT-5), after the loop-timing measurement above.
- [ ] Own USB PID; the current pair is borrowed. Read
      [compliance.md](docs/compliance.md) first.
- [ ] Bump espp past 1.3.6 and adopt the dispatcher `protocol` field, its own
      pull request. Drop the `<bit>` include workaround once hid-rp ships the
      fix.

## Hardware and documentation (M1 and M4)

- [ ] Measure and publish actuation force and throw. Highest-value open item;
      not blocked by firmware. Closes #3.
- [ ] Write the force-measurement protocol (HW-2, HW-2a).
- [ ] Publish editable CAD, PCB, wiring and a BOM. Closes #4.
- [ ] Print-tolerance guidance against the force budget (HW-9).
- [ ] Reconcile `docs/status.md` with the code after each milestone. It lags.

## Tests and tooling

- [ ] Hardware-in-the-loop tests: none exist. Decide whether a bench rig is
      worth it before M3's enumeration changes.
- [ ] Host tests do not cover the concurrent `save_to_flash` writers or the
      recalibration race against the control loop; both need a scheduler.
- [ ] Run the simulator end-to-end check (`tests/e2e/`) in CI, or document why
      it stays manual.

## Decisions needed (maintainers, not pull requests)

- [ ] Compete with Willow or collaborate.
- [ ] Is a browser-based configuration page reachable for every user (D11)? If
      not, the stick-gesture mode toggle comes back.
- [ ] Print tolerance versus force budget (HW-9).
- [ ] Where internal planning lives. There is no private admin repository for
      this project yet, so this file holds only engineering tasks.
