# Firmware updates and versioning

How to update an Omega Stick over USB, how a bad update is rolled back, how
firmware versions are named, and what to include in a bug report.

## Contents

- [Overview](#overview)
- [Partition layout](#partition-layout)
- [Updating from the console](#updating-from-the-console)
- [Rollback and the health check](#rollback-and-the-health-check)
- [Firmware versioning](#firmware-versioning)
- [Reporting bugs and releasing](#reporting-bugs-and-releasing)
- [Limitations](#limitations)

## Overview

After a one-time serial flash, the firmware can be updated over the same USB
connection the calibration console uses. No programmer, driver, or boot-button
sequence is involved. The new image is written to the inactive app slot, checked,
and booted. If it does not prove itself, the device goes back to the previous
image on its own.

The pieces:

- A two-slot partition table ([partitions.csv](../partitions.csv)).
- Upload and status messages in the calibration protocol
  ([calibration_service.hpp](../components/omega_calibration/include/calibration_service.hpp)).
- The "Firmware update" section of the calibration console
  ([calibration_console.html](../components/omega_calibration/web/calibration_console.html)).
- Bootloader rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` in
  [sdkconfig.defaults](../sdkconfig.defaults)) and a health check in
  [firmware_health_check.hpp](../components/omega_calibration/include/firmware_health_check.hpp) that decides when a new image is trusted.

## Partition layout

| Name | Type | Size | Purpose |
| --- | --- | --- | --- |
| `nvs` | data | 16 KB | Non-volatile key/value storage |
| `otadata` | data | 8 KB | Records which app slot boots next, and its verify state |
| `phy_init` | data | 4 KB | Radio calibration data |
| `ota_0` | app | 1920 KB | App slot A |
| `ota_1` | app | 1920 KB | App slot B |
| `littlefs` | data | 2 MB | Saved calibration and settings |
| `coredump` | data | 64 KB | Crash dumps |

There is no factory partition. The device always runs from `ota_0` or `ota_1`,
and an update goes to whichever one is not running.

Moving a device from the old single-app layout to this one needs **one full
serial flash**:

```
idf.py -p PORT flash
```

The `littlefs` partition moved in the new table, so **saved calibration and
settings are lost in that one-time move**. Recalibrate and save again afterwards.
Every later update goes over USB from the console and keeps the saved data.

## Updating from the console

1. Build the firmware (`idf.py build`), or download `omega_stick.bin` from a
   GitHub release.
2. Open `components/omega_calibration/web/calibration_console.html` in a browser
   and connect to the device.
3. In the "Firmware update" section, choose `build/omega_stick.bin`. This is the
   app image, not the `.elf`. A valid app image starts with the byte `0xE9`.
4. Click "Flash firmware". A progress bar shows the upload.
5. The device validates the image, switches to the new slot and reboots. The
   console disconnects, so reconnect afterwards.

After reconnecting, check the Version field in the same section, then test the
stick (see [Limitations](#limitations)).

If an upload is interrupted, nothing needs a restart. Disconnecting from the
console mid-upload sends OTA_ABORT first. If the link dies without that (cable
pulled, tab closed), reconnecting and clicking Flash again over the same link
replaces the stale upload, and the firmware drops any upload that has had no
traffic for 30 seconds. Until then, a Flash attempt over the other link (WebUSB
versus Web Serial) is refused as busy.

### Protocol messages

These live in the calibration module (see the protocol comment at the top of
`calibration_service.hpp` for exact framing and error replies):

| Type | Name | Payload |
| --- | --- | --- |
| `0x57` | RESTART | none. Replies OK, then reboots about 750 ms later |
| `0x58` | OTA_BEGIN | u32 image size |
| `0x59` | OTA_DATA | u32 offset + raw image bytes. The offset must equal the bytes written so far. Replies with the total written |
| `0x5A` | OTA_FINISH | none. Validates and activates the image, replies OK, then reboots |
| `0x5B` | OTA_ABORT | none. Aborts an upload begun by this service |
| `0x5C` | GET_FW_INFO | none. Replies FW_INFO |
| `0x5D` | MARK_VALID | none. Confirms the running image and cancels rollback |
| `0x5E` | REVERT | none. Makes the other OTA slot (the previous firmware) the boot partition, replies OK, then reboots about 750 ms later. ERROR if an upload is in progress, the other slot has no app image, or the bootloader selection could not be written |
| `0xD2` | FW_INFO (reply) | u8 pending_verify, u8 session_active, then length-prefixed strings: project, version, build date, build time, ESP-IDF version, running partition, 8-char ELF SHA, boot partition, previous slot label, previous version, previous build date, previous build time, previous state |

The previous state is one of `valid`, `new`, `pending_verify`, `invalid`, `aborted`,
`undefined`, or `empty` when the other slot holds no readable image (the previous
version, date and time strings are then empty). Older firmware sends fewer strings,
so hosts treat missing ones as empty.

A build without an OTA engine wired in replies ERROR to the OTA messages.

## Rollback and the health check

The bootloader is built with app rollback enabled. A freshly installed image
boots in the **pending verify** state, and the bootloader reverts to the previous
image at the next reset unless the application confirms itself.

Nothing confirms it automatically by default, so `FirmwareHealthCheck` in
`components/omega_calibration/include/firmware_health_check.hpp` (used by
`main/main.cpp`) does it. The new image is confirmed only when all of these hold:

- USB HID has enumerated.
- The TMAG5273 sensor answers.
- The polling loop has been running for `HEALTHY_UPTIME` (10 seconds).

If the image is not healthy within `ROLLBACK_DEADLINE` (90 seconds), the firmware
marks itself invalid and reboots into the previous image, rather than limping
along until a power cycle happens to trigger the rollback.

A fatal init failure on a pending image (magnetic sensor, calibration, or USB
setup) does not wait for the deadline: the firmware rolls back immediately
instead of halting on a broken image.

Things to know:

- **Any reset in the first 10 seconds or so after an update reverts to the
  previous image.** A mode switch or an unplug in that window counts. This is by
  design: the new image has not yet shown it works.
- While pending, the console shows "pending verify" and a "Mark running image
  valid" button. It is a manual override that sends MARK_VALID and cancels the
  rollback. Once the image is confirmed this way, the automatic check stops and
  never rolls it back, even if the sensor later fails.
- The console also shows the running partition and the boot partition (the one
  the bootloader will start next). They differ while an update is pending.

### Going back deliberately

The primary way back is the console's "Revert to previous firmware" button in the
Firmware section. The section lists what is in the other slot (slot, version,
build date and time, and state), and the button flips the bootloader to that slot
and reboots the device (REVERT, about 750 ms after the OK reply). The button is
disabled while an upload is running or when the other slot holds no app image.

Things to know:

- The reverted-to image boots **pending verify** again. It confirms or rolls back
  under the same rules as any update: healthy within `HEALTHY_UPTIME` (10 seconds),
  or marked invalid and rebooted at `ROLLBACK_DEADLINE` (90 seconds).
- A slot the health check previously marked `invalid` can still be selected on
  purpose. The console shows its state so the choice is deliberate.
- Reverting again from the reverted image goes back to the newer one, so the two
  slots can be swapped repeatedly.

The fallback is to upload an older `.bin` again with the same steps as an update.
The upload does not reject images with the same or an older version
(`reject_same_version` is false), so downgrades and re-flashing the current
version both work. This is the only option right after a serial flash, when the
other slot is empty.

## Firmware versioning

The base version is `MAJOR.MINOR.PATCH` in [version.txt](../version.txt) at the
repo root (currently `0.1.0`). It is bumped by hand in the PR that warrants it:

- **MAJOR:** the saved calibration/settings format or the console protocol
  become incompatible with older consoles or firmware.
- **MINOR:** a new user-visible feature (OTA, revert).
- **PATCH:** fixes only.

The top-level [CMakeLists.txt](../CMakeLists.txt) reads it and sets `PROJECT_VER`
before `project()`. What a build embeds and reports:

| Build | Version |
| --- | --- |
| Release: clean tree, HEAD tagged exactly `v0.1.0` | `0.1.0` |
| Any other build of commit `1a2b3c4` | `0.1.0+1a2b3c4` |
| Built with uncommitted changes | `0.1.0+1a2b3c4.dirty` |
| Git unavailable (for example a source tarball) | `0.1.0+unknown` |

`idf.py -DPROJECT_VER=...` overrides all of this.

CMake re-runs on the next `idf.py build` when `version.txt` changes or a commit
moves HEAD, so an incremental build picks up a version bump or a new commit.
The `.dirty` suffix reflects the tree when CMake last ran; run
`idf.py reconfigure` first if a build's dirty state matters.

The version is embedded in the image's `esp_app_desc_t` and shown in two places:

- **Serial log, at boot:**
  `Firmware <project> <version> (built ..., idf ...) running from ota_x`
- **Console, Firmware section:** version, build date and time, ESP-IDF version,
  running and boot partitions, and the first 8 characters of the ELF SHA-256.

The console's Copy button puts a one-line summary on the clipboard:

```
omega_stick <version> (built <date> <time>, idf <idf>, sha <sha>, running <partition>)
```

## Reporting bugs and releasing

### Bug reports

Include the Copy-button line in every bug report. It carries the version and the
first 8 characters of the ELF SHA-256. A `+<commit>` dev build is identified by
its commit. A `.dirty` build cannot be reproduced from a commit, so only the ELF
SHA identifies the exact image.

### Cutting a release

1. Open a PR that bumps `version.txt` and turns the `[Unreleased]` section of
   [CHANGELOG.md](../CHANGELOG.md) into `[MAJOR.MINOR.PATCH] - YYYY-MM-DD`,
   with a fresh empty `[Unreleased]` above it and the compare links at the
   bottom updated. Make sure it builds and the host tests pass.
2. Merge it to `main`.
3. Tag the merge commit `vMAJOR.MINOR.PATCH` (for example `v0.1.0`), push the tag,
   and publish a GitHub release for it. CI refuses to publish if the tag does
   not match `version.txt`.
4. [package_main.yml](../.github/workflows/package_main.yml) builds the release
   and attaches `build/*.bin` (including `omega_stick.bin`, the file users
   upload), `build/*.elf`, `bootloader.bin`, `partition-table.bin`,
   `flasher_args.json` and `flash_args`. The release's `omega_stick.bin` reports
   exactly `MAJOR.MINOR.PATCH`.

Users then upload `omega_stick.bin` from the release through the console.
Paste the release's CHANGELOG section into the release notes, and call out any
update that needs a serial flash (a partition table change) at the top.

## Limitations

- **The health check cannot judge correctness.** It sees that USB enumerated, the
  sensor answers and the loop runs. It cannot detect a wrong-but-enumerating HID
  report layout, for example an XAC that enumerates but misreads axes. After
  every update, test the device. If it misbehaves, use "Revert to previous
  firmware" in the console, or upload the previous `.bin` if the other slot is
  empty.
- **The first move to the OTA partition table needs a serial flash**, and it
  loses saved calibration and settings (see [Partition layout](#partition-layout)).
- **No first-flash web flasher on this branch.** Flashing a blank board through
  the ROM bootloader from the browser is not included, so initial flashing uses
  `idf.py`.
