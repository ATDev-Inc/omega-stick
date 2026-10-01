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

- `store_input_mode_same_value_is_a_no_op`: re-selecting the active mode
  still fires `on_mode_changed`, which saves and reboots.
- `store_set_calibration_preserves_live_manual_offset`: a drift nudge or
  recalibration pushed from a stale local `CenterResult` reverts a manual
  offset the console set.
- `service_ok_reply_is_sent_before_mode_change_callback_runs`: the mode
  change callback (which reboots) runs before the OK reply is built, so the
  console always times out on `SET_INPUT_MODE`.

The three `xac_*` report-layout tests used to be XFAIL entries for an espp
`GamepadInputReport::get_report()` bug (it skips a fixed 2 bytes, which is one
too many for the `uint8_t` axes `XacInputReport` uses). `XacInputReport` now
overrides `get_report()` with a 1-byte offset, so those tests are ordinary
`TEST`s again and guard the override. The upstream fix is esp-cpp/espp#829;
once `dependencies.lock` picks up a release that contains it, the override can
be removed and the tests will keep guarding the layout.

## Component quirks the host build surfaced

- `omega_calibration.hpp` uses `M_PI`, which is a GNU/POSIX extension. The
  test build enables GNU extensions to match ESP-IDF's `gnu++2b`; a strict
  `-std=c++20` build fails. `std::numbers::pi` would remove the dependency.
- The header specialises `std::is_error_code_enum` for
  `OmegaCalibration::Error`, but only provides a static member
  `make_error_code`. The specialisation needs a namespace-scope overload to
  work, so `ec == OmegaCalibration::Error::file_open_failed` does not
  compile. The tests go through `support::is_error()` instead.

## Not covered here

Anything that needs a real task scheduler or USB: the concurrent
`save_to_flash` writers, the joystick recalibration race against the control
loop, and the held-button-through-calibration report bug in `main.cpp`.
