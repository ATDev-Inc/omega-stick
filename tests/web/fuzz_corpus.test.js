"use strict";
// The golden-derived seeds in tests/fuzz/corpus/ (request_*.bin, reply_*.bin)
// must match tests/protocol/golden_frames.json. Seeds with other names are
// hand-added (crash regressions) and are not checked here. Fix a failure with
//   node scripts/fuzz_seeds.mjs

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const root = path.join(__dirname, "..", "..");
const corpusDir = path.join(root, "tests", "fuzz", "corpus");
const golden = JSON.parse(fs.readFileSync(path.join(root, "tests", "protocol", "golden_frames.json"), "utf8"));

const HINT = "Run `node scripts/fuzz_seeds.mjs` and commit the result.";

const expected = new Map();
for (const [key, prefix] of [["requests", "request_"], ["replies", "reply_"]]) {
  for (const e of golden[key]) expected.set(`${prefix}${e.name.toLowerCase()}.bin`, Buffer.from(e.frame_hex, "hex"));
}
const onDisk = fs.readdirSync(corpusDir).filter((n) => /^(request_|reply_).*\.bin$/.test(n));

test("every golden frame has a fuzz seed", () => {
  const missing = [...expected.keys()].filter((n) => !onDisk.includes(n));
  assert.deepEqual(missing, [], `missing fuzz seeds. ${HINT}`);
});

test("no stale golden-derived fuzz seeds", () => {
  const extra = onDisk.filter((n) => !expected.has(n));
  assert.deepEqual(extra, [], `fuzz seeds with no golden frame (request_/reply_ names are reserved for generated seeds). ${HINT}`);
});

test("fuzz seed bytes match the golden frames", () => {
  for (const [name, bytes] of expected) {
    const file = path.join(corpusDir, name);
    if (!fs.existsSync(file)) continue; // reported by the missing test
    assert.ok(fs.readFileSync(file).equals(bytes), `${name} differs from golden_frames.json. ${HINT}`);
  }
});
