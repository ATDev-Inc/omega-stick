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

## When it runs

[fuzz.yml](../../.github/workflows/fuzz.yml) has a `frame-fuzz` job whose time
budget depends on the trigger:

| Trigger | Budget |
| --- | --- |
| pull request, push to `main` | 60 s |
| nightly (`schedule`, 03:17 UTC) | 10 min |
| manual (`workflow_dispatch`) | the `minutes` input, default 10; values above 60 are capped at 60 |

Between runs the inputs libFuzzer found are kept in the Actions cache
(`frame-fuzz-corpus-<run id>-<attempt>`, restored from the newest earlier
entry). After fuzzing, a non-pull-request run minimizes the evolved directory
with `-merge=1` into a fresh one (keeping only inputs that add coverage) before
saving, so the cache does not grow without bound. The
fuzzer is started with the evolved directory `fuzz-corpus/` first and
`tests/fuzz/corpus` second, so new inputs land in the cache and the committed
seeds are never modified. A cache entry is only readable from its own branch
and the default branch, so pull requests start from what `main` saved. Pull
request runs do not save their own entry.

A failed nightly or manual run on the default branch also opens an issue, see
[Crash issues](#crash-issues).

## Run locally

Needs a Clang with libFuzzer (Linux or macOS; the Windows clang build does not
link the fuzzer runtime against the MSVC CRT).

```
cmake -S tests/host -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ -DOMEGA_HOST_SANITIZE=ON -DOMEGA_HOST_FUZZ=ON
cmake --build build-host --target omega_frame_fuzz
mkdir fuzz-corpus
build-host/omega_frame_fuzz fuzz-corpus tests/fuzz/corpus -max_total_time=60 -timeout=10 -rss_limit_mb=2048
```

`OMEGA_HOST_FUZZ` requires `OMEGA_HOST_SANITIZE=ON` and a GNU-style Clang.
libFuzzer adds new inputs to the first directory, so list a scratch directory
first, as above, to leave `tests/fuzz/corpus` untouched.

## Reproduce a crash

CI uploads `crash-*`, `timeout-*`, `oom-*` and `leak-*` files from the failed
run. Rerun the fuzzer binary on one file to replay it:

```
build-host/omega_frame_fuzz crash-<hash>
```

The harness resets the service and dispatcher parsers before every input, so a
single file reproduces on its own. The shared `OmegaStore` and its temp
directory persist across inputs, so a crash that depends on saved state may need
the evolved corpus as well.

Once fixed, copy the file into `tests/fuzz/corpus/` as a regression seed. Do
not name it `request_*.bin` or `reply_*.bin`: those names belong to the
generated seeds below, and the seed script deletes any such file it does not
recognise.

## Crash issues

The `report` job runs only after a failed `frame-fuzz` job, and only for a
`schedule` run (always the default branch) or a `workflow_dispatch` run whose
ref is the default branch, so a dispatch on a feature branch never opens,
comments on or closes the shared issue. (The branch is not checked for
`schedule`, whose event payload may lack the repository.) It is the only job
with `issues: write`.

- A crash, timeout, oom or leak file (LeakSanitizer is on): it opens `Fuzz crash: frame-fuzz`, or comments
  on the open one, with the run URL, the file name and the input in base64 (when
  it is 4 KB or less; otherwise the input is in the `fuzz-crashes` artifact).
- No such file: it opens or comments on `Fuzz target broken: frame-fuzz` with
  the run URL.

The issue gets the `fuzz` label only if that label already exists.

## Seed corpus

`corpus/` holds one file per frame in `tests/protocol/golden_frames.json`
(`request_<name>.bin` and `reply_<name>.bin`, name in lowercase, the bytes of
`frame_hex`), plus any regression seeds added by hand. The golden-derived files
are generated:

```
node scripts/fuzz_seeds.mjs           # write and prune
node scripts/fuzz_seeds.mjs --check   # report differences only
```

The script only writes and removes plain files named `request_*.bin` or
`reply_*.bin` directly in `tests/fuzz/corpus`, found from the script's own
location, and refuses to run if that directory is not the repository's. After
regenerating the golden file (`OMEGA_WRITE_GOLDEN=1 build-host/omega_host_tests`),
run it and commit the result. `tests/web/fuzz_corpus.test.js` fails when the
corpus and the golden file disagree.

## Replay in ctest

`omega_frame_replay` (always built, any compiler, no libFuzzer or sanitizer
needed) links `frame_fuzz.cpp` with `replay_main.cpp`, which runs every file in
the directories it is given through `LLVMFuzzerTestOneInput`. The
`omega_frame_replay` ctest runs it on `tests/fuzz/corpus`, so every seed,
including hand-added regression seeds, is checked on each host test run. It
fails if a directory is empty or missing, and if a `request_*` seed draws no
reply from the service or the discovery module, which shows the seeds reach the
handlers. The reply counter exists only in that build
(`OMEGA_FUZZ_COUNT_REPLIES`); the libFuzzer build is unchanged.
