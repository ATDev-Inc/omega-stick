# Host tests for `components/omega_calibration`

Unit tests that compile the calibration headers on a desktop toolchain, with
no ESP-IDF and no hardware. They cover:

- `OmegaCalibration::save/load` and `DeviceSettings::save/load` round-trips
- `OmegaStore` load-on-construct, live setters, change callbacks, `save_to_flash`
- `CalibrationService` request decoding, replies, and store side effects,
  including the framing path through the real espp `stream_frame` parser
- Firmware info and OTA through `CalibrationService`: `FW_INFO` flags, strings and
  previous-slot states, the OTA begin/data/finish/abort flow (bad offsets,
  missing session, write failure), and "revert to previous" (error cases, boot
  partition set, restart), all against the `ota.hpp` / `esp_ota_ops.h` fakes
- `OmegaCalibration::SensorFaultMonitor`: the trip, latch and recovery rules
  that center the stick on a sensor fault (D21 in `docs/decisions.md`)
- `ResetLog`: the boot counter and reset-reason ring persisted across boots
  (D23), including load tolerance for missing or damaged files
- `XacInputReport` (`components/xac`): the byte layout `get_report()` hands
  to `write_hid_report()`, checked against the real espp `hid-rp` headers
- Calibration math (`test_calibration_math.cpp`): `center_calibration`,
  `range_calibration`, `finalize_center_deadzone`, manual offsets and
  `MedianFilter3`, driven by a deterministic fake sensor and an injected
  no-op `SleepFunction` (`Config::sleep`)
- `DriftCompensator` (`test_drift.cpp`) through its time-injected `update()`
  overload: dwell, rate limit, motion reset, `reset()`
- `espp::TMAG5273` (`test_tmag5273.cpp`) over a fake 256-byte I2C register
  file: `initialize()`, read-modify-write setters, raw/scaled/temperature/angle
  reads, bus failures
- `MouseInputReport` (`test_mouse_report.cpp`): delta clamping, button bounds,
  reset semantics, report size and byte order
- HID descriptors (`test_hid_descriptor.cpp`, walker in
  `hid_descriptor_check.hpp`): a short-item parser totals the Input bits per
  Report ID and checks collection balance; applied to the XAC and mouse
  descriptors, whose bit totals must equal 8 * the bytes `get_report()` emits
- Control core (`test_control.cpp`): `components/omega_control/include/omega_control.hpp`
  is the pure core of `main/main.cpp` (nudge click, per-report output, drift
  bookkeeping, recalibration hand-off). Tests drive `mouse_step`/`xac_step` and
  `mouse_tick`/`xac_tick` with injected time and a fake `Ports` that records
  every call in order, including that a neutral report brackets every
  recalibration so a held nudge button is released

## Running

```
cmake -S tests/host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Needs CMake 3.20+ and a C++20 compiler with `<format>` (GCC 13+, Clang 17+,
MSVC 19.37+). On first configure each espp component is downloaded from the
ESP component registry, pinned to the version in `dependencies.lock` and
checked by SHA-256. If an `idf.py build` has already populated
`managed_components/`, those copies are used instead (except `hid-rp`, which
is always downloaded and hash-checked, so a local patch there cannot change
the XAC report tests' result).

### CMake options

All default to OFF.

| Option | Effect |
|---|---|
| `OMEGA_HOST_WERROR` | `-Werror` (`/WX` on MSVC) for the tests and the espp sources. espp headers are `SYSTEM` includes, so only project code can fail the build. |
| `OMEGA_HOST_SANITIZE` | `-fsanitize=address,undefined` on GCC and Clang (not MSVC). Run with `ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`. Does not link with Windows clang (MSVC ABI): `stl_asan.lib` is missing; use Linux or MSVC `/fsanitize=address` separately. |
| `OMEGA_HOST_COVERAGE` | `--coverage` on GCC and Clang. Summarise with `gcovr -r . --filter 'components/' --filter 'tests/host/' --exclude 'build.*'` after running ctest. |

```
cmake -S tests/host -B build-san -DCMAKE_BUILD_TYPE=Debug   -DCMAKE_CXX_COMPILER=clang++ -DOMEGA_HOST_WERROR=ON -DOMEGA_HOST_SANITIZE=ON
cmake --build build-san
ctest --test-dir build-san --output-on-failure
```

## What is real and what is stubbed

Nothing is stubbed. Every espp piece is the real, pinned component:

| Header | Source |
|---|---|
| `omega_calibration.hpp`, `omega_store.hpp`, `calibration_service.hpp`, `xac.hpp`, `firmware_health_check.hpp` | the components under test |
| `stream_frame.hpp`, `dispatcher.hpp` | real espp 1.3.6 |
| `base_component.hpp`, `logger.hpp` (+ `logger.cpp`) | real espp 1.3.6 |
| `format.hpp` and the vendored fmt | real espp `format` 1.3.6, header-only (`FMT_HEADER_ONLY`, set by `format.hpp`) |
| `range_mapper.hpp`, `vector2d.hpp` | real espp `math` 1.3.6 |
| `joystick.hpp` (+ `joystick.cpp`) | real espp 1.3.6 |
| `file_system.hpp` (+ `file_system.cpp`) | real espp 1.3.6 |
| `base_peripheral.hpp` | real espp 1.3.6 |
| `hid-rp-*.hpp`, `hid/` and the `*-formatters.hpp` headers | real espp `hid-rp` 1.3.6 archive |
| `ota.hpp`, `esp_app_desc.h` | `stubs/`, controllable `espp::Ota` fake and a fixed `esp_app_desc_t` |
| `esp_ota_ops.h` | `stubs/`, fake of the ESP-IDF OTA partition calls (other slot, image state, `esp_ota_set_boot_partition`), driven through `esp_ota_fake()` |

`logger.cpp`, `file_system.cpp` and `joystick.cpp` are built into the static
library `espp_host`, the same files espp's own desktop build compiles. Off
ESP-IDF, `espp::FileSystem::get_root_path()` is the current working
directory, so `test_main.cpp` switches into a throwaway temp directory before
running and removes it afterwards.

The real `Joystick` exposes no calibration getters, so tests assert on its
mapped `x()`/`y()` output (and `center_deadzone_radius()`/`range_deadzone()`)
after `update(raw_x, raw_y)`.

## Known-bug tests (`XFAIL_TEST`)

Some tests document behaviour that is currently wrong. They are registered
with `XFAIL_TEST` and are expected to fail; the run stays green while they
do. When the underlying bug is fixed the test starts passing, the runner
reports `XPASS`, and the run fails until the marker is changed to `TEST`.
That keeps the fix and its regression test tied together.

Current XFAIL entries:

- `range_calibration_rejects_insufficient_sector_coverage`:
  `MIN_COVERED_SECTORS` is never checked, so a sweep of 8 of 32 sectors is
  accepted instead of failing with `no_range_samples`.
- `tmag_angle_conversion_getter_reads_back_what_the_setter_wrote`:
  `set_angle_conversion` writes `SENSOR_CONFIG_2` bits 3:2 but
  `angle_conversion()` reads bits 7:6.
- `mouse_set_button_rejects_indices_that_wrap_into_range`: the button index is
  narrowed from `int` to `uint16_t`, so 65537 acts as button 1.

Fixed, now plain tests:

- `store_set_measured_calibration_preserves_live_manual_offset`: a drift
  nudge or recalibration pushed from the control loop's stale local
  `CenterResult` reverted a manual offset the console set. The loop now
  pushes through `OmegaStore::set_measured_calibration()`, which keeps the
  live offset, and reads `center()` back.
- `store_input_mode_same_value_is_a_no_op` and
  `service_ok_reply_is_sent_before_mode_change_callback_runs`: re-selecting
  the active mode is ignored, and `CalibrationService` applies a mode change
  only after the OK reply has been sent, so the console sees the OK before
  the device restarts.
- `xac_report_bytes_follow_descriptor_order`,
  `xac_centered_report_is_neutral`, `xac_stick_x_moves_report_x`: espp's
  `GamepadInputReport::get_report()` skips a fixed 2 bytes at the front of
  the object (report ID plus padding for the default `uint16_t` axes).
  `XacInputReport` uses `uint8_t` axes, which have no padding, so it
  overrides `get_report()` to skip only the report ID. Without that, X was
  dropped and every later field moved down a slot: the "drifts up, only
  moves in X" behaviour seen on the Xbox Adaptive Controller. Full
  deflection reads 0 and 254, not 255: hid-rp maps [-1, 1] to center 127
  plus or minus 127.

## Component quirks the host build surfaced

- hid-rp 1.3.3's `hid/rdf/short_item.hpp` uses `std::bit_cast` without
  including `<bit>`. GCC's `<array>` happens to pull it in; clang with the
  MSVC STL does not. `test_xac_report.cpp` includes `<bit>` first.

- `omega_calibration.hpp` uses `M_PI`, which is a GNU/POSIX extension. The
  test build enables GNU extensions to match ESP-IDF's `gnu++2b`; a strict
  `-std=c++20` build fails. `std::numbers::pi` would remove the dependency.
- The header specialises `std::is_error_code_enum` for
  `OmegaCalibration::Error`, but only provides a static member
  `make_error_code`. The specialisation needs a namespace-scope overload to
  work, so `ec == OmegaCalibration::Error::file_open_failed` does not
  compile. The tests go through `support::is_error()` instead.
- The pinned espp `hid-rp` header `hid/rdf/short_item.hpp` uses
  `std::bit_cast` without including `<bit>`. libstdc++ pulls it in
  transitively; the MSVC STL does not, so on Windows (clang with the MSVC STL,
  or MSVC itself) the build fails with "no member named 'bit_cast' in
  namespace 'std'". `CMakeLists.txt` force-includes `<bit>` (`-include bit`,
  `/FIbit`) for every translation unit until that is fixed upstream.
- `tmag5273.hpp`'s `convert_raw_to_mT()` is private, so `test_tmag5273.cpp`
  checks scaling through the public `read_x`/`read_z` readers over the fake
  register file. The header relies on `printf` being reachable transitively
  (no `<cstdio>` include), and `is_connected()` prints the device ID on every
  call.
- `hid-rp-mouse.hpp` narrows `int` to `uint16_t` in `set_button`/`get_button`;
  MSVC flags it (C4244). The two test files that include it silence that one
  warning (`/wd4244` in `CMakeLists.txt`) until the header is fixed.
- `range_calibration()` indexes an empty vector (undefined behaviour) when
  samples read fine but every one sits exactly on the center, because no
  sector then has a positive radius. Not tested, since it cannot be run safely.

## Golden frames

`test_golden_frames.cpp` drives every console request through the real
dispatcher and `CalibrationService` from a fixed store state and compares the
request/reply bytes with `tests/protocol/golden_frames.json`, which
`tests/web/golden.test.js` also reads. After an intentional protocol change
regenerate it with `OMEGA_WRITE_GOLDEN=1 build-host/omega_host_tests` and review
the diff. The ERROR message's platform-specific `std::errc` text is rewritten to
a fixed string so the file is identical on every platform.

## Not covered here

Anything that needs a real task scheduler or USB: the concurrent
`save_to_flash` writers, the joystick recalibration race against the control
loop, and the USB/HID writes and sensor sweep inside `EspPorts` in `main.cpp`
(the control logic itself is covered by `test_control.cpp`).
