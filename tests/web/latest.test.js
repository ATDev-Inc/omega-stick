"use strict";
// Latest-release feed: URL choice, strict validation, availability copy, and a
// round trip against the real feed builder in scripts/release_feed.mjs.

const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");
const { pathToFileURL } = require("node:url");

const P = require(path.join(__dirname, "..", "..", "components", "omega_calibration", "web", "omega_protocol.js"));

const FEED = "https://atdev-inc.github.io/omega-stick/firmware/latest.json";
const SHA = "a".repeat(64);
function feed(over) {
  return Object.assign({
    schema: 1, project: "omega_stick", version: "0.2.0", tag: "v0.2.0",
    published_at: "2026-10-11T12:00:00.000Z",
    notes_url: "https://github.com/ATDev-Inc/omega-stick/releases/tag/v0.2.0",
    app: { path: "omega_stick-0.2.0.bin", size: 1000, sha256: SHA },
    full: { path: "omega_stick-0.2.0-full.bin", size: 2000, sha256: SHA },
    chip: "esp32s3",
  }, over);
}

test("feedUrlFor: same-origin on the project Pages site, published URL elsewhere", () => {
  assert.equal(P.FEED_URL, FEED);
  assert.equal(P.feedUrlFor("https://atdev-inc.github.io/omega-stick/console/"), FEED);
  assert.equal(P.feedUrlFor("https://atdev-inc.github.io/omega-stick/console/index.html?x=1#y"), FEED);
  assert.equal(P.feedUrlFor("file:///C:/x/calibration_console.html"), FEED);
  assert.equal(P.feedUrlFor("http://localhost:8000/console/"), FEED);
  assert.equal(P.feedUrlFor("https://atdev-inc.github.io/other/console/"), FEED);
  assert.equal(P.feedUrlFor("garbage"), FEED);
  // a query string can never redirect the feed
  assert.equal(P.feedUrlFor("https://atdev-inc.github.io/omega-stick/console/?feed=https://evil.example/x.json"), FEED);
});

test("parseReleaseFeed accepts a good feed and resolves the image URL", () => {
  const r = P.parseReleaseFeed(feed(), FEED);
  assert.equal(r.ok, true);
  assert.equal(r.release.version, "0.2.0");
  assert.equal(r.release.appUrl, "https://atdev-inc.github.io/omega-stick/firmware/omega_stick-0.2.0.bin");
  assert.equal(r.release.size, 1000);
  assert.equal(r.release.sha256, SHA);
  assert.equal(r.release.fileName, "omega_stick-0.2.0.bin");
  assert.equal(r.release.notesUrl, "https://github.com/ATDev-Inc/omega-stick/releases/tag/v0.2.0");
});

test("parseReleaseFeed rejects bad feeds", () => {
  const cases = {
    "not an object": null,
    "array": [],
    "schema 2": feed({ schema: 2 }),
    "schema string": feed({ schema: "1" }),
    "wrong project": feed({ project: "other" }),
    "wrong chip": feed({ chip: "esp32" }),
    "version 2 parts": feed({ version: "0.2", tag: "v0.2" }),
    "version suffix": feed({ version: "0.2.0+abc", tag: "v0.2.0+abc" }),
    "version leading v": feed({ version: "v0.2.0" }),
    "tag mismatch": feed({ tag: "v0.2.1" }),
    "no app": feed({ app: undefined }),
    "size zero": feed({ app: { path: "a.bin", size: 0, sha256: SHA } }),
    "size too big": feed({ app: { path: "a.bin", size: P.OTA_SLOT_SIZE + 1, sha256: SHA } }),
    "size float": feed({ app: { path: "a.bin", size: 1.5, sha256: SHA } }),
    "size string": feed({ app: { path: "a.bin", size: "10", sha256: SHA } }),
    "sha short": feed({ app: { path: "a.bin", size: 10, sha256: "abc" } }),
    "sha upper": feed({ app: { path: "a.bin", size: 10, sha256: "A".repeat(64) } }),
    "path slash": feed({ app: { path: "sub/a.bin", size: 10, sha256: SHA } }),
    "path backslash": feed({ app: { path: "sub\\a.bin", size: 10, sha256: SHA } }),
    "path dotdot": feed({ app: { path: "..a.bin", size: 10, sha256: SHA } }),
    "path parent": feed({ app: { path: "../a.bin", size: 10, sha256: SHA } }),
    "path colon": feed({ app: { path: "https:a.bin", size: 10, sha256: SHA } }),
    "path query": feed({ app: { path: "a.bin?x=1", size: 10, sha256: SHA } }),
    "path empty": feed({ app: { path: "", size: 10, sha256: SHA } }),
  };
  for (const [name, f] of Object.entries(cases)) {
    const r = P.parseReleaseFeed(f, FEED);
    assert.equal(r.ok, false, name);
    assert.equal(r.release, null, name);
    assert.ok(r.error.length > 0, name);
  }
});

test("parseReleaseFeed drops a notes_url outside the project releases (not fatal)", () => {
  for (const u of ["https://evil.example/releases/", "http://github.com/ATDev-Inc/omega-stick/releases/x",
                   "https://github.com/ATDev-Inc/omega-stick/issues/1", undefined, 5]) {
    const r = P.parseReleaseFeed(feed({ notes_url: u }), FEED);
    assert.equal(r.ok, true);
    assert.equal(r.release.notesUrl, "");
  }
});

test("summarizeAvailability states and copy", () => {
  const rel = P.parseReleaseFeed(feed(), FEED).release;
  const s = (version, sha) => P.summarizeAvailability(version === null ? null : { version, sha: sha || "" }, rel);
  let r = s("0.1.0");
  assert.deepEqual([r.state, r.canInstall, r.message], ["update-available", true, "Firmware 0.2.0 is available. You have 0.1.0."]);
  r = s("0.2.0");
  assert.deepEqual([r.state, r.canInstall, r.message], ["up-to-date", false, "You have the latest release (0.2.0)."]);
  r = s("0.3.0");
  assert.equal(r.state, "running-newer"); assert.equal(r.canInstall, false);
  r = s("0.2.0+abc1234");
  assert.equal(r.state, "running-dev-build"); assert.equal(r.canInstall, false);
  assert.equal(r.message, "You are running a development build (0.2.0+abc1234). The latest release is 0.2.0.");
  r = s("0.1.0+abc1234");
  assert.equal(r.state, "update-available");
  r = s("garbage"); assert.equal(r.state, "unknown"); assert.equal(r.canInstall, false);
  r = s(null); assert.equal(r.state, "unknown");
  r = s(""); assert.equal(r.state, "unknown");
});

test("round trip: a feed built by scripts/release_feed.mjs parses", async () => {
  const m = await import(pathToFileURL(path.join(__dirname, "..", "..", "scripts", "release_feed.mjs")).href);
  const bytes = Buffer.from("firmware bytes");
  const built = m.buildLatest({
    version: "1.4.2", tag: "v1.4.2", publishedAt: "2026-10-11T12:00:00.000Z",
    notesUrl: "https://github.com/ATDev-Inc/omega-stick/releases/tag/v1.4.2",
    app: { size: bytes.length, sha256: m.sha256Hex(bytes) },
    full: { size: 99, sha256: m.sha256Hex(Buffer.from("full")) },
  });
  const r = P.parseReleaseFeed(JSON.parse(JSON.stringify(built)), FEED);
  assert.equal(r.ok, true, r.error);
  assert.equal(r.release.version, "1.4.2");
  assert.equal(r.release.size, bytes.length);
  assert.equal(r.release.sha256, m.sha256Hex(bytes));
  assert.equal(r.release.appUrl, "https://atdev-inc.github.io/omega-stick/firmware/" + m.fileNames("1.4.2").app);
  assert.ok(r.release.notesUrl.endsWith("/v1.4.2"));
});
