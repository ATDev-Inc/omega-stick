# Firmware updates and versioning

How to update an Omega Stick over USB, how a bad update is rolled back, how
firmware versions are named, and what to include in a bug report.

> **Updates over USB are on by default** (decision D25, amended 2026-10-25).
> Everything below describes a build with `CONFIG_OMEGA_STICK_OTA=y`. To build
> without them, add `sdkconfig.no_ota` to the defaults, for example
> `idf.py -B build-no-ota -DSDKCONFIG=build-no-ota/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.no_ota" build`,
> or disable "Firmware updates over USB" in menuconfig. The console then
> reports that the firmware takes no updates over USB, and new firmware is
> installed from the [flash page](#first-flash-and-recovery), which erases saved
> calibration and settings. The bootloader and partition table are the same
> either way.

## Contents

- [Overview](#overview)
- [Partition layout](#partition-layout)
- [Updating from the console](#updating-from-the-console)
- [Releases and the Pages site](#releases-and-the-pages-site)
- [First flash and recovery](#first-flash-and-recovery)
- [Rollback and the health check](#rollback-and-the-health-check)
- [Firmware versioning](#firmware-versioning)
- [Reporting bugs and releasing](#reporting-bugs-and-releasing)
- [Limitations](#limitations)

## Overview

After a first flash (from the [flash page](#first-flash-and-recovery) or
`idf.py`), the firmware can be updated over the same USB
connection the calibration console uses. No programmer, driver, or boot-button
sequence is involved. The new image is written to the inactive app slot, checked,
and booted. The console then checks that the new image works and confirms it. If
it is not confirmed, the device goes back to the previous image on its own.

The pieces:

- A two-slot partition table ([partitions.csv](../partitions.csv)).
- Upload and status messages in the calibration protocol
  ([calibration_service.hpp](../components/omega_calibration/include/calibration_service.hpp)).
- The "Firmware update" section of the calibration console
  ([calibration_console.html](../components/omega_calibration/web/calibration_console.html)).
- Bootloader rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` in
  [sdkconfig.defaults](../sdkconfig.defaults)) and a rollback
  deadline in
  [firmware_health_check.hpp](../components/omega_calibration/include/firmware_health_check.hpp)
  for an image the console never confirms.

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

1. Build the firmware (`idf.py build`), or download `omega_stick-X.Y.Z.bin` (or
   `omega_stick.bin`) from a GitHub release. The console can also fetch the
   latest release itself (see [Check for latest
   firmware](#check-for-latest-firmware)).
2. Open `components/omega_calibration/web/calibration_console.html` in a browser
   (or <https://atdev-inc.github.io/omega-stick/console/>) and connect to the
   device.
3. In the "Firmware update" section, choose the app image, or drag the `.bin`
   onto the section. This is the app image, not the `.elf`.
4. The page checks the file (see below) and shows a confirm panel. Confirm it
   to flash.
5. The device validates the image, switches to the new slot and reboots. The
   page reconnects by itself and reports the result.

After it reports, check the Version field in the same section, then test the
stick (see [Limitations](#limitations)).

### Checks before flashing

Before anything is sent, the page reads the image header and app descriptor. It
blocks:

- `.elf` files, partition tables, and bootloaders or other non-app images;
- images built for another chip (it must be ESP32-S3) or another project (it
  must be `omega_stick`);
- empty or truncated files;
- files larger than the 1920 KB (`0x1E0000`) app slot.

Each block explains the problem in plain language. The confirm panel then shows
"Running X -> New Y" and classifies the change: upgrade, downgrade, same
version, same build, or development build. Downgrades and reflashing the same
build are allowed, because recovery must not depend on the running release
(decision D22 rule 3), but the panel warns about them.

### What the page shows during an update

A status area walks through Checking file, Erasing (up to a minute), Writing
NN%, Verifying, Restarting, Reconnecting, Confirming, and finally "Update
confirmed" or a note that the new image was not confirmed. After Flash or Revert the page reconnects by
itself. It keeps looking for the stick for about 110 seconds after the link
dropped, showing the time left, and only then offers a Reconnect button: the
boot flash window (`OMEGA_STICK_BOOT_FLASH_WINDOW_S`) can hold USB back for up
to a minute, Windows can be slow to enumerate the device again, and the
firmware goes back to the previous image 120 seconds after a pending boot. It
takes the exact port it was using first, then a single granted device with the
same USB id and, when it knows it, the same USB serial number (every stick reports its
own, the factory MAC as 12 lowercase hex digits). It never guesses
between several sticks: if more than one could be the one it just flashed, it
offers the Reconnect button at once so you choose, and keeps looking meanwhile.
Once reconnected it
runs the checks listed under [Rollback and the health
check](#rollback-and-the-health-check) and confirms the image itself (the
Confirming step). It reports whether the new build is running, was confirmed,
or failed a named check, in which case the stick goes back to the old build by
itself. Errors are in plain language; the raw detail stays in the log.

While an update is in flight, from the upload until the new image is confirmed
or the stick has gone back, the page locks the Connect and Disconnect buttons,
Flash, Download and install, Revert, Restart and the input mode switch, because
a restart or a second update would make the pending image roll back. It also
asks before the page is closed or reloaded. If the link is lost anyway while it
checks the image or waits for the rollback (a pulled cable, a reset), it
reconnects by itself as above. If Disconnect is used, it says that the stick
goes back to the previous firmware by itself unless the page confirms the image
within about two minutes of the restart, and the next connect picks the update
up again.

Flash is also off whenever the running image is pending verification, with the
reason shown under it: the firmware refuses a new upload (OTA_BEGIN) until the
running image is confirmed. "Mark running image valid" keeps it; otherwise wait
for the stick to go back. If the firmware refuses OTA_BEGIN for this reason, the
page shows the firmware's own message.

If the reply to OTA_FINISH or REVERT does not arrive, the page does not assume
nothing happened, because the firmware switches the boot slot before it replies
and only the restart is lost with the reply. It reads FW_INFO again. If the boot
partition now differs from the running one, the page says the new image is
installed but the stick did not restart, and offers **Restart now**; the usual
reconnect and confirm follow. If the boot partition is unchanged, nothing was
installed and the update can be retried. If the link dropped with the reply,
the page reconnects and reports what the stick runs.

### Neutral output during an update

From OTA_BEGIN until the reboot, and before every console-initiated reboot
(OTA_FINISH, REVERT, RESTART, an input-mode switch), the device sends neutral
reports: stick centered, no motion, buttons released. A user driving a pointer
never has the device vanish mid-motion. The hold goes up before OTA_BEGIN erases
the inactive slot, since the erase takes seconds and stalls the polling loop
between flash operations. If the upload ends without a reboot (abort, the 30 s
idle expiry, a failed begin, write or finish, a lost transport), normal output
resumes. This is D22 rule 1 (`Config::output_hold` in `calibration_service.hpp`).
The deadline rollback (see [Rollback and the health check](#rollback-and-the-health-check))
holds the output neutral for 100 ms before it reboots, for the same reason.

### Interrupted uploads

If an upload is interrupted, nothing needs a restart. Disconnecting from the
console mid-upload sends OTA_ABORT first. If the link dies without that (cable
pulled, tab closed), reconnecting and clicking Flash again over the same link
replaces the stale upload, and the firmware drops any upload that has had no
traffic for 30 seconds. Until then, a Flash attempt over the other link (WebUSB
versus Web Serial) is refused as busy.

An interrupted upload can leave the other slot holding an image that cannot be
verified. FW_INFO then reports the previous slot's state as `partial`. The
console treats it like `empty`: Revert stays off, and a note under it says an
interrupted upload left a partial image, so a new upload is the way forward.

### Check for latest firmware

The "Check for latest firmware" button reads
`https://atdev-inc.github.io/omega-stick/firmware/latest.json` (which appears with the first full release; the copy of the
console on the Pages site reads its own sibling `firmware/latest.json`; there is
no way to point it elsewhere from the page address), says whether an
update is available, and links the release notes. "Download and install" fetches
the app image, verifies its size and SHA-256 against the feed, then runs the same
checks and confirm panel as a picked file. The download is cancelled if no data
arrives for 20 seconds. A failed check or download says whether the computer is
offline, the server did not answer in time, or the request was blocked (a
network, firewall or browser security rule; browsers report this the same way
as a network failure). An optional check runs once after
connecting ("Check for new firmware when I connect"; on by default, remembered
in the browser).

### Protocol messages

These live in the calibration module (see the protocol comment at the top of
`calibration_service.hpp` for exact framing and error replies):

| Type | Name | Payload |
| --- | --- | --- |
| `0x57` | RESTART | none. Replies OK, then reboots about 750 ms later |
| `0x58` | OTA_BEGIN | u32 image size. ERROR (operation_not_permitted) while the running image is still pending verify: the bootloader would refuse the upload, so confirm the image (MARK_VALID) or let it roll back first |
| `0x59` | OTA_DATA | u32 offset + raw image bytes. The offset must equal the bytes written so far. Replies with the total written |
| `0x5A` | OTA_FINISH | none. Validates and activates the image, replies OK, then reboots |
| `0x5B` | OTA_ABORT | none. Aborts an upload begun by this service |
| `0x5C` | GET_FW_INFO | none. Replies FW_INFO |
| `0x5D` | MARK_VALID | none. Confirms the running image and cancels rollback. The console sends it once its checks pass; the firmware never sends it for itself |
| `0x5E` | REVERT | none. Makes the other OTA slot (the previous firmware) the boot partition, replies OK, then reboots about 750 ms later. ERROR if an upload is in progress, the other slot has no app image, or the bootloader selection could not be written |
| `0xD2` | FW_INFO (reply) | u8 pending_verify, u8 session_active, then length-prefixed strings: project, version, build date, build time, ESP-IDF version, running partition, 8-char ELF SHA, boot partition, previous slot label, previous version, previous build date, previous build time, previous state |

The previous state is one of:

- `valid`, `new`, `pending_verify`, `invalid`, `aborted`: the slot's otadata state.
- `undefined`: a complete image with no otadata entry, such as the image a serial
  flash left in `ota_0`.
- `partial`: the slot starts with an app descriptor, so the previous version, date
  and time are still reported, but the image does not verify. This is what an
  upload that was aborted or expired leaves behind (OTA_BEGIN erases the slot and
  drops its otadata entry; the descriptor is in the first bytes written), and what
  the slot holds while an upload runs. REVERT to it would fail, so hosts treat it
  like `empty`. Firmware before this value reported such a slot as `undefined`.
- `empty`: the other slot holds no readable image (the previous version, date and
  time strings are then empty).

Telling `partial` apart reads the whole other slot (`esp_image_verify()`), a
fraction of a second per GET_FW_INFO. Older firmware sends fewer strings, so hosts
treat missing ones as empty.

A build without an OTA engine wired in replies ERROR to the OTA messages.

## Releases and the Pages site

Publishing a non-prerelease GitHub release makes
[package_main.yml](../.github/workflows/package_main.yml) also build a merged
full-flash image (`idf.py merge-bin`) and publish
<https://atdev-inc.github.io/omega-stick/>:

| Path | Contents |
| --- | --- |
| `/` | Landing page |
| `/console/` | A copy of the calibration console |
| `/flash.html` | First flash and recovery, using ESP Web Tools 10.4.0 |
| `/firmware/omega_stick-X.Y.Z.bin` | App image, for console updates |
| `/firmware/omega_stick-X.Y.Z-full.bin` | Merged bootloader, partition table and app, written at offset 0 |
| `/firmware/manifest.json` | ESP Web Tools manifest for the full image |
| `/firmware/latest.json` | Feed the console reads |
| `/firmware/SHA256SUMS` | `sha256sum -c` format |

The same `/firmware/` files are attached to the GitHub release. Browsers fetch
from Pages because GitHub release downloads send no CORS headers and Pages
does. Drafts and prereleases do not update Pages.

`latest.json` is written by [scripts/release_feed.mjs](../scripts/release_feed.mjs).
Paths are relative to the file itself:

```json
{
  "schema": 1,
  "project": "omega_stick",
  "version": "0.2.0",
  "tag": "v0.2.0",
  "published_at": "2026-10-11T12:00:00.000Z",
  "notes_url": "https://github.com/ATDev-Inc/omega-stick/releases/tag/v0.2.0",
  "app": { "path": "omega_stick-0.2.0.bin", "size": 0, "sha256": "<hex>" },
  "full": { "path": "omega_stick-0.2.0-full.bin", "size": 0, "sha256": "<hex>" },
  "chip": "esp32s3"
}
```

`version` is `MAJOR.MINOR.PATCH` and `tag` is `v` plus that version. `size` is
in bytes and `sha256` is hex.

Pages is already set up for this repository (Settings > Pages > Source:
GitHub Actions, and the `github-pages` environment allows `main` and `v*`
tags). A fork needs the same two settings before its first release can
deploy. The site goes live with the first full release whose tag contains the
Pages workflow; until then (2026-10-25: both URLs return 404) the flash page,
the hosted console and the feed are not there, and the console's
latest-release check reports the feed as unreachable.

## First flash and recovery

A blank board, a board on the old single-app layout, or a stick that will not
connect to the console is flashed from
<https://atdev-inc.github.io/omega-stick/flash.html> (Chrome or Edge, Web
Serial). **A first flash erases everything, including saved calibration and
settings.** Normal updates go through the console and keep them.

1. Put the board in download mode. A blank board usually enters it by itself; a
   board with firmware needs it because its USB port is busy acting as a mouse.
   Plug in a data-capable USB cable, hold `BOOT`, press and release `RESET`,
   then release `BOOT`. With no `RESET` button, unplug, hold `BOOT`, plug in,
   then release. On the QT Py ESP32-S3 both buttons are on the top of the board
   next to the USB-C connector, `BOOT` being the one nearer the connector. The
   board looks inactive in this mode and shows up as a serial device.
   Firmware built with `OMEGA_STICK_BOOT_FLASH_WINDOW_S` set (a developer
   setting, 0 in releases) needs no buttons: for that many seconds after
   plug-in the built-in USB Serial/JTAG port is still up, and the installer, or
   `idf.py flash`, resets the chip into download mode by itself. Replug and
   connect within the window.
2. Choose "Connect and install", pick the serial port (in download mode the
   board appears under a different port name from the console's), then
   "Install Omega Stick". The installer erases the board, then writes the
   firmware. It takes about a minute.
3. Unplug and replug the board. With no calibration it starts quiet: USB and
   the console come up, but the output stays neutral (decision D26). Open the
   console over Web Serial, which shows the prompts, and choose "Re-run
   calibration", following its prompts. The output starts once that succeeds.

The boot-window path is verified on the Omega Stick (esptool, no BOOT button).
The `BOOT` and `RESET` steps are the standard ESP32-S3 procedure and still need
confirming on the Omega Stick hardware.

`idf.py -p PORT flash` remains the developer path.

## Rollback and the health check

The bootloader is built with app rollback enabled. A freshly installed image
boots in the **pending verify** state, and the bootloader reverts to the previous
image at the next reset unless the application confirms itself.

The firmware never confirms a new image itself, with two exceptions: a build
with updates turned off (`sdkconfig.no_ota`) accepts an image that arrived by update at boot,
since its console cannot confirm anything, and an image booted by the crash-loop
handover is accepted at once (see below). After the update's reboot the console
reconnects and checks the image, then sends MARK_VALID (0x5D). It confirms only
when all of these hold:

- The running image's version and SHA match what the console flashed, and it is
  pending verify.
- A GET_STATE round trip succeeds.
- GET_DIAGNOSTICS reports a healthy sensor that has actually been observed.
  The firmware reports sensor status 3 ("not observed yet") from boot until its
  polling loop has read the sensor 10 times in a row (about 100 ms of polling),
  so a reply made before polling started is never mistaken for a healthy
  sensor. The console asks every 0.5 s, waits up to 10 s while the status is 3,
  and then needs a healthy reply twice, at least 1.5 s apart, before it
  confirms. A fault, or a sensor still unobserved after 10 s, fails the check.
  If the device does not support diagnostics this check is skipped.

If a check fails the console does not confirm. It says which check failed and
that the stick will go back to the previous firmware by itself, and it
reconnects after that rollback to report which image the stick ended up on.
If the new image turns out to be confirmed already (older firmware that
confirms itself), the console reports success without sending MARK_VALID. Confirming from
the host also proves the whole remote path still works after the update (vendor
enumeration, the protocol, OTA itself), so a bad image cannot lock the user out
of the next update, and what counts as healthy can be changed in the console
without shipping new firmware.

The firmware keeps a backstop. `FirmwareHealthCheck` in
`components/omega_calibration/include/firmware_health_check.hpp` (used by
`main/main.cpp`) marks an image that is still pending at `ROLLBACK_DEADLINE`
(120 seconds) invalid and reboots into the previous image, rather than limping
along until a power cycle happens to trigger the rollback. It first holds the
output neutral for 100 ms (`ROLLBACK_HOLD`), so the last reports the host sees
before the device vanishes are neutral ones. The deadline is
generous so the console's confirm lands first in normal use. It exists because
some users depend on the stick to operate their computer and may not be able to
unplug or power-cycle it themselves.

A fatal init failure on a pending image (magnetic sensor or USB setup) does not
wait for the deadline: the firmware rolls back immediately instead of halting
on a broken image. A missing or unusable calibration is not an init failure: the
image boots with its output held neutral (D26) and can still be confirmed.

Things to know:

- **Any reset before the console confirms reverts to the previous image.** A
  mode switch or an unplug in that window counts, including pulling the cable
  during "Confirming". This is by design: the new image has not yet shown it
  works.
- While pending, the console shows "pending verify" and a "Mark running image
  valid" button. It is a deliberate override: it sends MARK_VALID and cancels the
  rollback without running the checks. Once the image is confirmed, by the
  console's checks or by this button, nothing rolls it back, even if the sensor
  later fails.
- **A new firmware needs a console that confirms.** A console that still waits
  for the firmware to confirm itself would never send MARK_VALID, and the new
  image would roll back at the deadline. The console change ships first (it is
  harmless against firmware that still confirms itself), and the console is
  served from the Pages site, but a tab left open with an old copy of the page
  is a risk. Reload the console before updating.
- The console also shows the running partition and the boot partition (the one
  the bootloader will start next). They differ while an update is pending.
- **A confirmed image that crashes three boots in a row hands over to the other
  slot.** The bootloader only protects a pending image, so once an image is
  confirmed a crash loop would otherwise run until someone re-flashed over
  serial. At boot the firmware reads its reset log; if the last three resets of
  this slot were crashes (panic, watchdog, CPU lockup; brownouts do not count,
  since the other image would not survive them either) and the other slot holds
  an app image that has not already been rejected, it switches the boot
  partition and restarts. One crash does nothing beyond the core dump. A crash
  in the other image is charged to that image (the reset that starts a boot
  ended the previous one), so the two slots cannot bounce the device between
  them.
- **The image the crash-loop handover boots is confirmed at once.**
  `esp_ota_set_boot_partition()` makes it boot pending, and with no console
  attached nothing would confirm it: the deadline would mark it invalid and
  reboot into the crashing image, which could then never hand over again (the
  handover skips a rejected slot), so the device would crash-loop for good. That
  image ran before and was not rejected, so the firmware accepts it itself. The
  handover leaves a marker in RTC memory that survives the software reset but
  not a power-on (`FirmwareHealthCheck::take_handover_marker()`), and the boot it
  starts sees it and confirms the image (`accept_after_handover()`). At most one
  handover happens per power-on: if the accepted image crash-loops too, both
  images are broken, and the device stays put rather than swapping back and
  forth and wearing out the otadata sectors. Unplugging it allows one more
  handover. The fallback image has to contain this code: an older OTA-on build
  in the other slot does not read the marker, so it still rejects itself at
  the deadline and the device returns to the crashing image. A stick is
  protected against a crash loop once both slots hold firmware with this
  change, that is after the second update from it.

### Going back deliberately

The primary way back is the console's "Revert to previous firmware" button in the
Firmware section. The section lists what is in the other slot (slot, version,
build date and time, and state), and the button flips the bootloader to that slot
and reboots the device (REVERT, about 750 ms after the OK reply). The button is
disabled while an update is in flight, or when the other slot holds no app image
(`empty`) or only the remains of an interrupted upload (`partial`).

Things to know:

- The reverted-to image boots **pending verify** again. It confirms or rolls back
  under the same rules as any update: the console reconnects, runs the same
  checks and confirms it, or it is marked invalid and rebooted at
  `ROLLBACK_DEADLINE` (120 seconds).
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

Both saved files carry a `schema=N` key (currently 1). A schema bump that old
firmware cannot read is a MAJOR bump, because rolling back would leave the older
firmware unable to load the user's calibration or settings (it falls back to
booting with no calibration, output held neutral, or to default settings). A
key added under the same schema is not: older
firmware ignores keys it does not know. Migration code is written when the
first schema bump happens.

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
   `flasher_args.json` and `flash_args`. For a published, non-prerelease
   release it also attaches the versioned images and feed and deploys the
   [Pages site](#releases-and-the-pages-site). The release's `omega_stick.bin`
   reports exactly `MAJOR.MINOR.PATCH`.

Users then update through the console, with the image from the release or the
console's latest-firmware check.
Paste the release's CHANGELOG section into the release notes, and call out any
update that needs a serial flash (a partition table change) at the top.

## Limitations

- **The health checks cannot judge correctness.** They see that the console can
  reach the stick, the image is the one that was flashed and the sensor reports
  no fault. It cannot detect a wrong-but-enumerating HID
  report layout, for example an XAC that enumerates but misreads axes. After
  every update, test the device. If it misbehaves, use "Revert to previous
  firmware" in the console, or upload the previous `.bin` if the other slot is
  empty.
- **The first move to the OTA partition table needs a full flash**, and it
  loses saved calibration and settings (see [Partition layout](#partition-layout)).
- **First flash is a separate path.** It uses the [flash
  page](#first-flash-and-recovery) or `idf.py`, and erases calibration and
  settings.
- **The BOOT-button download-mode steps are unverified on this hardware.** The
  boot-window path is verified: on 2026-10-18 esptool flashed the board over its
  USB Serial/JTAG port with no BOOT button. The flash page's BOOT/RESET steps
  are the standard ESP32-S3 procedure and still need confirming on the Omega
  Stick.
- **Images are checked, not signed.** The console verifies the size and SHA-256
  of a downloaded image against the feed, which catches a corrupt download but
  not a compromised site, because the feed and the image come from the same
  place. Secure boot and image signing are deferred (see D22 in
  [decisions.md](decisions.md#d22-update-safety-rules-and-a-register-of-deferred-security-controls)).
- **Hardware-tested on 2026-10-25.** Update,
  automatic reconnect, confirm, deadline rollback, bootloader rollback and
  revert all ran on a stick (XAC mode, WebUSB). Since #56 the console reconnects; before it, it never
  reconnected after an update (it passed its own record to the device open
  call), so every update with it rolled back at the deadline. Still untested on
  hardware: the latest-release check (no feed exists until the first full
  release) and pulling the cable during "Confirming".
