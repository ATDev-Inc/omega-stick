# Host tests for `components/omega_calibration`

Unit tests that compile the calibration headers on a desktop toolchain, with
no ESP-IDF and no hardware. They cover:

- `OmegaCalibration::save/load` and `DeviceSettings::save/load` round-trips
- `OmegaStore` load-on-construct, live setters, change callbacks, `save_to_flash`
- `CalibrationService` request decoding, replies, and store side effects,
  including the framing path through the real espp `stream_frame` parser
- `XacInputReport` (`components/xac`): the byte layout `get_report()` hands
  to `write_hid_report()`, checked against the real espp `hid-rp` headers

## Running

```
cmake -S tests/host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Needs CMake 3.20+ and a C++20 compiler with `<format>` (GCC 13+, Clang 17+,
MSVC 19.37+). On first configure the two espp headers the protocol depends
on (`stream_frame.hpp`, `dispatcher.hpp`) are downloaded pinned to the
release recorded in `dependencies.lock` and checked by SHA-256. If an
`idf.py build` has already populated `managed_components/`, those copies are
used instead. The `espp/hid-rp` component archive (about 130 KB, the same
one idf.py unpacks) is always downloaded and hash-checked, never taken from
`managed_components/`, so a local patch there cannot change the XAC report
tests' result.

## What is real and what is stubbed

| Header | Source |
|---|---|
| `omega_calibration.hpp`, `omega_store.hpp`, `calibration_service.hpp` | the component under test |
| `xac.hpp` | the component under test |
| `stream_frame.hpp`, `dispatcher.hpp` | real espp, pinned |
| `hid-rp-gamepad.hpp`, `hid-rp.hpp`, `gamepad_hat.hpp`, `hid/` | real espp `hid-rp` 1.3.3 archive, pinned |
| `format.hpp`, `hid-rp-gamepad-formatters.hpp` | `stubs/`, empty: only a `fmt::formatter` forward declaration is needed |
| `logger.hpp`, `base_component.hpp` | `stubs/`, prints to stderr via `std::format` |
| `joystick.hpp`, `range_mapper.hpp` | `stubs/`, records what `set_calibration` received |
| `file_system.hpp` | `stubs/`, root under the system temp directory |

The stubs deliberately expose a few extra accessors (marked "test-only") so
tests can assert on what the component pushed into them.

## Known-bug tests (`XFAIL_TEST`)

Some tests document behaviour that is currently wrong. They are registered
with `XFAIL_TEST` and are expected to fail; the run stays green while they
do. When the underlying bug is fixed the test starts passing, the runner
reports `XPASS`, and the run fails until the marker is changed to `TEST`.
That keeps the fix and its regression test tied together.

Current XFAIL entries:

- `store_set_calibration_preserves_live_manual_offset`: a drift nudge or
  recalibration pushed from a stale local `CenterResult` reverts a manual
  offset the console set.

Fixed, now plain tests:

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

- `omega_calibration.hpp` uses `M_PI`, which is a GNU/POSIX extension. The
  test build enables GNU extensions to match ESP-IDF's `gnu++2b`; a strict
  `-std=c++20` build fails. `std::numbers::pi` would remove the dependency.
- The header specialises `std::is_error_code_enum` for
  `OmegaCalibration::Error`, but only provides a static member
  `make_error_code`. The specialisation needs a namespace-scope overload to
  work, so `ec == OmegaCalibration::Error::file_open_failed` does not
  compile. The tests go through `support::is_error()` instead.
- On Windows (clang with the MSVC STL, or MSVC itself) the build fails in
  the pinned espp `hid-rp` headers with "no member named 'bit_cast' in
  namespace 'std'": `hid/rdf/short_item.hpp` uses `std::bit_cast` without
  including `<bit>`, which libstdc++ happens to pull in transitively. Until
  that is fixed upstream, force-include the header when configuring, for
  example with clang and Ninja:

  ```
  cmake -S tests/host -B build-host -G Ninja -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_CXX_FLAGS=-include bit"
  ```

## Not covered here

Anything that needs a real task scheduler or USB: the concurrent
`save_to_flash` writers, the joystick recalibration race against the control
loop, and the held-button-through-calibration report bug in `main.cpp`.
