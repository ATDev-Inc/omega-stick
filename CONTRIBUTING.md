# Contributing to Omega Stick

Thanks for your interest in Omega Stick. This is an accessibility project: the
goal is a joystick that people with limited strength, range of motion, or
endurance can actually use. Contributions that move that goal forward are
welcome, whether or not they involve code.

By participating you agree to follow our [Code of Conduct](./CODE_OF_CONDUCT.md).

## Contents

- [Ways to contribute](#ways-to-contribute)
- [Design principles](#design-principles)
- [Development environment](#development-environment)
- [Code style](#code-style)
- [Static analysis](#static-analysis)
- [Making a change](#making-a-change)
- [Versioning](#versioning)
- [Adding dependencies](#adding-dependencies)
- [Testing](#testing)
- [Reporting security issues](#reporting-security-issues)

## Ways to contribute

**You do not need to be a programmer.** The most useful contribution to an
accessibility device is usually a description of how it fails someone in real
use.

- **Accessibility feedback.** If you use assistive technology, tell us what
  worked, what did not, and what your setup looks like. Use the [accessibility
  feedback issue
  template](https://github.com/ATDev-Inc/omega-stick/issues/new/choose). No
  technical detail required, and share only what you are comfortable sharing.
- **Bug reports.** Something behaved wrong. Use the bug report template.
- **Feature requests.** Frame these around the need, not the implementation:
  "I cannot reach the far edge of the screen" is more useful than "add a
  sensitivity register".
- **Documentation.** Setup instructions that were unclear, steps that were
  missing, wiring that was not explained.
- **Code.** See the rest of this document.

If you are looking for a place to start, the open areas are listed under
[Project status](./README.md#project-status) in the README.

## Design principles

Keep these in mind when proposing changes. They are the reason the project
exists.

1. **Force stays low and adjustable.** The device must reach under 10 gf, and
   must let the user choose a higher force if that works better for them. A
   change that raises the *minimum* achievable force is not an acceptable
   tradeoff for anything else. Note this principle originally forbade adding any
   mechanical resistance at all, which would have ruled out the adjustment
   mechanism the evidence supports. See D12 in
   [docs/decisions.md](./docs/decisions.md).
2. **No required host software.** The stick enumerates as a standard USB HID
   device so it works on a locked-down school or work machine, on a console, and
   on a tablet, with nothing installed. Do not make core functionality depend on
   a companion app or a driver.
3. **Configuration is optional, not mandatory.** The device should be usable the
   moment it is plugged in. Tuning is a refinement, never a prerequisite.
4. **Fail visibly, not silently.** If the sensor drops out or calibration is
   bad, the user needs to be able to tell. Someone relying on this device to
   operate their computer cannot debug it the way a developer can.
5. **Do not assume the user's body.** Avoid hard-coding assumptions about grip,
   hand, range of motion, or which axis someone can move. Make ranges and
   mappings adjustable.

## Development environment

This is an [ESP-IDF](https://github.com/espressif/esp-idf) project targeting
**ESP-IDF v6.0** (the version CI builds with; v5.5.1 is untested) and the
ESP32-S3, written in C++20.

1. Install ESP-IDF by following the [official getting started
   guide](https://docs.espressif.com/projects/esp-idf/en/release-v6.0/esp32s3/get-started/index.html).
   On Windows, the ESP-IDF Installation Manager CLI installs the version
   `dependencies.lock` was resolved against:

   ```console
   winget install Espressif.EIM-CLI
   eim install -i v6.0.2 -t esp32s3 -n true
   ```

   It installs into `C:\Espressif` and puts an "ESP-IDF v6.0.2 PowerShell"
   shortcut on the desktop. Run `idf.py` from that shell, or dot-source
   `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1` in an
   existing PowerShell session.
2. Clone the repository and build:

   ```console
   git clone https://github.com/ATDev-Inc/omega-stick.git
   cd omega-stick
   idf.py set-target esp32s3
   idf.py build
   ```

3. Flash and monitor:

   ```console
   idf.py -p PORT flash monitor
   ```

Component dependencies are resolved by the IDF Component Manager from
[main/idf_component.yml](main/idf_component.yml). You do not need to vendor
[ESP++ (espp)](https://github.com/esp-cpp/espp) manually.

## Code style

Formatting is enforced by `clang-format` using
[.clang-format](./.clang-format), run automatically through a
[pre-commit](https://pre-commit.com) hook.

1. Install `clang-format`.
2. Install `pre-commit`.
3. Enable the hook in your clone:

   ```console
   pre-commit install
   ```

To reformat everything at once:

```console
pre-commit run --all-files
```

Please do not send formatting-only changes to files you are not otherwise
touching; they make review harder.

### Line endings

[.gitattributes](./.gitattributes) normalizes every text file to LF, so you
should not need to configure anything. If you cloned before that was added and
see a diff touching whole files you did not edit, refresh your working tree:

```console
git rm --cached -r .
git reset --hard
```

The line-ending normalization commit is listed in
[.git-blame-ignore-revs](./.git-blame-ignore-revs). GitHub skips it in blame
automatically; to get the same locally, run:

```console
git config blame.ignoreRevsFile .git-blame-ignore-revs
```

Beyond formatting, match the surrounding code. Notable conventions in this
tree:

- C++20. Prefer `std::error_code` out-parameters over exceptions, matching the
  `espp` component style.
- Hardware drivers live in `components/<name>/` with a matching
  `idf_component_register` in that directory's `CMakeLists.txt`.
- New components must also be added to the `COMPONENTS` list in the top-level
  [CMakeLists.txt](./CMakeLists.txt) and to `REQUIRES` in
  [main/CMakeLists.txt](main/CMakeLists.txt) if `main` uses them.

## Static analysis

Pull requests are checked with `cppcheck` via the [static analysis
workflow](./.github/workflows/static_analysis.yml). Project-wide suppressions
live in [suppressions.txt](./suppressions.txt). If you need a narrow, local
suppression, prefer an inline `// cppcheck-suppress` comment with a short reason
over widening the global list.

## Making a change

1. Fork the repository and create a branch off `main`. Use a descriptive name,
   for example `fix/i2c-timeout` or `feat/hid-report-descriptor`.
2. Keep the change focused. One logical change per pull request. Unrelated
   cleanups belong in their own PR.
3. Write a clear commit message: a short imperative subject line, then a body
   explaining *why* the change is needed if that is not obvious.
4. Make sure `idf.py build` succeeds before you open the PR. If you touched
   `components/omega_calibration`, run the host tests too (see
   [Testing](#testing)).
5. Open a pull request and fill in the template. Say which hardware and which
   host OS you tested on, or say explicitly that you could not test on hardware.
   That is fine, just tell us so a maintainer can.
6. CI runs a build, static analysis, and the host tests on every PR. All three
   should pass.

Maintainers may ask for changes. Accessibility tradeoffs in particular tend to
need discussion, because a change that helps one person's use case can make the
device unusable for someone else.

## Versioning

`version.txt` holds the firmware number as `MAJOR.MINOR.PATCH`. It is bumped by
hand, in the pull request that warrants it:

- **MAJOR** when saved calibration or settings, or the console protocol, become
  incompatible.
- **MINOR** for a new user-visible feature.
- **PATCH** for fixes.

Builds embed the number alone for a release (clean tree at the tag), and
`<version>+<7-char sha>` otherwise, with `.dirty` added for uncommitted changes.
A release tag is `vMAJOR.MINOR.PATCH` (for example `v0.1.0`), placed on the merge
commit, and CI refuses to publish a release whose tag does not match
`version.txt`. See [docs/firmware-updates.md](docs/firmware-updates.md).

The console and the firmware can be on different versions, so the protocol is
append-only:

- Message IDs in
  [calibration_service.hpp](components/omega_calibration/include/calibration_service.hpp)
  are never renumbered or reused. Add new ones at the end.
- Anything that reads `FW_INFO` must tolerate extra trailing fields.

## Adding dependencies

Use the component manager rather than editing the manifest by hand:

```console
idf.py add-dependency "espp/timer>=1.0"
```

Commit both [main/idf_component.yml](main/idf_component.yml) and the updated
[dependencies.lock](./dependencies.lock).

## Testing

Everything that does not need the sensor or a USB host runs in software, on
every pull request:

- **Firmware build** for `esp32s3` (`build.yml`).
- **Host unit tests** in [tests/host](tests/host/README.md): gcc with
  coverage, clang with AddressSanitizer and UBSan, and MSVC, all with warnings
  as errors (`host_tests.yml`).
- **Web console tests** in [tests/web](tests/web/README.md) under Node
  (`web_tests.yml`), including golden frames shared with the C++ side.
- **Fuzzing** of the frame parser and calibration service in
  [tests/fuzz](tests/fuzz/README.md), a short libFuzzer run (`fuzz.yml`).
- **Static analysis** with cppcheck (`static_analysis.yml`) and a binary size
  delta comment (`size_delta.yml`).

There is still no automated hardware test. Anything touching sensor reads, USB
enumeration, or the calibration sweep's real-world behaviour needs manual
verification on a board; describe what you did in the PR. When testing input
behaviour, check more than the happy path: slow movements, held positions,
rapid direction changes, and the sensor being unplugged mid-session.

### Host unit tests

The host suite builds the project's headers (`omega_calibration`,
`omega_control`, `tmag5273`, `hid-rp-mouse`, `xac`) against the real espp
components, pinned to the versions in `dependencies.lock` and verified by
hash, with no ESP-IDF and no board. It needs CMake 3.20+ and a C++20 compiler
with `<format>` (GCC 13+, Clang 17+, MSVC 19.37+):

```console
cmake -S tests/host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

The control loops in `main/main.cpp` are thin adapters around
`components/omega_control/include/omega_control.hpp`, which is pure and
host-tested. Hardware access (I2C, time, sleeping, HID writes) is injected, so
logic changes belong in the testable layer, not in `main.cpp`.

The only stand-ins left under `tests/host/stubs/` are for the ESP-only OTA
headers (`ota.hpp`, `esp_app_desc.h`, `esp_ota_ops.h`).

If you change the calibration store, the console protocol, the calibration
file format, the control loop, or a driver's register handling, add or update a
test alongside the change. Protocol changes must keep the C++ and JavaScript
sides in step: regenerate `tests/protocol/golden_frames.json` (see
tests/host/README.md) and the web tests will check the console against it.

**Known-bug tests.** A test registered with `XFAIL_TEST` documents behaviour
that is currently wrong. It is expected to fail and does not turn the run red.
When the bug is fixed the test starts passing, the runner reports `XPASS`, and
the run fails until the marker is changed to `TEST`. That keeps a fix and its
regression test in the same PR. Do not delete an XFAIL test to make CI green;
fix the bug or leave the marker in place.

If you add another component that can be tested without hardware, adding
tests for it in the same way is very welcome.

## Reporting security issues

Do not open a public issue for a security vulnerability. See
[SECURITY.md](./SECURITY.md) for the private reporting process.
