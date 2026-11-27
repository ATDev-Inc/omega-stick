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
  `parseDiscovery` (including v2 records with the protocol id),
  `selectCalibrationModule` (service found by protocol id on any module id,
  discovery v1 falls back to module 5, version mismatch flagged) and
  `decodeState`. Expected bytes are derived by hand from
  `stream_frame.hpp`, `dispatcher.hpp` and `calibration_service.hpp`, with CRCs
  cross-checked against Node's `zlib.crc32`.
- `golden.test.js`: checks the JS encoders, `FrameParser` and decoders against
  `tests/protocol/golden_frames.json`, which the C++ host test
  `test_golden_frames.cpp` generates from the real service. Regenerate it
  from C++ with `OMEGA_WRITE_GOLDEN=1 build-host/omega_host_tests`, never by hand,
  then run `node scripts/fuzz_seeds.mjs` to refresh the fuzz seeds and commit both.
- `fuzz_corpus.test.js`: the golden-derived seeds in `tests/fuzz/corpus/`
  (`request_*.bin`, `reply_*.bin`) match `golden_frames.json`; it fails when they
  are out of sync (fix with `node scripts/fuzz_seeds.mjs`).
- `update.test.js`: the pure firmware-update logic in `omega_protocol.js`
  (`inspectImage`, `compareFirmware`, `buildFlashPlan`, `buildRevertPlan`,
  `decideOutcome`, `describeUpdateError`, `formatProgress`), the post-update
  confirm checks (`CONFIRM_CHECKS`, `judgeConfirmCheck`, the sensor wait
  `awaitSensorHealthy` with a fake clock,
  `confirmFailureNotice`: a version or build mismatch, a failed GET_STATE or
  a sensor fault must stop MARK_VALID, and a device without diagnostics skips
  the sensor check), and
  `pickReconnectCandidate`, which chooses the stick to reopen after a reboot
  (exact saved port first, then a unique VID/PID or USB serial match, and
  never a guess between several sticks). Test images are
  built byte by byte following `esp_image_header_t` and `esp_app_desc_t`.
- `latest.test.js`: the release-feed side of "Check for latest firmware"
  (`feedUrlFor`, `parseReleaseFeed`, `summarizeAvailability`), including a
  round trip through the feed that `scripts/release_feed.mjs` writes.
- `offline.test.js`: when the console registers its service worker
  (`shouldRegisterConsoleSw`), which requests `site/console-sw.js` handles,
  and the worker's install, activate and fetch handlers run against a fake
  service worker environment (other sites' caches survive activation; a failed
  cache write still returns the fresh page).
- `save_copy.test.js`: the console's "Save a copy of this page" button
  (`canSaveCopy`, `saveCopyUrl`, and `looksLikeConsole`, which the real
  `calibration_console.html` must pass and error or captive-portal pages must
  fail; `fetchBytesWithDeadline`, which must abort a response whose body
  stalls; and the wording for a started download, which must not claim the
  file was saved).
- `release_feed.test.js`: `scripts/release_feed.mjs`, which builds
  `latest.json`, `manifest.json` and `SHA256SUMS` for the Pages site.
- `sim.test.js`: the simulator helpers in `omega_protocol.js`
  (`encodeSimMessage`, `parseSimMessage`, `validateSimScript` against the rules
  in `docs/simulator.md`, `createHidAccumulator`).
- `mock_sim.mjs` and `mock_sim.test.js`: a small stand-in for `omega_sim` (WebSocket
  server on 127.0.0.1, Node built-ins only) used to try the console's Simulator
  connection in a browser without the real simulator. It answers GET_FW_INFO
  (an upload leaves the image pending), GET_DIAGNOSTICS (with a settable
  sensor fault, or an unsupported error) and MARK_VALID. Run it with
  `node tests/web/mock_sim.mjs [port]`; the test is a quick smoke test of it.
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
