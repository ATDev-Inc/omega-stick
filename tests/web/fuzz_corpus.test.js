"use strict";
// The golden-derived seeds in tests/fuzz/corpus/ (request_*.bin, reply_*.bin)
// must match tests/protocol/golden_frames.json. Seeds with other names are
// hand-added (crash regressions) and are not checked here. Fix a failure with
//   node scripts/fuzz_seeds.mjs

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { pathToFileURL } = require("node:url");

const root = path.join(__dirname, "..", "..");
const corpusDir = path.join(root, "tests", "fuzz", "corpus");
const golden = JSON.parse(fs.readFileSync(path.join(root, "tests", "protocol", "golden_frames.json"), "utf8"));

const HINT = "Run `node scripts/fuzz_seeds.mjs` and commit the result.";

const expected = new Map();
for (const [key, prefix] of [["requests", "request_"], ["replies", "reply_"]]) {
  for (const e of golden[key]) expected.set(`${prefix}${e.name.toLowerCase()}.bin`, Buffer.from(e.frame_hex, "hex"));
}
const onDisk = fs.readdirSync(corpusDir).filter((n) => /^(request|reply)_[a-z0-9_]+\.bin$/.test(n));

test("every golden frame has a fuzz seed", () => {
  const missing = [...expected.keys()].filter((n) => !onDisk.includes(n));
  assert.deepEqual(missing, [], `missing fuzz seeds. ${HINT}`);
});

test("no stale golden-derived fuzz seeds", () => {
  const extra = onDisk.filter((n) => !expected.has(n));
  assert.deepEqual(extra, [], `fuzz seeds with no golden frame (request_/reply_ names are reserved for generated seeds). ${HINT}`);
});

test("fuzz seeds are regular files with the golden bytes", () => {
  for (const [name, bytes] of expected) {
    const file = path.join(corpusDir, name);
    let st;
    try {
      st = fs.lstatSync(file);
    } catch (e) {
      assert.fail(`${name} is missing (${e.code}). ${HINT}`);
    }
    assert.ok(st.isFile(), `${name} is not a regular file (a link or directory). ${HINT}`);
    let have;
    try {
      have = fs.readFileSync(file);
    } catch (e) {
      assert.fail(`${name} cannot be read (${e.code}). ${HINT}`);
    }
    assert.ok(have.equals(bytes), `${name} differs from golden_frames.json. ${HINT}`);
  }
});

test("the seed generator refuses golden names that are not plain file names", async () => {
  const { expectedSeeds } = await import(pathToFileURL(path.join(root, "scripts", "fuzz_seeds.mjs")).href);
  assert.equal(expectedSeeds(golden).size, expected.size);
  for (const bad of ["../evil", "a/b", "a\\b", "..", "x.y", "UP PER", "", undefined, 7, null]) {
    const g = { requests: [{ name: bad, frame_hex: "00" }], replies: [] };
    assert.throws(() => expectedSeeds(g), /valid seed file name/, `name ${JSON.stringify(bad)} must be refused`);
  }
  const missing = { requests: [{ frame_hex: "00" }], replies: [] };
  assert.throws(() => expectedSeeds(missing), /valid seed file name/, "a missing name must be refused");
  const twice = { requests: [{ name: "A", frame_hex: "00" }, { name: "a", frame_hex: "01" }], replies: [] };
  assert.throws(() => expectedSeeds(twice), /valid seed file name/, "a repeated name must be refused");
});
