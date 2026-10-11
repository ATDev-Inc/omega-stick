"use strict";
// Pure firmware-update logic in omega_protocol.js: image inspection, version
// comparison, flash/revert plans, post-reboot outcome and error wording.
// Images are built byte by byte here, following esp_image_header_t and
// esp_app_desc_t in ESP-IDF.

const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");

const P = require(path.join(__dirname, "..", "..", "components", "omega_calibration", "web", "omega_protocol.js"));

function putStr(buf, off, s) { for (let i = 0; i < s.length; i++) buf[off + i] = s.charCodeAt(i); }

// Builds a complete ESP32 app image: header, one segment holding the app
// descriptor, checksum padding, optional appended SHA-256.
function makeImage(o) {
  o = Object.assign({
    chip: 9, project: "omega_stick", version: "0.2.0", secure: 0, segments: 1,
    dataLen: 256 + 64, hash: true, sha: 0xAB, descMagic: 0xABCD5432,
  }, o || {});
  const segHdrs = o.segments;
  let pos = 24;
  const lens = [];
  for (let i = 0; i < segHdrs; i++) { const len = i === 0 ? o.dataLen : 16; lens.push(len); pos += 8 + len; }
  const total = (pos | 15) + 1 + (o.hash ? 32 : 0);
  const b = new Uint8Array(total);
  const v = new DataView(b.buffer);
  b[0] = 0xE9; b[1] = o.segments; b[2] = 2; b[3] = 0x4F;
  v.setUint32(4, 0x40375000, true);
  v.setUint16(12, o.chip, true);
  v.setUint16(15, 0, true); v.setUint16(17, 99, true);
  b[23] = o.hash ? 1 : 0;
  let p = 24;
  for (let i = 0; i < segHdrs; i++) {
    v.setUint32(p, 0x3C000000 + i * 0x1000, true);
    v.setUint32(p + 4, lens[i], true);
    if (i === 0) {
      const d = p + 8; // 32
      v.setUint32(d, o.descMagic, true);
      v.setUint32(d + 4, o.secure, true);
      putStr(b, d + 16, o.version);
      putStr(b, d + 48, o.project);
      putStr(b, d + 80, "12:34:56");
      putStr(b, d + 96, "Oct 10 2026");
      putStr(b, d + 112, "v6.0.2");
      for (let k = 0; k < 32; k++) b[d + 144 + k] = (o.sha + k) & 0xFF;
    }
    p += 8 + lens[i];
  }
  return b;
}
const codes = (r) => r.errors.map((e) => e.code);
const wcodes = (r) => r.warnings.map((e) => e.code);

test("offsets match ESP-IDF: a hand-checked image parses to its fields", () => {
  const img = makeImage({ version: "0.2.0+abc1234", sha: 0x10 });
  assert.equal(img[0], 0xE9);
  assert.equal(img[12], 9);                 // chip_id
  assert.equal(img[32], 0x32);              // magic_word 0xABCD5432 little endian
  assert.equal(img[35], 0xAB);
  const r = P.inspectImage(img);
  assert.ok(r.ok, JSON.stringify(r.errors));
  assert.equal(r.info.chipId, 9);
  assert.equal(r.info.chipName, "ESP32-S3");
  assert.equal(r.info.segmentCount, 1);
  assert.equal(r.info.hashAppended, true);
  assert.equal(r.info.appDesc.version, "0.2.0+abc1234");
  assert.equal(r.info.appDesc.project, "omega_stick");
  assert.equal(r.info.appDesc.time, "12:34:56");
  assert.equal(r.info.appDesc.date, "Oct 10 2026");
  assert.equal(r.info.appDesc.idf, "v6.0.2");
  assert.equal(r.info.appDesc.sha256.slice(0, 8), "10111213");
  assert.equal(r.info.appDesc.shaShort, "10111213");
  assert.equal(r.info.expectedSize, img.length);
  assert.deepEqual(r.warnings, []);
});

test("image without appended hash, and with several segments, is complete", () => {
  assert.ok(P.inspectImage(makeImage({ hash: false })).ok);
  assert.ok(P.inspectImage(makeImage({ segments: 3 })).ok);
  assert.ok(P.inspectImage(makeImage({ dataLen: 256 + 13 })).ok); // unaligned data length
});

test("empty and tiny files", () => {
  assert.deepEqual(codes(P.inspectImage(new Uint8Array(0))), ["empty"]);
  assert.deepEqual(codes(P.inspectImage(new Uint8Array([0xE9, 1, 2]))), ["truncated"]);
});

test(".elf, partition table and random files are named for what they are", () => {
  const elf = new Uint8Array(100); elf.set([0x7F, 0x45, 0x4C, 0x46]);
  assert.deepEqual(codes(P.inspectImage(elf)), ["elf"]);
  const pt = new Uint8Array(100); pt.set([0xAA, 0x50]);
  assert.deepEqual(codes(P.inspectImage(pt)), ["partition_table"]);
  assert.deepEqual(codes(P.inspectImage(new Uint8Array(100).fill(0x41))), ["not_esp_image"]);
  const badSeg = makeImage(); badSeg[1] = 0;
  assert.deepEqual(codes(P.inspectImage(badSeg)), ["not_esp_image"]);
  badSeg[1] = 17;
  assert.deepEqual(codes(P.inspectImage(badSeg)), ["not_esp_image"]);
});

test("a bootloader style image (no app descriptor) is not an app image", () => {
  const r = P.inspectImage(makeImage({ descMagic: 0 }));
  assert.deepEqual(codes(r), ["not_app_image"]);
  assert.equal(r.info.appDesc, null);
});

test("wrong chip is a hard error that names both chips", () => {
  const r = P.inspectImage(makeImage({ chip: 5 }));
  assert.deepEqual(codes(r), ["wrong_chip"]);
  assert.match(r.errors[0].message, /ESP32-C3/);
  assert.match(r.errors[0].message, /ESP32-S3/);
  assert.deepEqual(codes(P.inspectImage(makeImage({ chip: 77 }))), ["wrong_chip"]);
});

test("project is compared with the device's name, default omega_stick", () => {
  const other = makeImage({ project: "coredump_tool" });
  assert.deepEqual(codes(P.inspectImage(other)), ["wrong_project"]);
  assert.ok(P.inspectImage(other, { expectedProject: "coredump_tool" }).ok);
  assert.deepEqual(codes(P.inspectImage(makeImage(), { expectedProject: "something_else" })), ["wrong_project"]);
});

test("truncated images are caught, including mid-segment and missing hash", () => {
  const img = makeImage();
  assert.deepEqual(codes(P.inspectImage(img.subarray(0, img.length - 1))), ["truncated"]);
  assert.deepEqual(codes(P.inspectImage(img.subarray(0, 100))), ["truncated"]);
  assert.deepEqual(codes(P.inspectImage(img.subarray(0, 30))), ["truncated"]);
  assert.deepEqual(codes(P.inspectImage(img.subarray(0, 24))), ["truncated"]);
  // Header claims a segment far larger than the file.
  const big = makeImage(); new DataView(big.buffer).setUint32(28, 0x7FFFFFFF, true);
  assert.deepEqual(codes(P.inspectImage(big)), ["truncated"]);
});

test("size limit is the 0x1E0000 slot", () => {
  assert.equal(P.OTA_SLOT_SIZE, 0x1E0000);
  const r = P.inspectImage(makeImage(), { slotSize: 256 });
  assert.deepEqual(codes(r), ["too_big"]);
  const huge = new Uint8Array(0x1E0000 + 1); huge.set(makeImage());
  assert.ok(codes(P.inspectImage(huge)).includes("too_big"));
  const exact = makeImage({ dataLen: 0x1E0000 - 24 - 8 - 33 - 15 });
  assert.ok(exact.length <= 0x1E0000);
  assert.ok(P.inspectImage(exact).ok);
});

test("a non-zero secure version is flagged but allowed", () => {
  const r = P.inspectImage(makeImage({ secure: 3 }));
  assert.ok(r.ok);
  assert.deepEqual(wcodes(r), ["secure_version"]);
});

test("parseVersion follows D19 formats", () => {
  assert.deepEqual(
    (({ valid, major, minor, patch, dev, sha, dirty }) => ({ valid, major, minor, patch, dev, sha, dirty }))(P.parseVersion("1.2.3")),
    { valid: true, major: 1, minor: 2, patch: 3, dev: false, sha: "", dirty: false });
  const a = P.parseVersion("0.1.0+abc1234");
  assert.ok(a.valid && a.dev && a.sha === "abc1234" && !a.dirty);
  const b = P.parseVersion("0.1.0+abc1234.dirty");
  assert.ok(b.valid && b.dev && b.dirty);
  const c = P.parseVersion("0.1.0+unknown");
  assert.ok(c.valid && c.dev && c.sha === "unknown");
  for (const bad of ["", "abc", "1.2", "1.2.3.4", "1.2.3-dev", undefined, null]) assert.equal(P.parseVersion(bad).valid, false, String(bad));
});

test("compareFirmware classifies upgrade, downgrade, same, dev and same build", () => {
  const run = { version: "0.1.0", sha: "aaaaaaaa" };
  const k = (v, sha) => P.compareFirmware(run, { version: v, sha: sha || "bbbbbbbb" }).kind;
  assert.equal(k("0.2.0"), "upgrade");
  assert.equal(k("0.1.1"), "upgrade");
  assert.equal(k("1.0.0"), "upgrade");
  assert.equal(k("0.0.9"), "downgrade");
  assert.equal(k("0.1.0"), "same-version");
  assert.equal(k("0.1.0+abc1234"), "dev");
  assert.equal(k("0.1.0+abc1234.dirty"), "dev");
  assert.equal(k("0.10.0"), "upgrade"); // numeric, not lexical
  assert.equal(k("garbage"), "unknown");
  assert.equal(P.compareFirmware({ version: "x", sha: "" }, { version: "0.1.0", sha: "" }).kind, "unknown");
  // Exact build: ELF SHA prefix wins over everything, even a dev suffix.
  assert.equal(k("0.1.0", "AAAAAAAA" + "0".repeat(56)), "same-build");
  assert.equal(P.compareFirmware({ version: "0.2.0+abc.dirty", sha: "1234abcd" },
                                 { version: "0.2.0+abc.dirty", sha: "1234abcd99" }).kind, "same-build");
  // Short or missing SHA is not trusted as proof.
  assert.notEqual(P.compareFirmware({ version: "0.1.0", sha: "aaaa" }, { version: "0.1.0", sha: "aaaa" }).kind, "same-build");
  assert.equal(P.compareFirmware({ version: "0.1.0", sha: "" }, { version: "0.1.0", sha: "" }).kind, "same-version");
  // Dev builds flagged on either side.
  assert.equal(P.compareFirmware({ version: "0.1.0+a.dirty" }, { version: "0.2.0" }).runningDev, true);
  assert.equal(P.compareFirmware({ version: "0.1.0" }, { version: "0.2.0+b" }).candidateDev, true);
});

test("flash plan: upgrade has no warnings", () => {
  const insp = P.inspectImage(makeImage({ version: "0.2.0" }));
  const plan = P.buildFlashPlan(insp, { version: "0.1.0", sha: "11111111" }, { name: "omega_stick.bin", size: 123 });
  assert.equal(plan.blocked, false);
  assert.equal(plan.kind, "upgrade");
  assert.equal(plan.headline, "Running 0.1.0 -> New 0.2.0");
  assert.equal(plan.fileName, "omega_stick.bin");
  assert.equal(plan.size, 123);
  assert.deepEqual(plan.warnings, []);
});

test("flash plan: downgrade, same build, same version and dev build are allowed with warnings", () => {
  const w = (ver, runVer, runSha) => {
    const insp = P.inspectImage(makeImage({ version: ver, sha: 0x20 }));
    const plan = P.buildFlashPlan(insp, { version: runVer, sha: runSha || "ffffffff" }, { name: "a.bin", size: 1 });
    assert.equal(plan.blocked, false);
    return plan;
  };
  let p = w("0.1.0", "0.2.0");
  assert.equal(p.kind, "downgrade");
  assert.deepEqual(wcodes(p), ["downgrade"]);
  assert.match(p.warnings[0].message, /older/);
  p = w("0.2.0", "0.2.0", "20212223");
  assert.equal(p.kind, "same-build");
  assert.deepEqual(wcodes(p), ["same_build"]);
  assert.match(p.warnings[0].message, /already running/);
  p = w("0.2.0", "0.2.0");
  assert.equal(p.kind, "same-version");
  assert.deepEqual(wcodes(p), ["same_version"]);
  p = w("0.2.0+abc1234.dirty", "0.2.0");
  assert.equal(p.kind, "dev");
  assert.deepEqual(wcodes(p), ["dev_build"]);
  p = w("0.3.0+abc1234", "0.2.0");
  assert.equal(p.kind, "upgrade");
  assert.deepEqual(wcodes(p), ["dev_build"]);
});

test("flash plan: hard errors block, and running version unknown only warns", () => {
  const bad = P.buildFlashPlan(P.inspectImage(makeImage({ chip: 5 })), { version: "0.1.0" }, { name: "x.bin", size: 1 });
  assert.equal(bad.blocked, true);
  assert.deepEqual(codes(bad), ["wrong_chip"]);
  const elf = new Uint8Array(50); elf.set([0x7F, 0x45, 0x4C, 0x46]);
  const e = P.buildFlashPlan(P.inspectImage(elf), null, { name: "x.elf", size: 50 });
  assert.equal(e.blocked, true);
  assert.equal(e.headline, "");
  const unk = P.buildFlashPlan(P.inspectImage(makeImage()), null, { name: "x.bin", size: 1 });
  assert.equal(unk.blocked, false);
  assert.equal(unk.headline, "New 0.2.0");
  assert.deepEqual(wcodes(unk), ["running_unknown"]);
});

test("revert plan", () => {
  const ok = P.buildRevertPlan({ version: "0.2.0", sha: "aa" }, { version: "0.1.0", label: "ota_1", state: "valid" });
  assert.equal(ok.blocked, false);
  assert.equal(ok.headline, "Running 0.2.0 -> Previous 0.1.0");
  assert.equal(ok.kind, "downgrade");
  assert.deepEqual(ok.warnings, []);
  const pend = P.buildRevertPlan({ version: "0.2.0" }, { version: "0.1.0", state: "pending_verify" });
  assert.deepEqual(wcodes(pend), ["prev_state"]);
  const newer = P.buildRevertPlan({ version: "0.1.0" }, { version: "0.2.0", state: "valid" });
  assert.deepEqual(wcodes(newer), ["revert_newer"]);
  assert.equal(P.buildRevertPlan({ version: "0.1.0" }, { version: "", state: "empty" }).blocked, true);
  assert.equal(P.buildRevertPlan({ version: "0.1.0" }, null).blocked, true);
});

test("decideOutcome: flashed build running and pending, then confirmed", () => {
  const exp = { kind: "flash", version: "0.2.0", sha: "20212223" + "0".repeat(56), before: { version: "0.1.0", sha: "11111111" } };
  let o = P.decideOutcome(exp, { version: "0.2.0", sha: "20212223", pending: true });
  assert.equal(o.state, "confirming");
  assert.equal(o.message, "Updated to 0.2.0. Checking it works...");
  o = P.decideOutcome(exp, { version: "0.2.0", sha: "20212223", pending: false });
  assert.equal(o.state, "confirmed");
  assert.match(o.message, /^Update confirmed/);
});

test("decideOutcome: old build back means the self-test failed", () => {
  const exp = { kind: "flash", version: "0.2.0", sha: "20212223", before: { version: "0.1.0", sha: "11111111" } };
  const o = P.decideOutcome(exp, { version: "0.1.0", sha: "11111111", pending: false });
  assert.equal(o.state, "rolled_back");
  assert.equal(o.message, "The new firmware did not pass its self-test, so the device went back to 0.1.0.");
});

test("decideOutcome: version matches but the build differs is unexpected, not success", () => {
  const exp = { kind: "flash", version: "0.2.0+abc.dirty", sha: "20212223", before: { version: "0.1.0", sha: "11111111" } };
  assert.equal(P.decideOutcome(exp, { version: "0.2.0+abc.dirty", sha: "99999999", pending: true }).state, "unexpected");
  assert.equal(P.decideOutcome(exp, { version: "0.3.0", sha: "abcdabcd", pending: false }).state, "unexpected");
});

test("decideOutcome: reflashing the same build relies on the pending flag", () => {
  const exp = { kind: "flash", version: "0.2.0", sha: "20212223", before: { version: "0.2.0", sha: "20212223" } };
  assert.equal(P.decideOutcome(exp, { version: "0.2.0", sha: "20212223", pending: true }).state, "confirming");
  assert.equal(P.decideOutcome(exp, { version: "0.2.0", sha: "20212223", pending: false }).state, "same_build");
});

test("decideOutcome: revert, version only (previous sha is not reported)", () => {
  const exp = { kind: "revert", version: "0.1.0", sha: "", before: { version: "0.2.0", sha: "22222222" } };
  let o = P.decideOutcome(exp, { version: "0.1.0", sha: "11111111", pending: true });
  assert.equal(o.state, "confirming");
  assert.equal(o.message, "Reverted to 0.1.0. Checking it works...");
  o = P.decideOutcome(exp, { version: "0.2.0", sha: "22222222", pending: false });
  assert.equal(o.state, "rolled_back");
  assert.match(o.message, /went back to 0\.2\.0/);
});

test("decideOutcome: nothing readable", () => {
  const exp = { kind: "flash", version: "0.2.0", sha: "", before: { version: "0.1.0" } };
  assert.equal(P.decideOutcome(exp, null).state, "no_device");
  assert.equal(P.decideOutcome(exp, { version: "" }).state, "no_device");
});

test("describeUpdateError maps raw failures to plain language", () => {
  const d = (s) => P.describeUpdateError(new Error(s));
  assert.equal(d("OTA_BEGIN timed out").category, "timeout");
  assert.match(d("OTA_BEGIN timed out").title, /get ready/);
  assert.equal(d("OTA_DATA timed out").category, "timeout");
  assert.equal(d("transport lost").category, "lost");
  assert.equal(d("disconnected").category, "lost");
  assert.equal(d("upload cancelled by disconnect").category, "lost");
  assert.equal(d("not connected").category, "disconnected");
  assert.equal(d("send failed: Failed to execute 'transferOut'").category, "other");
  assert.equal(d("OUT endpoint stalled").category, "write");
  assert.equal(d("USB write failed: babble").category, "write");
  assert.equal(d("device error (code 16): OTA_BEGIN failed: Device or resource busy").category, "busy");
  assert.equal(d("device error (code 119): REVERT refused: a firmware upload is in progress").category, "busy");
  assert.equal(d("device error (code 88): OTA not wired on this build: Function not implemented").category, "unsupported");
  assert.equal(d("device error (code 88): whatever").category, "unsupported");
  assert.equal(d("device error (code 1): no OTA session started here: Operation not permitted").category, "session");
  assert.equal(d("device error (code 22): OTA_DATA offset mismatch: Invalid argument").category, "session");
  assert.equal(d("device error (code 5): OTA_DATA write failed: Input/output error").category, "write");
  assert.equal(d("device error (code 74): OTA_FINISH (validate/activate) failed: Bad message").category, "rejected");
  assert.equal(d("device error (code 19): REVERT refused: the other slot holds no app image: No such device").category, "no_previous");
  assert.equal(d("device error (code 5): REVERT: esp_ota_set_boot_partition failed: ESP_ERR_X").category, "write");
  assert.equal(d("device error (code 28): OTA_BEGIN failed: No space left on device").category, "too_big");
  assert.equal(d("device error (code 22): OTA_BEGIN failed: Invalid argument").category, "rejected");
  assert.equal(d("something never seen").category, "other");
  assert.equal(P.describeUpdateError("plain string timed out").category, "timeout");
  assert.equal(P.describeUpdateError(undefined).category, "other");
  for (const s of ["OTA_BEGIN timed out", "transport lost", "x", "device error (code 16): busy"]) {
    const r = d(s);
    assert.ok(r.title.length > 10 && r.advice.length > 10);
    assert.ok(!/[—]/.test(r.title + r.advice), "no em dashes");
  }
});

test("formatProgress", () => {
  assert.deepEqual(P.formatProgress(0, 1000), { percent: 0, text: "Writing 0% (0 B of 1,000 B)" });
  assert.equal(P.formatProgress(500, 1000).percent, 50);
  assert.equal(P.formatProgress(1999, 2000).percent, 99);
  assert.equal(P.formatProgress(2000, 2000).text, "Writing 100% (2,000 B of 2,000 B)");
  assert.equal(P.formatProgress(5, 0).percent, 100);
  assert.equal(P.formatProgress(1234567, 2000000).text, "Writing 61% (1,234,567 B of 2,000,000 B)");
  assert.equal(P.formatBytes(1920 * 1024), "1,966,080 B");
});
