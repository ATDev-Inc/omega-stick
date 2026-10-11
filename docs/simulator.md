# Simulator

`omega_sim` is a desktop program that runs the Omega Stick firmware logic
around a simulated sensor, so the calibration console, calibration, settings,
mode switching, diagnostics and firmware updates can be exercised with nothing
plugged in.

It runs the real code, not an imitation of it: `CalibrationService` and the
espp `Dispatcher`, `OmegaStore`, the calibration sweep (`OmegaCalibration`),
the `omega_control` loop, `FirmwareHealthCheck`, `ResetLog`, and the
`TMAG5273` driver reading a fake I2C bus. What it replaces is only what needs
hardware: USB (a localhost WebSocket instead), the HID output (reported to the
page instead of moving your cursor), flash partitions (a two-slot fake), and
restarts (the firmware state is rebuilt in the same process).

Nothing here proves the hardware works. The simulator is for the logic and the
console; see [docs/status.md](status.md) for what has been tried on a device.

## Running it

```
omega_sim [--port 8765] [--data DIR] [--fresh] [--mode mouse|xac]
          [--script FILE] [--headless] [--speed N]
```

| Option | Meaning |
|---|---|
| `--port N` | WebSocket port on 127.0.0.1 (default 8765) |
| `--data DIR` | Directory that holds the simulated flash: calibration, settings, reset log, OTA slot state (default `./omega_sim_data`) |
| `--fresh` | Erase `--data` first, like a brand-new board (forces the first-boot calibration sweep) |
| `--mode` | Input mode to start in when there is no saved setting |
| `--script FILE` | Play a script (format below) from boot |
| `--headless` | No WebSocket; run `--script` on simulated time as fast as possible, print a report, exit 0 if every `expect` passed, 1 otherwise |
| `--speed N` | Interactive mode only: run simulated time N times faster than real time (default 1) |

In the calibration console choose **Simulator**. The page connects to
`ws://127.0.0.1:8765` and from then on behaves as with a real stick, plus a
Simulator panel for moving the stick and injecting faults.

## Time

Interactive mode runs on real time (times `--speed`). Headless mode runs on a
simulated clock: every sleep in the firmware logic (the 5 s calibration settle
delays, the 10 ms / 5 ms loop period) advances the clock instantly, and the
stick is evaluated at the simulated time, so a scenario that takes a minute on
a device finishes in well under a second.

## Simulated sensor

The stick position (x, y), each in -1..1, is turned into a magnetic field and
written into the fake TMAG5273 result registers, together with a 3-bit
conversion counter that advances on every new sample, exactly as the real
driver expects to read it. The model has a center offset, a per-axis gain, a
small amount of Gaussian noise, an optional slow drift of the center, and these
faults:

| Fault | Effect |
|---|---|
| `ok` | Normal |
| `no_response` | Every I2C transaction fails (sensor unplugged) |
| `stuck_counter` | Readings succeed but the conversion counter stops advancing |
| `weak_magnet` | Field strength drops to a fraction of normal |

## WebSocket protocol

One client at a time. A new connection replaces the old one. The server binds
to 127.0.0.1 only.

**Binary messages** carry the device byte stream in both directions, byte for
byte what the CDC (Web Serial) interface carries on a real stick: protocol
frames, plus plain console text such as the calibration prompts. The console
feeds received bytes to the same parser it uses for USB.

**Text messages** are JSON objects with a `type` field.

Page to simulator:

| Message | Meaning |
|---|---|
| `{"type":"hello","client":"console","version":1}` | Sent once after connecting |
| `{"type":"stick","x":0.4,"y":-0.1}` | Move the stick by hand (switches the input source to manual) |
| `{"type":"pattern","name":"circle"}` | Play a built-in pattern (see below) |
| `{"type":"script","script":{...}}` | Play a script (format below) |
| `{"type":"stop"}` | Stop a pattern or script and return to manual with the stick at rest |
| `{"type":"fault","sensor":"no_response"}` | Set the sensor fault (`ok`, `no_response`, `stuck_counter`, `weak_magnet`) |
| `{"type":"drift","rate":0.02}` | Center drift in stick units per minute, 0 for none |
| `{"type":"noise","sigma":0.002}` | Sensor noise, in stick units |
| `{"type":"next_image","behavior":"fail_self_test"}` | How the next firmware image flashed through the console behaves after its restart: `ok`, `fail_self_test` (never confirms, rolls back), `crash_loop` (crashes on boot, reverted) |
| `{"type":"reset"}` | Power-cycle the simulated stick |
| `{"type":"wipe"}` | Erase the simulated flash and power-cycle (a new board) |

Simulator to page:

| Message | Meaning |
|---|---|
| `{"type":"hello","sim":"omega_sim","version":1,"fw":"0.1.0+sim","mode":"mouse","slot":"ota_0"}` | Sent once after connecting |
| `{"type":"hid","mode":"mouse","dx":3,"dy":-1,"wheel":0,"buttons":0}` | Mouse output since the last message: `dx`, `dy`, `wheel` are sums, `buttons` is the latest state (bit 0 left, bit 1 right). At most 30 per second |
| `{"type":"hid","mode":"xac","x":0.25,"y":-0.5}` | Gamepad output, latest value. At most 30 per second |
| `{"type":"sensor","x":0.4,"y":-0.1,"bx":12.3,"by":-3.1,"ok":true,"fault":"ok"}` | Stick position and simulated field in mT. At most 10 per second |
| `{"type":"source","kind":"pattern","name":"circle","t_ms":4200,"done":false}` | What is moving the stick (`manual`, `pattern`, `script`) |
| `{"type":"rebooting","reason":"mode switch"}` | Sent just before the simulated stick restarts and the socket closes. Reasons: `restart`, `mode switch`, `update`, `revert`, `rollback`, `crash`, `reset`, `wipe` |
| `{"type":"error","message":"..."}` | A control message was rejected |

Unknown `type` values are ignored on both sides, so either side can add
messages without breaking the other.

## Built-in patterns

| Name | Motion |
|---|---|
| `rest` | Stick released at center, noise only |
| `circle` | Slow full circle at full deflection, about 8 s per turn, for the range sweep |
| `calibrate` | Released for the center phases, then circles for the range phase, timed to the firmware's calibration prompts |
| `nudge_right`, `nudge_left`, `nudge_up`, `nudge_down` | A short push past the nudge threshold and back, which should produce a click |
| `wander` | Slow random movement around the whole range |

## Scripts

A script is a JSON timeline. The console can upload one, and `--script` plays
one from boot. Times are milliseconds from the start of the script.

```json
{
  "version": 1,
  "name": "first boot calibrates and then moves right",
  "steps": [
    { "at_ms": 0,      "pattern": "calibrate" },
    { "at_ms": 40000,  "expect": { "calibrated": true, "center_max": 0.05 } },
    { "at_ms": 41000,  "stick": [0, 0] },
    { "at_ms": 41500,  "stick": [0.6, 0] },
    { "at_ms": 43000,  "expect": { "mouse_moved": "right" } },
    { "at_ms": 44000,  "action": "recalibrate" },
    { "at_ms": 44100,  "action": "recalibrate", "expect_reply": "busy" }
  ]
}
```

Each step has `at_ms` and exactly one of:

| Key | Meaning |
|---|---|
| `stick: [x, y]` | Keyframe. The stick moves linearly between consecutive keyframes |
| `pattern: name` | Play a built-in pattern from this time (optional `duration_ms`) |
| `fault`, `drift`, `noise`, `next_image` | Same values as the WebSocket messages |
| `action` | A console request sent through the real protocol: `recalibrate`, `save`, `restart`, `set_mode` (with `value`: `mouse` or `xac`), `set_deadzone` (with `value`: `{"center": 0.1, "range": 0.05}`, either may be omitted). Optional `expect_reply`: `ok`, `busy`, `invalid`, `error` |
| `expect` | Assertions, checked at that time (headless mode reports them; interactive mode logs them) |

`expect` keys:

| Key | Passes when |
|---|---|
| `calibrated: true/false` | A calibration exists and the last sweep succeeded (or not) |
| `center_max: v` | The calibrated center offset is within v of the simulated true center, per axis |
| `mouse_moved: "right"/"left"/"up"/"down"/"none"` | Net mouse motion since the previous `expect` |
| `clicked: "left"/"right"/"none"` | A button press happened since the previous `expect` |
| `mode: "mouse"/"xac"` | Current input mode |
| `sensor_fault: true/false` | The firmware reports a sensor fault |
| `neutral: true` | Every report since the previous `expect` was neutral |
| `fw_slot: "ota_0"/"ota_1"` | The running slot |
| `fw_pending: true/false` | The running image is waiting for its self-test |

Scenarios kept in [sim/scenarios/](../sim/scenarios/) run headless in CI.
