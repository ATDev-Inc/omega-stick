# Code quality review, 2026-10-26

Point-in-time snapshot of a repo-wide read-only review at `3b0f71d` (v0.1.1).
Nothing was built or run. Findings marked **verified** were re-read against the
code by a second reviewer; the rest are single-reviewer reads. Items marked
*datasheet* must be checked against the TMAG5273 datasheet before any change.

Every finding is tracked as an issue, #73 to #94; follow those, not this file,
for current status. First fixes: #95 (#80), #96 (#74), #97 (#75).

Severity: H = affects what the user's cursor or gamepad does, or persisted data;
M = real bug with limited reach; L = hygiene.

## Firmware: input path

| Sev | Where | Finding | Status |
| --- | --- | --- | --- |
| H | `components/tmag5273/include/tmag5273.hpp:473` | CONV_AVG is written to DEVICE_CONFIG_1 bits 6:4; the datasheet puts it at 4:2 with MAG_TEMPCO at 6:5. `main.cpp` asks for AVG_32 (5), which writes 0x50: the chip gets 16x averaging and a nonzero MAG_TEMPCO. `test_tmag5273.cpp:96-141, 296-306` assert the same wrong placement. Note also that requirements SEN-2 says 4x, not 32x. | verified (code), *datasheet* |
| H | `components/omega_calibration/include/omega_calibration.hpp:814` | `DriftCompensator` blends `center_x` toward raw, but rest is at `center_x + manual_offset`. With a trim set, idle drift correction slowly undoes it until the rest output reaches the idle threshold. Contradicts the comment at :143-149. Fix: blend toward `raw - manual_offset`. | verified |
| M | `main/main.cpp:635` | `on_calibration_changed` calls `apply_calibration(*live_joystick, ...)` on the console dispatcher task while the polling task may be inside `js.update()`. espp `Joystick` is not locked, so one tick can use a half-updated mapper. | plausible |
| M | `components/omega_control/include/omega_control.hpp:321` | `to_delta(jx * 15)` truncates with no carried remainder: outputs below about 0.067 send 0, so the slowest cursor speed is 1 count per tick (about 100 counts/s). Fine positioning with a light touch is lost. Product call. | verified |
| M | `omega_calibration.hpp:303-355` | `DeviceSettings::load` does no range checks: `strtof` accepts nan/inf, `input_mode` is cast float to uint8 (UB for NaN/negative), `nudge_hold_time_ms` unchecked. A corrupt file bypasses the limits the console commands enforce. | single |
| M | `calibration_service.hpp:945-960` | SET_NUDGE_SETTINGS has no bound on `time_ms`: 0 makes any single noisy sample fire a nudge click; a huge value silently disables nudging. | single |
| L | `omega_control.hpp:210-220` | `static_cast<int>` of a NaN/inf/huge joystick value is UB; nothing guards `js.x()` / `js.y()`. | plausible |
| L | `omega_control.hpp:470-525` | Drift compensator is fed while a frozen sensor reading is still within the stuck window, and `common.drift` is not reset on fault trip/recovery. | plausible |
| L | `components/xac/include/xac.hpp:85-108` | float and uint8_t overloads map to different axes (`set_x(uint8_t)` hits axis 2, `set_x(float)` axis 0); `set_joystick(0, 0)` is ambiguous; max output is 254 not 255. Production uses only the float form. Stale comments and "untested code" header. | single |
| L | `tmag5273.hpp:577-600` | `read_raw()` ignores RESULT_READY, DIAG_FAIL and POR in the CONV_STATUS byte it already reads; only the frozen-count heuristic catches a brown-out. | single |
| L | `tmag5273.hpp:798-814` | A failed DEVICE_ID read in `detect_variant` is overwritten by later calls; the driver silently falls back to the A2 scale (A1 part reads 3.3x large). | single |

## Firmware: persistence and service

| Sev | Where | Finding | Status |
| --- | --- | --- | --- |
| H | `omega_calibration.hpp:264-293, 1120-1152` | Settings and calibration saves open the live file with `trunc` and write in place. Unplugging mid-save leaves an empty or partial file (input mode falls back to mouse, calibration lost). `ResetLog::save` already does temp file + rename. | single |
| L | `main/main.cpp:642-654` | Switching input mode calls `save_to_flash`, which also persists any unsaved live edits (trim, deadzone, nudge tuning). | single |
| L | `main/main.cpp:194-201` | A failed save after recalibration is only printed; output resumes and the console is told it succeeded, then the next boot is quiet again. Cuts against "fail visibly". | verified |
| L | `calibration_service.hpp:543, 554` | `reset_parser()` / `abort_update()` are never called on transport loss outside tests, so a half frame from a dropped link can swallow the first request after reconnect. | plausible |
| L | `main/main.cpp:798-1148` | About 100 lines of USB/console wiring and per-iteration loop glue duplicated between mouse and XAC branches (D16 smell). | single |

## Firmware: dead code and misleading comments

- `tmag5273.hpp:539-560` `trigger_conversion()` writes the I2C_RD field of DEVICE_CONFIG_1, not the operating mode (which is in DEVICE_CONFIG_2). Unused.
- `tmag5273.hpp:693` temperature formula `raw/128 + 25` does not match the datasheet `25 + (raw - 17508) / 60.1`. Unused. *datasheet*
- `tmag5273.hpp:312` unconditional `printf` in `is_connected()`.
- `tmag5273.hpp:732-745` `read_angle()` does not mask reserved bits. Unused.
- `hid-rp-mouse.hpp:218` `static_assert(size == 3 || size == 4, "must be 3 bytes")` is vacuous with a wrong message.
- `firmware_health_check.hpp:21-34, 161-172` "three rules" lists four; the `revert_if_crash_looping` doc comment sits on the wrong declaration.
- `omega_control.hpp:94` says the loop runs every 10 ms; the real period adds I2C and HID time.

## Web console

| Sev | Where | Finding | Status |
| --- | --- | --- | --- |
| M | `calibration_console.html:2439, 3975` | Two top-level `function hex` in one script; the later full-length one wins, so the frame log writes whole OTA_DATA chunks as hex into an `aria-live` log during a flash. Rename the SHA one. | verified |
| M | `calibration_console.html:2885-2955` | Replies are matched by type only; the parsed correlation id is unused. A late reply after a timeout answers the next same-type request (stale state; a late OTA_DATA OK can satisfy OTA_ABORT). | plausible |
| M | `calibration_console.html:3737` | `uploading` is set only after OTA_BEGIN replies, so a timeout during erase sends no OTA_ABORT and a retry may be refused as busy. | plausible |
| L | `calibration_console.html:819` | `#log` is a single polite live region receiving every console line and frame; it buries the status/alert messages for screen-reader users. | verified (markup) |
| L | `calibration_console.html:307` | Buttons are about 30 px tall, below the 44 px target for motor-impaired users. | single |
| L | `calibration_console.html:4417` | `finally { endBusy() }` in confirm flow runs on stale early returns and can clear a newer flow's busy state. | plausible |
| L | `calibration_console.html:2596, 2527, 2960` | `getWriter()` after `port.open()` not guarded; non-ok/non-stall `transferIn` status spins with no backoff; discovery uses a single resolve slot outside the transaction chain. | plausible |
| L | `site/flash.html:136`, `scripts/release_feed.mjs` | Site accepts any https notes_url while the console restricts it; release_feed does not validate notes_url or check app.bin fits the 1920 KB slot. | single |
| L | `calibration_console.html` | 4900-line file; the sim UI (about 400 lines) and transports could be split into inlined files using the existing marker mechanism. `lastUpdate` (:4004) is dead. | single |

No XSS found: device and feed strings all go through `textContent`. Frame
decoding bounds and the post-update confirm sequence checked out.

## Simulator

- M `sim/src/sim_host.cpp:222, 386` `last_source_key_` string written on the accept thread and read/written on the firmware thread with no common lock (data race on reconnect).
- L `sim/src/ws_server.cpp:295` connection published before `hello` is sent, so telemetry can precede `hello`.
- L `ws_server.cpp:234-285` handshake read is inline on the single accept thread (5 s stall per idle client); `:319` leftover over 8192 bytes silently dropped; blocking 2 s sends on the firmware thread stall the sim clock.
- L `ws_protocol.hpp:379` control-frame size check counts the buffered fragment; close codes not validated; text not UTF-8 checked.
- L `sim/src/main.cpp:171` `--fresh` does `remove_all(--data)` with no sanity check on the path.
- L `sim/src/firmware.cpp:293, 303-314` unmarked drift from `main.cpp` (null running partition fallback, reset-history log, HID-ready wait), contrary to the `sim:` marker rule.
- L `sim/src/main.cpp:195` detached reboot threads can outlive `host` at exit.

JSON parser and WebSocket frame decoder checked out.

## Tests, build, CI

- M `.github/workflows/static_analysis.yml:47` cppcheck runs `--std=c++17 --platform=mips32` on C++20 code with `missingInclude` suppressed; files can fail to parse silently.
- M `tests/fuzz/corpus` only four reply frames, no request frames, 60 s run, no corpus cache.
- M `tests/e2e/sim_console.mjs` `waitFor(...).catch(() => {})` plus fixed `sleep()`s; same pattern as the race fixed in `facc3e6`.
- L `tests/host/CMakeLists.txt:111` fuzz target has no `-Werror`.
- L `suppressions.txt:10` `components/espp/*` matches nothing; global `unusedFunction` / `unusedStructMember` hide project dead code.
- L `host_tests.yml:56` unpinned `pip install gcovr`; `security.yml:12` still lists stale `marshall-updates`; no ESP-IDF build cache.

Checked fine: workflow permissions blocks and SHA pins, `static_analysis.yml`
`pull_request_target` safety, partition sizing (last build about 43% of a slot),
no stale XFAIL entries.
