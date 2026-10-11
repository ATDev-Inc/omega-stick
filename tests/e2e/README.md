# Simulator and console end-to-end check

`sim_console.mjs` drives the real `omega_sim` through the real calibration
console in headless Chrome, using the Chrome DevTools Protocol. It is a manual
developer check and is not part of CI.

It exercises: connecting and the hello, the first-boot calibration prompts, the
`calibrate` pattern, the virtual stick (keyboard and pointer, right and up), a
nudge click, re-running calibration from the console, a deadzone override
(accepted in range, refused out of range), the switch to XAC mode with the
automatic reconnect, a `no_response` sensor fault and recovery, a firmware
update with confirmation, a revert, and an image that fails its self-test and is
rolled back. It also fails on any script error in the page or any crash line in
the simulator output.

## Running it

Build the simulator first (from the repository root):

```
cmake -S tests/host -B build-host -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
```

Then, with Node 24 and Chrome installed:

```
node tests/e2e/sim_console.mjs
```

It takes about one minute, prints one PASS or FAIL line with its evidence per
check, and exits 1 if any check fails.

| Option | Meaning |
|---|---|
| `--sim PATH` | The `omega_sim` executable (default `build-host/sim/omega_sim[.exe]`) |
| `--chrome PATH` | The Chrome executable (default: the usual install path for the platform) |
| `--speed N` | Simulator speed factor (default 4) |
| `--verbose` | Also print the console log and the simulator output at the end, or on failure the tail of both |

Notes:

- Only Node and Chrome are needed: the script has no dependencies, a small CDP
  client is in `cdp.mjs`.
- A wrapper copy of the console page is written to a scratch directory only to
  preset the simulator address and to collect script errors. Every action goes
  through the page's own buttons, fields and keyboard or pointer events.
- The firmware update check waits for each new image to confirm itself (a 10 s
  health window) before the next step, as the real flow does.
