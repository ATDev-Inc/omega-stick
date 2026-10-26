# Frame fuzzer

A libFuzzer harness (`frame_fuzz.cpp`) for the calibration wire path. Every
input is fed, whole and again in small input-derived chunks, to:

- the espp `stream_frame` parser (magic scan and resync, length limit, CRC,
  optional correlation id),
- the `Dispatcher` (module routing and the discovery module),
- `CalibrationService` with a fixed `OmegaStore` and a send callback that
  discards replies (payload size checks, float validation, store writes,
  `SAVE_TO_FLASH` into a temp directory).

Any crash, ASan/UBSan report, timeout (10 s) or memory blow-up fails the run.

## Run locally

Needs a Clang with libFuzzer (Linux or macOS; the Windows clang build does not
link the fuzzer runtime against the MSVC CRT).

```
cmake -S tests/host -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ -DOMEGA_HOST_SANITIZE=ON -DOMEGA_HOST_FUZZ=ON
cmake --build build-host --target omega_frame_fuzz
build-host/omega_frame_fuzz tests/fuzz/corpus -max_total_time=60 -timeout=10 -rss_limit_mb=2048
```

`OMEGA_HOST_FUZZ` requires `OMEGA_HOST_SANITIZE=ON` and a GNU-style Clang. Run
with a scratch copy of the corpus if you do not want libFuzzer to add files to
`tests/fuzz/corpus`.

## Reproduce a crash

CI uploads `crash-*`, `timeout-*` and `oom-*` files from the failed run. Rerun
the fuzzer binary on one file to replay it:

```
build-host/omega_frame_fuzz crash-<hash>
```

Once fixed, copy the file into `tests/fuzz/corpus/` as a regression seed.

## Seed corpus

`corpus/` holds one file per frame in `tests/protocol/golden_frames.json`
(`request_*.bin` and `reply_*.bin`), written once from that file. They are
only seeds: the fuzzer mutates them, and they need no regeneration when the
golden file changes.
