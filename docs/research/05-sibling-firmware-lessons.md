# 05: Lessons from sibling firmware

Captured 2026-10-10. See [README](README.md) for method and caveats.

**Question asked:** What do the other ATDev and RAMMP firmware projects do that
Omega Stick should copy, and what should it avoid?

**Method:** Read-only survey of fifteen firmware repositories across the
ATDev-Inc and rammp-org organizations, compared against this repository on
`owen-updates` (merged: OTA rollback in PR #19, the test suite and control core in PR #20,
and this research in PR #21). Several
of the surveyed repositories are private, so this note describes their practices
without citing their files. File paths are given only for public repositories.

---

## Headline finding

**Omega Stick is already ahead of every sibling project on tests, CI, OTA, and
decision records.** None of the other projects has host unit tests, fuzzing, an
OTA rollback path, or a decision log. The lessons worth taking are about
**failing safe when the sensor misbehaves** and **diagnosing field failures**,
where the motor-control and medical-device projects have thought harder than we
have.

> **One gap is confirmed, not hypothetical.** When a TMAG5273 read fails, the
> `get_values` callback in `main/main.cpp` returns `false`, and
> `espp::Joystick::update()` returns early without changing its output. The
> joystick therefore holds its last position. If the sensor drops off the I2C
> bus while the stick is deflected, the host keeps receiving that deflection
> indefinitely: a cursor that runs away, or a character that keeps walking. A
> user with limited strength may not be able to unplug the device to stop it.

---

## Recommendations, ranked

### 1. Fail to neutral on sensor faults

Every motor-control project in the survey treats "no fresh, valid input" as a
fault that drives the output to a safe state. Omega Stick's safe state is a
centered stick with no buttons pressed.

- **Trip after N consecutive failures, not one.** `rammp-org/pace-racer-fw`
  (`commanded-current/main/commanded_current.cpp`) trips only after three
  consecutive over-limit samples, so a single glitch is ignored, then latches
  the fault in an atomic flag. Apply the same rule to TMAG5273 read errors.
- **Detect a stuck sensor, not just a failed read.** An internal ATDev
  device library flags a sensor whose reading has not changed at all for N
  samples, and one whose last update is too old. A TMAG5273 that returns the
  same raw counts for seconds is almost certainly wedged; real hall readings
  always carry noise.
- **Send neutral once on the transition, then recover deliberately.**
  `rammp-org/prototype-fw` (`holo_deck_mib/main/holo_deck_controller.hpp`)
  sends its zero command exactly once on the enabled-to-disabled edge, and
  issues every output from a single loop so a stop can never be overtaken by
  a command from another task. Resume reporting only after M good reads.
- **Latch and report the fault.** Record which check tripped at the moment it
  tripped, and expose it on the calibration console so a clinician or user can
  see why the stick went dead.

**Done** as D21 in [decisions.md](../decisions.md) and
`OmegaCalibration::SensorFaultMonitor`, with host tests. The console and LED
reporting of the latched fault is still open (STA-1, STA-2).

### 2. Persist a reset-reason log and boot counter

An internal ATDev medical device keeps a boot counter and a ring of the last N
reset reasons (`esp_reset_reason()`) in flash, prints it at boot, and flags
panic and watchdog resets. Omega Stick already captures core dumps, but a core
dump only covers panics. Brownouts, watchdog resets, and USB-triggered resets
leave no trace today.

This pairs with the OTA rollback work: the post-update health check can refuse
to mark an image valid if the reset log shows it crashed, and the same log can
be exposed through the coredump or calibration console.

### 3. Tighten dependency floors in `main/idf_component.yml`

The manifest still carries the template's `idf: '>=4.1.0'`, and every espp
component is `'>=1.0'`. `dependencies.lock` pins the exact versions, so builds
are reproducible today, but the manifest no longer says what the code actually
needs. A lock refresh could silently accept a version the firmware cannot use.
Known real floors: the IDF version CI builds with, and `espp/hid-rp >= 1.3.6`
for the 8-bit axis fix the XAC descriptor depends on.

### 4. Harden CI the way ATOS does

The ATOS repository pins every GitHub Action to a commit SHA with a version
comment, keeps them current with Dependabot, and runs a secret scan (gitleaks)
plus workflow linting (zizmor) on push, on PRs, and on a daily schedule.
Omega Stick pins actions by tag, and `esp-cpp/StaticAnalysis@master` floats on
a branch. Because this repository is public and ships firmware to users, a
compromised action tag is a supply chain risk worth closing.

### 5. Write the OTA safety rules down before shipping rollback

The ATOS OTA design study is the most thorough treatment of OTA in the survey,
even though ATOS has not built it yet. Its rules map directly onto
the merged OTA rollback work (PR #19):

- Only apply an update when the device is in a safe state. For Omega Stick,
  that means the host has stopped sending input and the output is neutral.
- Mark the new image valid only after a self-test passes; otherwise roll back.
- **Defer anti-rollback (`secure_version` eFuses) until the release pipeline
  manages version bumps.** Burning eFuses before that can brick devices on a
  legitimate downgrade.
- Record every deferred security control (secure boot, flash encryption,
  anti-rollback) with the date, the reason, and a "revisit when" trigger.
  ATOS keeps these in a dedicated register; a section in
  [decisions.md](../decisions.md) would do the same job here.

### 6. Smaller items

- **`CONFIG_COMPILER_STACK_CHECK_MODE_STRONG=y`.** ATOS enables it. Measure
  the HID report loop timing before and after; the cost is usually small.
- **Rationale comments in `sdkconfig.defaults`.** `rammp-org/pace-hmi-fw`
  explains every non-default setting next to the line that sets it.
- **Generate host constants from one source.** `rammp-org/pace-hmi-fw` parses
  its C wire-spec header (`main/rammp_rtps_spec.h`) from its Python host tool
  (`scripts/rammp_rtps.py`) so the two cannot drift, and the host tool has a
  `--self-test` mode. The merged test suite (PR #20)'s golden frames already catch most
  drift; generating the console's message type table from the C++ header would
  close the rest.
- **Bench logging.** A plain serial-to-CSV logger for raw TMAG5273 counts
  would make drift and noise characterization repeatable. Two internal test
  rigs use exactly this.
- **Dated evidence reports.** `rammp-org/pace-racer-fw`
  (`components/pace-racer-board/ETHERNET_FOC_COEXISTENCE.md`) records date,
  branch, hardware, and status at the top of each measurement report and marks
  superseded results with a banner instead of deleting them.

---

## Already covered here

| Practice seen elsewhere | Omega Stick status |
| --- | --- |
| Core dump partition and retrieval tooling | Done: `coredump` partition, `espp/coredump`, web console |
| PR binary size delta | Done: `size_delta.yml` |
| Framed protocol with magic and CRC | Done: espp "OT" framing with CRC-32, shared by all consoles |
| Release bundle for people who cannot build | Done: `package_main.yml` with the packaged programmer |
| Committed `dependencies.lock` | Done |
| clang-format 14 via pre-commit, cppcheck suppressions | Done |
| Versioned persisted settings | Covered: `key=value` files load partial and older files cleanly. Add a version key only if a key ever changes meaning. |

## Do not copy

- **Erasing NVS on a version mismatch.** Several template-derived projects do
  this on `ESP_ERR_NVS_NEW_VERSION_FOUND`. It would wipe a user's calibration
  after an IDF upgrade. Migrate instead.
- **Committed binaries, logs, and bulk tuning CSVs.** Build output belongs in
  CI artifacts, not git.
- **`__DATE__`-based version strings.** They make builds non-reproducible.
  Derive the version from git, as the merged OTA rollback work (PR #19) already does.
- **Monolithic `main.cpp` files of several thousand lines.** Two sibling
  projects show where that leads; keep logic in components that host tests can
  reach.
- **Key material in the repository**, including "template" or example keys.
