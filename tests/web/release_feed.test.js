"use strict";
// Tests for scripts/release_feed.mjs (ESM, so loaded with import()).

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { pathToFileURL } = require("node:url");

const modUrl = pathToFileURL(path.join(__dirname, "..", "..", "scripts", "release_feed.mjs")).href;
const load = () => import(modUrl);

const SHA_ABC = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

test("fileNames are versioned", async () => {
  const m = await load();
  assert.deepEqual(m.fileNames("0.2.0"), {
    app: "omega_stick-0.2.0.bin",
    full: "omega_stick-0.2.0-full.bin",
  });
});

test("checkVersion accepts matching tag and rejects the rest", async () => {
  const m = await load();
  m.checkVersion("0.2.0", "v0.2.0");
  assert.throws(() => m.checkVersion("0.2.0", "v0.2.1"), /does not match/);
  assert.throws(() => m.checkVersion("0.2", "v0.2"), /MAJOR.MINOR.PATCH/);
  assert.throws(() => m.checkVersion("0.2.0-rc1", "v0.2.0-rc1"), /MAJOR.MINOR.PATCH/);
});

test("sha256Hex matches the known vector", async () => {
  const m = await load();
  assert.equal(m.sha256Hex(Buffer.from("abc")), SHA_ABC);
});

test("buildLatest has exactly the documented schema", async () => {
  const m = await load();
  const latest = m.buildLatest({
    version: "0.2.0",
    tag: "v0.2.0",
    publishedAt: "2026-10-11T12:00:00.000Z",
    notesUrl: "https://github.com/ATDev-Inc/omega-stick/releases/tag/v0.2.0",
    app: { size: 3, sha256: SHA_ABC },
    full: { size: 9, sha256: "f".repeat(64) },
  });
  assert.deepEqual(latest, {
    schema: 1,
    project: "omega_stick",
    version: "0.2.0",
    tag: "v0.2.0",
    published_at: "2026-10-11T12:00:00.000Z",
    notes_url: "https://github.com/ATDev-Inc/omega-stick/releases/tag/v0.2.0",
    app: { path: "omega_stick-0.2.0.bin", size: 3, sha256: SHA_ABC },
    full: { path: "omega_stick-0.2.0-full.bin", size: 9, sha256: "f".repeat(64) },
    chip: "esp32s3",
  });
});

test("buildManifest writes the merged image at offset 0 and prompts to erase", async () => {
  const m = await load();
  assert.deepEqual(m.buildManifest({ version: "0.2.0" }), {
    name: "Omega Stick",
    version: "0.2.0",
    new_install_prompt_erase: true,
    builds: [
      {
        chipFamily: "ESP32-S3",
        parts: [{ path: "omega_stick-0.2.0-full.bin", offset: 0 }],
      },
    ],
  });
});

test("buildSums is sorted and in sha256sum format", async () => {
  const m = await load();
  const text = m.buildSums([
    { name: "b.bin", sha256: "2".repeat(64) },
    { name: "a.bin", sha256: "1".repeat(64) },
  ]);
  assert.equal(text, `${"1".repeat(64)}  a.bin\n${"2".repeat(64)}  b.bin\n`);
});

test("parseArgs requires every option", async () => {
  const m = await load();
  assert.throws(() => m.parseArgs(["--app", "x"]), /missing --full/);
  assert.throws(() => m.parseArgs(["app"]), /bad argument/);
});

test("run writes the output directory", async () => {
  const m = await load();
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "release-feed-"));
  try {
    fs.writeFileSync(path.join(dir, "app.bin"), "abc");
    fs.writeFileSync(path.join(dir, "full.bin"), "0123456789");
    const out = path.join(dir, "out");
    const { latest } = m.run({
      app: path.join(dir, "app.bin"),
      full: path.join(dir, "full.bin"),
      version: "0.2.0",
      tag: "v0.2.0",
      "notes-url": "https://example.test/notes",
      "published-at": "2026-10-11T12:00:00Z",
      out,
    });
    assert.equal(latest.app.size, 3);
    assert.equal(latest.app.sha256, SHA_ABC);
    assert.equal(latest.full.size, 10);
    assert.equal(latest.published_at, "2026-10-11T12:00:00.000Z");
    assert.deepEqual(fs.readdirSync(out).sort(), [
      "SHA256SUMS",
      "latest.json",
      "manifest.json",
      "omega_stick-0.2.0-full.bin",
      "omega_stick-0.2.0.bin",
    ]);
    assert.equal(fs.readFileSync(path.join(out, "omega_stick-0.2.0.bin"), "utf8"), "abc");
    const onDisk = JSON.parse(fs.readFileSync(path.join(out, "latest.json"), "utf8"));
    assert.deepEqual(onDisk, latest);
    // Every line of SHA256SUMS must verify against the file on disk.
    const sums = fs.readFileSync(path.join(out, "SHA256SUMS"), "utf8").trim().split("\n");
    assert.equal(sums.length, 4);
    for (const line of sums) {
      const [hash, name] = line.split("  ");
      assert.equal(m.sha256Hex(fs.readFileSync(path.join(out, name))), hash, name);
    }
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test("run rejects an empty image, a mismatched tag and a bad date", async () => {
  const m = await load();
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "release-feed-"));
  try {
    fs.writeFileSync(path.join(dir, "app.bin"), "");
    fs.writeFileSync(path.join(dir, "full.bin"), "x");
    const base = {
      app: path.join(dir, "app.bin"),
      full: path.join(dir, "full.bin"),
      version: "0.2.0",
      tag: "v0.2.0",
      "notes-url": "u",
      "published-at": "2026-10-11T12:00:00Z",
      out: path.join(dir, "out"),
    };
    assert.throws(() => m.run(base), /is empty/);
    assert.throws(() => m.run({ ...base, tag: "v9.9.9" }), /does not match/);
    assert.throws(() => m.run({ ...base, "published-at": "nope" }), /not a date/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
