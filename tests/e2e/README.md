# Simulator and console end-to-end check

`sim_console.mjs` drives the real `omega_sim` through the real calibration
console in headless Chrome, using the Chrome DevTools Protocol. It runs in CI
([e2e_tests.yml](../../.github/workflows/e2e_tests.yml), job `sim-console-e2e`)
and can be run by hand.

It exercises: connecting and the hello, the first-boot calibration prompts, the
`calibrate` pattern, the virtual stick (keyboard and pointer, right and up), a
nudge click, re-running calibration from the console, a deadzone override
(accepted in range, refused out of range), the switch to XAC mode with the
automatic reconnect, a `no_response` sensor fault and recovery, a firmware
update that the console checks and confirms, a revert, and an image whose
sensor fails, which the console refuses to confirm and the simulator rolls back
at the deadline, and an image whose sensor starts polling late, which the console
waits for before confirming, and a simulator started with `--no-ota`, for which the console hides its update controls and shows the no-updates note. It also fails on any script error in the page or any crash line
in the simulator output.

## Running it

Build the simulator first (from the repository root):

```
cmake -S sim -B build-sim
cmake --build build-sim
```

Then, with Node 24 and Chrome installed:

```
node tests/e2e/sim_console.mjs --sim build-sim/omega_sim
```

It takes about one minute, prints one PASS or FAIL line with its evidence per
check, and exits 1 if any check fails.

| Option | Meaning |
|---|---|
| `--sim PATH` | The `omega_sim` executable (default `build-host/sim/omega_sim[.exe]`; the build above puts it at `build-sim/omega_sim[.exe]`) |
| `--chrome PATH` | The Chrome executable (default: the usual install path for the platform) |
| `--no-sandbox` | Run Chrome with `--no-sandbox`. CI needs it: the runner's Chrome sandbox can be refused |
| `--speed N` | Simulator speed factor (default 4) |
| `--verbose` | Also print the console log and the simulator output at the end, or on failure the tail of both |

Notes:

- In CI the job uses the runner's `google-chrome`, Node 24 and the simulator
  built as above, at `--speed 1` (the late-sensor check needs the console to
  reconnect inside the simulator's 3 s window, which a loaded runner can miss
  at speed 4; the run takes a few minutes), and uploads the output (`--verbose`) as the `e2e-log`
  artifact on failure. The page is loaded from a local file, so no web server
  is needed.

- Only Node and Chrome are needed: the script has no dependencies, a small CDP
  client is in `cdp.mjs`.
- A wrapper copy of the console page is written to a scratch directory only to
  preset the simulator address and to collect script errors. Every action goes
  through the page's own buttons, fields and keyboard or pointer events.
- The firmware update check waits for the console to run its checks and confirm
  each new image before the next step, as the real flow does. The unconfirmed
  image is rolled back by the simulator's 120 s deadline, which the default
  speed factor of 4 brings down to about 30 s.
