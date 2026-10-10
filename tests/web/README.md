# Web console tests

Node tests (`node:test`, no dependencies, Node 22.2 or newer) for the pure
protocol code of the calibration console.

```
node --test "tests/web/*.test.js"
```

## What is covered

- `protocol.test.js`: CRC-32 vectors, exact `buildFrame` byte layouts, the
  request payload encoders, the streaming `FrameParser` (split chunks, garbage,
  bad CRC, truncated frames, batched frames, correlation ids), `parseErrorReply`,
  `parseDiscovery` and `decodeState`. Expected bytes are derived by hand from
  `stream_frame.hpp`, `dispatcher.hpp` and `calibration_service.hpp`, with CRCs
  cross-checked against Node's `zlib.crc32`.
- `golden.test.js`: checks the JS encoders, `FrameParser` and decoders against
  `tests/protocol/golden_frames.json`, which the C++ host test
  `test_golden_frames.cpp` generates from the real service. Regenerate it
  from C++ with `OMEGA_WRITE_GOLDEN=1 build-host/omega_host_tests`, never by hand.
- `sync.test.js`: keeps the copy of the protocol code inside the HTML identical
  to `omega_protocol.js`, and checks the page only uses exported names.

## Where the code lives

`components/omega_calibration/web/omega_protocol.js` is the source of truth. It
is a classic script (no ES module) that sets `globalThis.OmegaProtocol` and
also exports itself through `module.exports` for these tests.

`calibration_console.html` is meant to be opened as one standalone file from
disk, so the page does not load the `.js` file. Instead a verbatim copy sits
between the `BEGIN omega_protocol.js` and `END omega_protocol.js` markers in
the HTML. After editing `omega_protocol.js`, paste its full contents between
those markers; `sync.test.js` fails until they match.

The DOM, transport and button handlers stay in the HTML and are not covered.
