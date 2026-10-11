# Directory map

Where everything in this repository lives and what it is for. The
[README](README.md#repository-layout) has a short version; this file lists
every directory and the files that matter in each.

Generated build output (`build/`, `build-host/`, `managed_components/`,
`sdkconfig`) is not tracked and is not listed here.

## Top level

| Path | What it is |
|---|---|
| [README.md](README.md) | Project overview, building, flashing, the calibration console, updates |
| [DIRECTORY.md](DIRECTORY.md) | This map |
| [AGENTS.md](AGENTS.md) | Conventions and commands for AI coding agents working in the repo |
| [CONTRIBUTING.md](CONTRIBUTING.md) | How to contribute, toolchain setup, tests, pull requests |
| [CHANGELOG.md](CHANGELOG.md) | User-facing changes per release (Keep a Changelog style) |
| [SECURITY.md](SECURITY.md) | How to report a vulnerability, and the security controls in place |
| [SUPPORT.md](SUPPORT.md) | Where to ask for help |
| [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) | Community code of conduct |
| [LICENSE](LICENSE) | MIT license |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | Licenses of bundled and depended-on code |
| [CMakeLists.txt](CMakeLists.txt) | ESP-IDF project file: the `COMPONENTS` list and the version string built from `version.txt` |
| [version.txt](version.txt) | Firmware `MAJOR.MINOR.PATCH`; release tags must match it |
| [partitions.csv](partitions.csv) | Flash layout: nvs, otadata, phy_init, two 1920 KB OTA app slots, 2 MB littlefs, 64 KB coredump |
| [sdkconfig.defaults](sdkconfig.defaults) | Project-wide ESP-IDF configuration (target ESP32-S3, rollback enabled, and so on) |
| [dependencies.lock](dependencies.lock) | Exact versions of the registry components (espp pinned at 1.3.6) |
| [suppressions.txt](suppressions.txt) | cppcheck suppressions used by the static analysis job |
| [.clang-format](.clang-format) | C++ formatting rules (format with clang-format 14) |
| [.clangd](.clangd) | Editor language-server settings |
| [.pre-commit-config.yaml](.pre-commit-config.yaml) | Pre-commit hooks (clang-format on commit) |
| [.gitattributes](.gitattributes) | Line-ending normalization (LF in the repository) |
| [.git-blame-ignore-revs](.git-blame-ignore-revs) | Bulk formatting commits for `git blame` to skip |
| [.gitignore](.gitignore) | Ignored build output and local files |

## Firmware

### `main/`

The application entry point. It brings up the sensor, loads calibration,
starts USB, and runs the polling loop that drives the control core.

| Path | What it is |
|---|---|
| [main/main.cpp](main/main.cpp) | Sensor bring-up, calibration, USB setup, console wiring, and thin adapters around `omega_control` |
| [main/CMakeLists.txt](main/CMakeLists.txt) | Component registration, and the littlefs image built from `files/` |
| [main/Kconfig.projbuild](main/Kconfig.projbuild) | `OMEGA_STICK_BOOT_FLASH_WINDOW_S`, the optional boot delay before USB starts |
| [main/idf_component.yml](main/idf_component.yml) | Registry dependencies (espp components, TinyUSB) |

### `components/`

Local ESP-IDF components. A new one must also be added to `COMPONENTS` in the
top-level `CMakeLists.txt`.

| Path | What it is |
|---|---|
| [components/tmag5273/](components/tmag5273/) | Driver for the TMAG5273 Hall-effect sensor |
| [components/hid-rp-mouse/](components/hid-rp-mouse/) | HID mouse report descriptor and input report |
| [components/xac/](components/xac/) | Xbox Adaptive Controller gamepad report |
| [components/omega_control/](components/omega_control/) | Pure, host-tested control-loop logic: nudge clicks, drift correction, sensor faults, the output hold during updates, and the recalibration gate |
| [components/omega_calibration/](components/omega_calibration/) | Calibration maths, persisted settings, the USB console protocol, the firmware health check, the reset log, and the browser console. See its [README](components/omega_calibration/README.md) |

Inside `components/omega_calibration/`:

| Path | What it is |
|---|---|
| `include/omega_calibration.hpp` | Center and range calibration maths, and the deadzone limits |
| `include/omega_store.hpp` | `OmegaStore`: calibration and settings files on littlefs |
| `include/calibration_service.hpp` | `CalibrationService`: the framed request/reply protocol the console talks to, including firmware updates |
| `include/firmware_health_check.hpp` | Confirms a newly installed image, or rolls it back |
| `include/reset_log.hpp` | Persistent log of recent resets, used for crash-loop detection |
| `web/calibration_console.html` | The calibration console: one standalone page, opened from disk or from the Pages site |
| `web/omega_protocol.js` | The console's pure protocol and update logic. A verbatim copy lives inside the HTML |

### `files/`

Source directory for the littlefs image flashed with the app. It is empty
apart from `.gitkeep`; calibration and settings files are written at runtime.

## Web and releases

| Path | What it is |
|---|---|
| [site/](site/) | GitHub Pages site published with each full release: `index.html` (landing page), `flash.html` (first flash and recovery with ESP Web Tools), `site.css` |
| [scripts/release_feed.mjs](scripts/release_feed.mjs) | Builds `latest.json`, `manifest.json` and `SHA256SUMS` for the Pages site |

The Pages site itself is assembled in CI. It also carries a copy of the
console and the firmware files; see
[docs/firmware-updates.md](docs/firmware-updates.md#releases-and-the-pages-site).

## Tests

| Path | What it is |
|---|---|
| [tests/host/](tests/host/) | Desktop C++ unit tests that build the real espp code (no hardware). Stubs exist only for the OTA headers, in `stubs/`. See its [README](tests/host/README.md) |
| [tests/web/](tests/web/) | Node tests for the console's protocol, update and release-feed logic, and the check that the HTML's inline copy matches `omega_protocol.js`. See its [README](tests/web/README.md) |
| [tests/protocol/golden_frames.json](tests/protocol/golden_frames.json) | Frames generated by the C++ service and checked by both the host and the web tests |
| [tests/fuzz/](tests/fuzz/) | libFuzzer harness for the frame parser, with a seed corpus in `corpus/`. See its [README](tests/fuzz/README.md) |

## Documentation

| Path | What it is |
|---|---|
| [docs/PRD.md](docs/PRD.md) | Product requirements document |
| [docs/requirements.md](docs/requirements.md) | Numbered requirements |
| [docs/decisions.md](docs/decisions.md) | Decision log (D1 onward) with the reasoning behind each choice |
| [docs/firmware-updates.md](docs/firmware-updates.md) | Update paths, the console update flow, rollback, versioning, releases and the Pages site |
| [docs/status.md](docs/status.md) | What is implemented, what is tested, and what is still open |
| [docs/compliance.md](docs/compliance.md) | Regulatory and compliance notes |
| [docs/research/](docs/research/) | Point-in-time research notes. Not kept up to date |

## Hardware and brand

| Path | What it is |
|---|---|
| [models/](models/) | STL files for the printed parts: joystick, magnet and sensor holders, QT Py holder, sockets, topper |
| [assets/logo/](assets/logo/) | Logo, wordmark and scene artwork, with the scripts that generate them. See its [README](assets/logo/README.md) |

## GitHub configuration

| Path | What it is |
|---|---|
| [.github/workflows/build.yml](.github/workflows/build.yml) | Firmware build on pull requests |
| [.github/workflows/package_main.yml](.github/workflows/package_main.yml) | Release packaging: build artifacts, packaged programmers, the merged full-flash image, and the Pages deploy |
| [.github/workflows/size_delta.yml](.github/workflows/size_delta.yml) | Firmware size report on pull requests |
| [.github/workflows/static_analysis.yml](.github/workflows/static_analysis.yml) | cppcheck |
| [.github/workflows/security.yml](.github/workflows/security.yml) | gitleaks secret scan and zizmor workflow audit |
| [.github/workflows/host_tests.yml](.github/workflows/host_tests.yml) | Host tests with gcc, clang (sanitizers) and MSVC, on pull requests |
| [.github/workflows/web_tests.yml](.github/workflows/web_tests.yml) | Node tests for the console |
| [.github/workflows/fuzz.yml](.github/workflows/fuzz.yml) | Frame parser fuzzing |
| [.github/ISSUE_TEMPLATE/](.github/ISSUE_TEMPLATE/) | Bug report, feature request and accessibility feedback forms |
| [.github/pull_request_template.md](.github/pull_request_template.md) | Pull request checklist |
| [.github/CODEOWNERS](.github/CODEOWNERS) | Default reviewers |
| [.github/dependabot.yml](.github/dependabot.yml) | Dependency update schedule for GitHub Actions |
