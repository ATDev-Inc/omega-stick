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

1. **Force stays low.** Under 10 grams of actuation force is the point of the
   device. A change that adds mechanical resistance to the input path is not an
   acceptable tradeoff for anything else.
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
**ESP-IDF v5.5.1 or v6.0** and the ESP32-S3, written in C++20.

1. Install ESP-IDF by following the [official getting started
   guide](https://docs.espressif.com/projects/esp-idf/en/release-v6.0/esp32s3/get-started/index.html).
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
4. Make sure `idf.py build` succeeds before you open the PR.
5. Open a pull request and fill in the template. Say which hardware and which
   host OS you tested on, or say explicitly that you could not test on hardware.
   That is fine, just tell us so a maintainer can.
6. CI runs a build and static analysis on every PR. Both should pass.

Maintainers may ask for changes. Accessibility tradeoffs in particular tend to
need discussion, because a change that helps one person's use case can make the
device unusable for someone else.

## Adding dependencies

Use the component manager rather than editing the manifest by hand:

```console
idf.py add-dependency "espp/timer>=1.0"
```

Commit both [main/idf_component.yml](main/idf_component.yml) and the updated
[dependencies.lock](./dependencies.lock).

## Testing

There is no automated hardware test suite yet. Until there is:

- CI verifies that the firmware builds for `esp32s3`.
- Anything touching sensor reads, calibration, or HID output needs manual
  verification on real hardware. Describe what you did in the PR.
- When testing input behavior, check more than the happy path. Test slow
  movements, held positions, rapid direction changes, and the sensor being
  unplugged mid-session.

If you add a component that can be tested without hardware, adding tests for it
is very welcome.

## Reporting security issues

Do not open a public issue for a security vulnerability. See
[SECURITY.md](./SECURITY.md) for the private reporting process.
