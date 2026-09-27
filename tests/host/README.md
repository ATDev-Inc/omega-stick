# Host tests for `components/omega_calibration`

Unit tests that compile the calibration headers on a desktop toolchain, with
no ESP-IDF and no hardware. They cover:

- `OmegaCalibration::save/load` and `DeviceSettings::save/load` round-trips
- `OmegaStore` load-on-construct, live setters, change callbacks, `save_to_flash`
- `CalibrationService` request decoding, replies, and store side effects,
  including the framing path through the real espp `stream_frame` parser

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
used instead and nothing is downloaded.

## What is real and what is stubbed

| Header | Source |
|---|---|
| `omega_calibration.hpp`, `omega_store.hpp`, `calibration_service.hpp` | the component under test |
| `stream_frame.hpp`, `dispatcher.hpp` | real espp, pinned |
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
