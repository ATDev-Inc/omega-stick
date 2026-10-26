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

test("decideOutcome: old build back means the new firmware was not confirmed", () => {
  const exp = { kind: "flash", version: "0.2.0", sha: "20212223", before: { version: "0.1.0", sha: "11111111" } };
  const o = P.decideOutcome(exp, { version: "0.1.0", sha: "11111111", pending: false });
  assert.equal(o.state, "rolled_back");
  assert.equal(o.message, "The new firmware was not confirmed, so the device went back to 0.1.0.");
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
  // A revert that left the old image running says so, not "the new firmware was not confirmed".
  assert.match(o.message, /running 0\.2\.0 again, not the previous firmware \(0\.1\.0\)/);
  assert.doesNotMatch(o.message, /new firmware/);
});

const CONFIRM_EXP = { kind: "flash", version: "0.2.0", sha: "20212223", before: { version: "0.1.0", sha: "11111111" } };
const PENDING_NEW = { version: "0.2.0", sha: "20212223", pending: true };

test("CONFIRM_CHECKS lists firmware, protocol, sensor in order", () => {
  assert.deepEqual(P.CONFIRM_CHECKS.map((c) => c.id), ["firmware", "protocol", "sensor"]);
  for (const c of P.CONFIRM_CHECKS) assert.ok(c.label);
});

test("judgeConfirmCheck: all checks pass", () => {
  assert.deepEqual(P.judgeConfirmCheck("firmware", { expected: CONFIRM_EXP, after: PENDING_NEW }),
    { ok: true, skipped: false, message: "" });
  assert.equal(P.judgeConfirmCheck("protocol", { error: "" }).ok, true);
  const r = P.judgeConfirmCheck("sensor", { diagnostics: { sensorFault: 0, sensorFirstFault: 0 } });
  assert.deepEqual(r, { ok: true, skipped: false, message: "" });
});

test("judgeConfirmCheck: a version or build mismatch fails the firmware check", () => {
  const wrongVer = P.judgeConfirmCheck("firmware", { expected: CONFIRM_EXP, after: { version: "0.3.0", sha: "abcdabcd", pending: true } });
  assert.equal(wrongVer.ok, false);
  assert.match(wrongVer.message, /not the version that was installed/);
  const wrongSha = P.judgeConfirmCheck("firmware", { expected: CONFIRM_EXP, after: { version: "0.2.0", sha: "99999999", pending: true } });
  assert.equal(wrongSha.ok, false);
  const old = P.judgeConfirmCheck("firmware", { expected: CONFIRM_EXP, after: { version: "0.1.0", sha: "11111111", pending: false } });
  assert.equal(old.ok, false);
  assert.match(old.message, /was not confirmed/);
  assert.equal(P.judgeConfirmCheck("firmware", { expected: CONFIRM_EXP, after: null }).ok, false);
});

test("judgeConfirmCheck: the expected image already confirmed is a success that needs no MARK_VALID", () => {
  // Older self-confirming firmware can confirm itself between the reconnect and the console's own check.
  const r = P.judgeConfirmCheck("firmware", { expected: CONFIRM_EXP, after: { ...PENDING_NEW, pending: false } });
  assert.equal(r.ok, true);
  assert.equal(r.alreadyConfirmed, true);
  assert.match(r.message, /Update confirmed/);
  // A pending image is the normal case and is not flagged.
  assert.equal(P.judgeConfirmCheck("firmware", { expected: CONFIRM_EXP, after: PENDING_NEW }).alreadyConfirmed, undefined);
  // A different or rolled-back image that is not pending is still rejected.
  assert.equal(P.judgeConfirmCheck("firmware", { expected: CONFIRM_EXP, after: { version: "0.3.0", sha: "abcdabcd", pending: false } }).ok, false);
  assert.equal(P.judgeConfirmCheck("firmware", { expected: CONFIRM_EXP, after: { version: "0.1.0", sha: "11111111", pending: false } }).ok, false);
});

test("judgeConfirmCheck: a failed GET_STATE fails the protocol check", () => {
  const r = P.judgeConfirmCheck("protocol", { error: "GET_STATE timed out" });
  assert.equal(r.ok, false);
  assert.match(r.message, /GET_STATE timed out/);
});

test("judgeConfirmCheck: a sensor fault fails the sensor check", () => {
  let r = P.judgeConfirmCheck("sensor", { diagnostics: { sensorFault: 1 } });
  assert.equal(r.ok, false);
  assert.equal(r.skipped, false);
  assert.match(r.message, /read failed/);
  r = P.judgeConfirmCheck("sensor", { diagnostics: { sensorFault: 2 } });
  assert.match(r.message, /reading stuck/);
  assert.match(P.judgeConfirmCheck("sensor", { diagnostics: { sensorFault: 9 } }).message, /code 9/);
});

test("judgeConfirmCheck: diagnostics unsupported skips the sensor check, other errors fail it", () => {
  const skip = P.judgeConfirmCheck("sensor", { error: "device error (code 88): GET_DIAGNOSTICS: Function not implemented" });
  assert.equal(skip.ok, true);
  assert.equal(skip.skipped, true);
  assert.match(skip.message, /skipped/);
  assert.equal(P.judgeConfirmCheck("sensor", { error: "function_not_supported" }).skipped, true);
  const bad = P.judgeConfirmCheck("sensor", { error: "GET_DIAGNOSTICS timed out" });
  assert.equal(bad.ok, false);
  assert.equal(bad.skipped, false);
  assert.equal(P.judgeConfirmCheck("sensor", {}).ok, false);
});

test("judgeConfirmCheck: an unknown check id fails", () => {
  assert.equal(P.judgeConfirmCheck("nope", {}).ok, false);
});

test("confirmFailureNotice says the device will go back and points at the manual override", () => {
  const s = P.confirmFailureNotice("The sensor reported a fault (read failed).");
  assert.ok(s.startsWith("The sensor reported a fault (read failed)."));
  assert.match(s, /not confirmed/);
  assert.match(s, /within about two minutes/);
  assert.match(s, /Mark running image valid/);
  assert.match(s, /skips these checks/);
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

// ---- Choosing which granted device to reopen after a reboot ----

const serialLink = { kind: "serial", vid: 0x303a, pid: 0x4001 };
const sp = (ref, o) => Object.assign({ ref, kind: "serial", opened: false, vid: 0x303a, pid: 0x4001, serial: "", isSaved: false }, o);

test("reconnect: the saved port wins whether it is listed first or second", () => {
  const orig = sp("orig", { isSaved: true }), other = sp("other");
  assert.equal(P.pickReconnectCandidate(serialLink, [orig, other]).candidate, orig);
  const r = P.pickReconnectCandidate(serialLink, [other, orig]);
  assert.equal(r.candidate, orig);
  assert.equal(r.reason, "exact");
});

test("reconnect: two same-VID/PID sticks and the original absent is ambiguous", () => {
  const r = P.pickReconnectCandidate(serialLink, [sp("a"), sp("b")]);
  assert.equal(r.candidate, null);
  assert.equal(r.reason, "ambiguous");
});

test("reconnect: a single VID/PID match is used when the saved port is gone", () => {
  const only = sp("new");
  const r = P.pickReconnectCandidate(serialLink, [only, sp("x", { vid: 1, pid: 2 })]);
  assert.equal(r.candidate, only);
  assert.equal(r.reason, "only-match");
  assert.equal(P.pickReconnectCandidate(serialLink, []).reason, "none");
});

test("reconnect: opened ports are skipped", () => {
  const open = sp("open", { opened: true, isSaved: true });
  assert.equal(P.pickReconnectCandidate(serialLink, [open]).reason, "none");
  const free = sp("free");
  const r = P.pickReconnectCandidate(serialLink, [open, free]);
  assert.equal(r.candidate, free);
  assert.equal(r.reason, "only-match");
});

test("reconnect: USB serial number picks one of several sticks", () => {
  const link = { kind: "usb", vid: 0x303a, pid: 0x4001, serial: "AAA" };
  const u = (ref, serial) => sp(ref, { kind: "usb", serial });
  const want = u("a", "AAA");
  const r = P.pickReconnectCandidate(link, [u("b", "BBB"), want, u("c", "CCC")]);
  assert.equal(r.candidate, want);
  assert.equal(r.reason, "exact");
  assert.equal(P.pickReconnectCandidate(link, [u("b", "BBB"), u("c", "CCC")]).reason, "none");
});

test("reconnect: USB without a known serial and several matches is ambiguous", () => {
  const link = { kind: "usb", vid: 0x303a, pid: 0x4001, serial: "" };
  const u = (ref) => sp(ref, { kind: "usb" });
  assert.equal(P.pickReconnectCandidate(link, [u("a"), u("b")]).reason, "ambiguous");
  assert.equal(P.pickReconnectCandidate(link, [u("a")]).reason, "only-match");
});

// ---- the sensor check over time (D22: "not observed" is not "healthy") ------

test("autoReconnectStep: a manual connect owns connection setup; a success ends the loop for good", () => {
  const s = (o) => P.autoReconnectStep(Object.assign({ transportPresent: false, manualInProgress: false, manualSucceeded: false }, o));
  assert.equal(s({}), "try");
  assert.equal(s({ manualInProgress: true }), "wait");
  assert.equal(s({ transportPresent: true }), "stop");
  assert.equal(s({ transportPresent: true, manualInProgress: true }), "stop");
  assert.equal(s({ manualSucceeded: true }), "stop"); // even if that link has dropped again
});

// Two granted sticks that share the default USB serial. The person picks A
// while the automatic loop is scanning; connectUsb(A) is still awaiting
// claimInterface() (A is opened, not yet the transport) across a loop tick.
test("reconnect race: the automatic loop never opens stick B while a manual open of A is pending", () => {
  const link = { kind: "usb", vid: 0x1209, pid: 0x0d32, serial: "123456" };
  const A = { ref: "A", kind: "usb", opened: false, vid: 0x1209, pid: 0x0d32, serial: "123456", isSaved: false };
  const B = Object.assign({}, A, { ref: "B" });
  const world = { transport: null, manualInProgress: false, manualSucceeded: false, opened: [] };
  // One wake of the automatic loop, as the page runs it: ask, list, ask again, open.
  const autoTick = () => {
    const ask = () => P.autoReconnectStep({ transportPresent: !!world.transport,
      manualInProgress: world.manualInProgress, manualSucceeded: world.manualSucceeded });
    if (ask() !== "try") return ask();
    const found = P.pickReconnectCandidate(link, [A, B]);
    if (ask() !== "try") return ask();
    if (found.candidate) { found.candidate.opened = true; world.opened.push(found.candidate.ref); }
    return found.reason;
  };
  assert.equal(autoTick(), "ambiguous"); // both free: nothing opened, the picker is offered
  world.manualInProgress = true; A.opened = true; // picker chose A, open pending
  assert.equal(autoTick(), "wait");
  assert.equal(autoTick(), "wait");
  world.transport = "A"; world.manualInProgress = false; world.manualSucceeded = true;
  assert.equal(autoTick(), "stop");
  world.transport = null; // A drops again later: the loop still must not reach for B
  assert.equal(autoTick(), "stop");
  assert.deepEqual(world.opened, []);
  // Without the arbitration the same sequence would have opened B.
  assert.equal(P.pickReconnectCandidate(link, [A, B]).candidate.ref, "B");
});

test("reconnect race: a cancelled manual connect lets the loop resume", () => {
  assert.equal(P.autoReconnectStep({ transportPresent: false, manualInProgress: true, manualSucceeded: false }), "wait");
  assert.equal(P.autoReconnectStep({ transportPresent: false, manualInProgress: false, manualSucceeded: false }), "try");
});

function fakeClock() {
  const c = { t: 0, sleeps: 0 };
  c.now = () => c.t;
  c.sleep = async (ms) => { c.t += ms; c.sleeps++; };
  return c;
}
// A device whose DIAGNOSTICS sensor byte follows `codes` (the last repeats), one entry per request.
function sensorReader(codes, requests) {
  let i = 0;
  return async () => {
    requests.push(i);
    const code = codes[Math.min(i++, codes.length - 1)];
    return { diagnostics: { sensorFault: code } };
  };
}

test("judgeConfirmCheck: code 3 (not observed yet) is a wait, not a fault and not healthy", () => {
  const r = P.judgeConfirmCheck("sensor", { diagnostics: { sensorFault: 3 } });
  assert.equal(r.ok, false);
  assert.equal(r.wait, true);
  assert.equal(P.SENSOR_NOT_OBSERVED, 3);
  assert.doesNotMatch(r.message, /fault/i);
});

test("describeSensorState names code 3 as starting up, never as a fault", () => {
  assert.equal(P.describeSensorState(0), "none");
  assert.equal(P.describeSensorState(1), "read failed");
  assert.equal(P.describeSensorState(2), "reading stuck");
  assert.equal(P.describeSensorState(3), "waiting for first readings");
  assert.equal(P.describeSensorState(9), "code 9");
});

test("sensor check: startup-order regression, diagnostics asked before polling then reads fail, is withheld", async () => {
  const clock = fakeClock();
  const asked = [];
  // The first replies come before polling began (3); the sensor then fails (1).
  const r = await P.awaitSensorHealthy(sensorReader([3, 3, 3, 1], asked), clock.sleep, clock.now);
  assert.equal(r.ok, false);
  assert.match(r.message, /read failed/);
  assert.equal(asked.length, 4);
});

test("sensor check: waits through code 3 and confirms only after healthy twice, at least 1.5 s apart", async () => {
  const clock = fakeClock();
  const asked = [];
  const waits = [];
  const r = await P.awaitSensorHealthy(sensorReader([3, 3, 3, 0], asked), clock.sleep, clock.now, { onWait: (m) => waits.push(m) });
  assert.equal(r.ok, true);
  assert.equal(waits.length, 3);
  // Three waits, then the first 0 at 1500 ms, the second 0 at 3000 ms (1.5 s later).
  assert.equal(asked.length, 3 + 4);
  assert.equal(clock.t, 3000);
});

test("sensor check: a sensor stuck at code 3 fails after 10 s and never reports healthy", async () => {
  const clock = fakeClock();
  const asked = [];
  const r = await P.awaitSensorHealthy(sensorReader([3], asked), clock.sleep, clock.now);
  assert.equal(r.ok, false);
  assert.match(r.message, /not producing readings after 10 seconds/);
  assert.ok(clock.t >= P.SENSOR_WAIT_MS && clock.t <= P.SENSOR_WAIT_MS + P.SENSOR_POLL_MS);
});

test("sensor check: firmware that cannot say 'not observed' (always 0) is still held for the dwell", async () => {
  const clock = fakeClock();
  const asked = [];
  const r = await P.awaitSensorHealthy(sensorReader([0], asked), clock.sleep, clock.now);
  assert.equal(r.ok, true);
  assert.equal(clock.t, P.SENSOR_DWELL_MS);
  // ...and a fault that shows up during the dwell stops it.
  const clock2 = fakeClock();
  const r2 = await P.awaitSensorHealthy(sensorReader([0, 0, 1], []), clock2.sleep, clock2.now);
  assert.equal(r2.ok, false);
  assert.match(r2.message, /read failed/);
});

test("sensor check: unsupported diagnostics skips at once, a transport error fails at once, abort stops", async () => {
  const clock = fakeClock();
  const skip = await P.awaitSensorHealthy(async () => ({ error: "device error (code 88): GET_DIAGNOSTICS: Function not implemented" }), clock.sleep, clock.now);
  assert.equal(skip.skipped, true);
  assert.equal(clock.t, 0);
  const bad = await P.awaitSensorHealthy(async () => ({ error: "GET_DIAGNOSTICS timed out" }), clock.sleep, clock.now);
  assert.equal(bad.ok, false);
  assert.equal(clock.t, 0);
  const aborted = await P.awaitSensorHealthy(sensorReader([3], []), clock.sleep, clock.now, { abort: () => true });
  assert.equal(aborted.aborted, true);
  assert.equal(aborted.ok, false);
});

// ---- Robustness of the update flow: controls, link loss, lost replies ----

const noDash = (s) => assert.ok(!/\u2014/.test(s), "no em dashes: " + s);

test("inspectImage: an app descriptor cut short by the end of the file is an error, never ok without appDesc", () => {
  // One 16-byte segment: the image is complete, but the descriptor (256 bytes) is not.
  const short = makeImage({ dataLen: 16 });
  assert.ok(short.length < 32 + 256);
  const r = P.inspectImage(short);
  assert.equal(r.ok, false);
  assert.deepEqual(codes(r), ["truncated"]);
  assert.equal(r.info.appDesc, null);
  // Every prefix of a good image: whenever the check passes, the descriptor was read.
  const img = makeImage();
  for (let n = 0; n <= img.length; n++) {
    const x = P.inspectImage(img.subarray(0, n));
    if (x.ok) assert.ok(x.info && x.info.appDesc, "ok without appDesc at length " + n);
  }
});

test("previous slot: partial is not revertible and says an interrupted upload left it", () => {
  assert.deepEqual(P.revertAvailability({ prevState: "valid" }), { usable: true, reason: "" });
  assert.equal(P.revertAvailability({ prevState: "empty" }).usable, false);
  const partial = P.revertAvailability({ prevState: "partial" });
  assert.equal(partial.usable, false);
  assert.match(partial.reason, /interrupted upload left a partial image/);
  assert.match(partial.reason, /Upload new firmware/);
  assert.equal(P.revertAvailability(null).usable, false);
  assert.equal(P.revertAvailability({ prevState: "unknown (older firmware)" }).usable, false);
  assert.ok(!P.REVERTIBLE_STATES.includes("partial") && !P.REVERTIBLE_STATES.includes("empty"));
  assert.match(P.describePrevState("partial"), /interrupted upload/);
  assert.equal(P.describePrevState("valid"), "valid");
  // The revert plan refuses it even when a version string came with it.
  const plan = P.buildRevertPlan({ version: "0.2.0", sha: "22222222" }, { version: "0.3.0", label: "ota_1", state: "partial" });
  assert.equal(plan.blocked, true);
  assert.deepEqual(plan.errors.map((e) => e.code), ["partial_previous"]);
  noDash(partial.reason);
});

test("describeUpdateError shows the firmware's refusal while the running image is pending", () => {
  const raw = "device error (code 1): OTA_BEGIN refused: the running image is not confirmed yet: Operation not permitted";
  const d = P.describeUpdateError(new Error(raw));
  assert.equal(d.category, "not_confirmed");
  assert.match(d.title, /the running image is not confirmed yet/);
  assert.doesNotMatch(d.title + d.advice, /would not start the update/);
  assert.match(d.advice, /Mark running image valid/);
  const longer = P.describeUpdateError("device error (code 1): OTA_BEGIN refused: the running image is not confirmed yet (confirm it or wait for the rollback): Operation not permitted");
  assert.match(longer.title, /not confirmed yet \(confirm it or wait for the rollback\)\.$/);
  // Other OTA_BEGIN refusals keep their categories.
  assert.equal(P.describeUpdateError("device error (code 16): OTA_BEGIN failed: Device or resource busy").category, "busy");
  noDash(d.title + d.advice);
});

const idle = (o) => Object.assign({
  connected: true, busy: false, latestBusy: false, phase: null, reconnecting: false, pending: false,
  revert: { usable: true, reason: "" },
}, o);
const enabled = (c) => !c.disabled;

test("updateControls: idle and connected, everything is usable and nothing is locked", () => {
  for (const phase of [null, undefined, "done"]) {
    const c = P.updateControls(idle({ phase }));
    assert.ok(enabled(c.flash) && enabled(c.install) && enabled(c.revert) && enabled(c.restart) && enabled(c.markValid));
    assert.equal(c.connectLocked, false);
    assert.equal(c.modeLocked, false);
  }
  const off = P.updateControls(idle({ connected: false }));
  assert.ok(off.flash.disabled && off.revert.disabled && off.restart.disabled && off.markValid.disabled);
  assert.equal(P.updateControls(idle({ latestBusy: true })).install.disabled, true);
  assert.equal(P.updateControls(idle({ busy: true })).flash.disabled, true);
});

test("updateControls: an update in flight locks Flash, Revert, Restart, mode and the link even when not busy", () => {
  for (const phase of ["running", "rebooting", "confirming"]) {
    const c = P.updateControls(idle({ phase, busy: false }));
    for (const k of ["flash", "install", "revert", "restart", "markValid"]) assert.ok(c[k].disabled, phase + ": " + k);
    assert.ok(c.flash.reason.length > 0, phase);
    assert.equal(c.modeLocked, true, phase);
    assert.equal(c.connectLocked, true, phase + ": Disconnect locked");
  }
  // After the drop the person may connect by hand, also while the automatic
  // reconnect still runs: a stick whose USB serial changed with the update is
  // only reachable through the picker.
  assert.equal(P.updateControls(idle({ phase: "rebooting", connected: false, reconnecting: true })).connectLocked, false);
  assert.equal(P.updateControls(idle({ phase: "rebooting", connected: false, reconnecting: false })).connectLocked, false);
  // Never dropped after the restart command: still locked.
  assert.equal(P.updateControls(idle({ phase: "rebooting", connected: true, reconnecting: false })).connectLocked, true);
  // After a manual disconnect the person must be able to connect again.
  assert.equal(P.updateControls(idle({ phase: "link_lost", connected: false })).connectLocked, false);
});

test("updateControls: a pending running image disables Flash with the reason, Mark valid stays", () => {
  const c = P.updateControls(idle({ pending: true }));
  assert.equal(c.flash.disabled, true);
  assert.equal(c.flash.reason, P.PENDING_FLASH_REASON);
  assert.match(c.flash.reason, /refuses a new update/);
  assert.match(c.flash.reason, /Mark running image valid/);
  assert.equal(c.install.disabled, true);
  assert.ok(enabled(c.markValid) && enabled(c.revert) && enabled(c.restart));
  assert.equal(c.connectLocked, false);
  // Waiting for the rollback after a failed check: only Mark valid remains.
  const w = P.updateControls(idle({ phase: "waiting_rollback", pending: true }));
  assert.ok(w.flash.disabled && w.revert.disabled && w.restart.disabled);
  assert.match(w.flash.reason, /about to go back/);
  assert.ok(enabled(w.markValid));
  assert.equal(w.connectLocked, true);
  noDash(c.flash.reason + w.flash.reason);
});

test("updateControls: installed but not restarted offers Restart only", () => {
  const c = P.updateControls(idle({ phase: "switched_no_restart" }));
  assert.ok(enabled(c.restart));
  assert.ok(c.flash.disabled && c.revert.disabled && c.markValid.disabled);
  assert.match(c.flash.reason, /Restart now/);
  assert.equal(c.connectLocked, true);
  assert.equal(c.modeLocked, true);
});

test("updateControls: a partial previous slot disables Revert with the explanation", () => {
  const c = P.updateControls(idle({ revert: P.revertAvailability({ prevState: "partial" }) }));
  assert.equal(c.revert.disabled, true);
  assert.match(c.revert.reason, /interrupted upload/);
  assert.ok(enabled(c.flash));
});

test("afterLinkLoss: Disconnect while confirming or waiting is resumed by the next connect, not left stuck", () => {
  const c = P.afterLinkLoss("confirming", true);
  assert.equal(c.phase, "link_lost");
  assert.equal(c.reconnect, false);
  assert.match(c.message, /goes back to the previous firmware by itself unless this page confirms it/);
  assert.equal(P.resumesOnReconnect(c.phase), true);
  const w = P.afterLinkLoss("waiting_rollback", true);
  assert.equal(w.phase, "link_lost");
  assert.match(w.message, /goes back to the previous firmware by itself/);
  const s = P.afterLinkLoss("switched_no_restart", true);
  assert.equal(s.phase, "link_lost");
  assert.match(s.message, /Restart now/);
  // A dropped link (not the button) reconnects by itself.
  for (const phase of ["confirming", "waiting_rollback", "switched_no_restart"]) {
    const d = P.afterLinkLoss(phase, false);
    assert.equal(d.phase, "rebooting");
    assert.equal(d.reconnect, true);
    assert.equal(P.resumesOnReconnect(d.phase), true);
    noDash(d.message + P.afterLinkLoss(phase, true).message);
  }
  for (const phase of ["running", "rebooting", "done", null]) {
    assert.equal(P.afterLinkLoss(phase, true), null);
    assert.equal(P.afterLinkLoss(phase, false), null);
  }
  for (const phase of ["confirming", "waiting_rollback", "switched_no_restart", "running", "done", null]) {
    assert.equal(P.resumesOnReconnect(phase), false, String(phase));
  }
});

test("reconnect window outlasts a 60 s boot flash window and ends before the 120 s rollback", () => {
  assert.ok(P.RECONNECT_WINDOW_MS >= 100000 && P.RECONNECT_WINDOW_MS < 120000);
  assert.equal(P.reconnectDeadline(5000), 5000 + P.RECONNECT_WINDOW_MS);
  const early = P.reconnectWaitText(100000, 2000);
  assert.match(early, /100 s left/);
  assert.doesNotMatch(early, /minute/);
  const late = P.reconnectWaitText(65000, 45000);
  assert.match(late, /65 s left/);
  assert.match(late, /up to a minute/);
  assert.match(late, /Connect WebUSB/);
  assert.doesNotMatch(early, /Connect WebUSB/);
  assert.match(P.reconnectWaitText(-50, 120000), /0 s left/);
});

test("decideAfterFinishTimeout: a switched boot slot means installed but not restarted", () => {
  const sw = P.decideAfterFinishTimeout("flash", { running: "ota_0", boot: "ota_1" });
  assert.equal(sw.state, "switched");
  assert.match(sw.message, /new firmware is installed, but the device did not restart/);
  assert.match(sw.message, /Restart now/);
  const rv = P.decideAfterFinishTimeout("revert", { running: "ota_1", boot: "ota_0" });
  assert.equal(rv.state, "switched");
  assert.match(rv.message, /previous firmware is installed/);
  const same = P.decideAfterFinishTimeout("flash", { running: "ota_0", boot: "ota_0" });
  assert.equal(same.state, "not_switched");
  assert.match(same.message, /nothing was installed/);
  assert.match(P.decideAfterFinishTimeout("revert", { running: "ota_0", boot: "ota_0" }).message, /nothing was changed/);
  assert.equal(P.decideAfterFinishTimeout("flash", null).state, "unknown");
  assert.equal(P.decideAfterFinishTimeout("flash", { running: "ota_0", boot: "" }).state, "unknown");
  for (const m of [sw.message, rv.message, same.message]) noDash(m);
});

test("decideOutcome: the old image with the boot slot switched is not_restarted, a real rollback is not", () => {
  const exp = { kind: "flash", version: "0.2.0", sha: "20212223", before: { version: "0.1.0", sha: "11111111" } };
  const o = P.decideOutcome(exp, { version: "0.1.0", sha: "11111111", pending: false, running: "ota_0", boot: "ota_1" });
  assert.equal(o.state, "not_restarted");
  assert.match(o.message, /Restart now/);
  const back = P.decideOutcome(exp, { version: "0.1.0", sha: "11111111", pending: false, running: "ota_0", boot: "ota_0" });
  assert.equal(back.state, "rolled_back");
  assert.match(back.message, /was not confirmed/);
  // Same build reflashed and not restarted.
  const sameExp = { kind: "flash", version: "0.2.0", sha: "20212223", before: { version: "0.2.0", sha: "20212223" } };
  assert.equal(P.decideOutcome(sameExp, { version: "0.2.0", sha: "20212223", pending: false, running: "ota_0", boot: "ota_1" }).state, "not_restarted");
  // The OTA_FINISH reply went with the link: old image back means the update did not complete.
  const unc = P.decideOutcome(Object.assign({ uncertain: true }, exp), { version: "0.1.0", sha: "11111111", pending: false });
  assert.equal(unc.state, "rolled_back");
  assert.match(unc.message, /did not complete, so the device is still running 0\.1\.0/);
  // ...and the new image pending is confirmed as usual.
  assert.equal(P.decideOutcome(Object.assign({ uncertain: true }, exp), PENDING_NEW).state, "confirming");
});

test("shouldWarnBeforeUnload: only while an upload, download or confirm is unfinished", () => {
  assert.equal(P.shouldWarnBeforeUnload({ phase: null }), false);
  assert.equal(P.shouldWarnBeforeUnload({ phase: "done" }), false);
  assert.equal(P.shouldWarnBeforeUnload({ phase: "waiting_rollback" }), false);
  assert.equal(P.shouldWarnBeforeUnload({ phase: null, uploading: true }), true);
  assert.equal(P.shouldWarnBeforeUnload({ phase: null, downloading: true }), true);
  for (const phase of ["running", "rebooting", "confirming", "switched_no_restart", "link_lost"]) {
    assert.equal(P.shouldWarnBeforeUnload({ phase }), true, phase);
  }
});

test("fetch failures: CORS or network is not called offline; a stalled download is a timeout", () => {
  const abort = Object.assign(new Error("aborted"), { name: "AbortError" });
  assert.equal(P.classifyFetchError(abort, true), "timeout");
  assert.equal(P.classifyFetchError(new TypeError("Failed to fetch"), true), "unreachable");
  assert.equal(P.classifyFetchError(new TypeError("Failed to fetch"), undefined), "unreachable");
  assert.equal(P.classifyFetchError(new TypeError("Failed to fetch"), false), "offline");
  const unreachable = P.describeFeedFailure("unreachable");
  assert.doesNotMatch(unreachable, /offline|internet connection/i);
  assert.match(unreachable, /blocked/);
  assert.match(P.describeFeedFailure("offline"), /offline/);
  assert.match(P.describeFeedFailure("http", 503), /503/);
  assert.match(P.describeDownloadFailure("timeout"), /stopped making progress/);
  assert.doesNotMatch(P.describeDownloadFailure("unreachable"), /offline|internet connection/i);
  assert.match(P.describeDownloadFailure("sha"), /checksum/);
  for (const k of ["offline", "timeout", "unreachable", "missing", "http", "invalid", undefined]) noDash(P.describeFeedFailure(k, 500));
  for (const k of ["offline", "timeout", "unreachable", "http", "size", "sha", "nohash", undefined]) noDash(P.describeDownloadFailure(k, 500));
});

test("runConfirmCheck judges the non-sensor checks once and waits only for the sensor", async () => {
  const clock = fakeClock();
  const env = { sleep: clock.sleep, now: clock.now };
  const fw = await P.runConfirmCheck("firmware", async () => ({ expected: CONFIRM_EXP, after: PENDING_NEW }), env);
  assert.equal(fw.ok, true);
  assert.equal((await P.runConfirmCheck("protocol", async () => ({ error: "x" }), env)).ok, false);
  assert.equal(clock.sleeps, 0);
  const sensor = await P.runConfirmCheck("sensor", sensorReader([3, 0], []), env);
  assert.equal(sensor.ok, true);
});

// ---- D26: a stick waiting for calibration can still confirm an update -------

test("confirm checks: a pending image with needs_calibration=1 and a healthy sensor is confirmed", async () => {
  // The quiet-boot hold must not block MARK_VALID: the checks look at the
  // sensor fault code only, and a board waiting for calibration still polls
  // its sensor.
  const clock = fakeClock();
  const diagnostics = P.decodeDiagnostics(Uint8Array.from([0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 1]));
  assert.equal(diagnostics.calibrationState, P.CALIBRATION_NEEDED);
  assert.equal(diagnostics.sensorFault, 0);
  const results = [];
  for (const c of P.CONFIRM_CHECKS) {
    const gather = async () => c.id === "firmware" ? { expected: CONFIRM_EXP, after: PENDING_NEW }
      : c.id === "protocol" ? { error: "" } : { diagnostics };
    results.push(await P.runConfirmCheck(c.id, gather, { sleep: clock.sleep, now: clock.now }));
  }
  for (const r of results) {
    assert.equal(r.ok, true, r.message);
    assert.equal(r.skipped, false);
  }
  assert.ok(clock.t >= P.SENSOR_DWELL_MS); // the sensor was asked twice, as usual
});
