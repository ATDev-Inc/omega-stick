#!/usr/bin/env node
// release_feed.mjs: turns a finished ESP-IDF build into the files a release
// publishes: versioned firmware images, the esp-web-tools manifest, the
// latest.json feed the calibration console reads, and SHA256SUMS.
//
// No dependencies (Node 22 or newer). The pure functions are exported so
// tests/web/release_feed.test.js can check them; the file I/O is in run().
//
//   node scripts/release_feed.mjs \
//     --app build/omega_stick.bin --full dist-merged/full.bin \
//     --version 0.2.0 --tag v0.2.0 --notes-url https://github.com/... \
//     --published-at 2026-10-11T12:00:00Z --out site-out/firmware

import { createHash } from "node:crypto";
import { copyFileSync, mkdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { basename, join } from "node:path";
import { fileURLToPath } from "node:url";

export const PROJECT = "omega_stick";
export const CHIP = "esp32s3";
export const CHIP_FAMILY = "ESP32-S3"; // spelling esp-web-tools expects
export const FEED_SCHEMA = 1;

const VERSION_RE = /^\d+\.\d+\.\d+$/;

export function checkVersion(version, tag) {
  if (!VERSION_RE.test(version)) {
    throw new Error(`version must be MAJOR.MINOR.PATCH, got '${version}'`);
  }
  if (tag !== `v${version}`) {
    throw new Error(`tag '${tag}' does not match version '${version}' (expected 'v${version}')`);
  }
}

export function fileNames(version) {
  return {
    app: `${PROJECT}-${version}.bin`,
    full: `${PROJECT}-${version}-full.bin`,
  };
}

export function sha256Hex(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

// Paths in the feed are relative to latest.json itself.
export function buildLatest({ version, tag, publishedAt, notesUrl, app, full }) {
  checkVersion(version, tag);
  const names = fileNames(version);
  return {
    schema: FEED_SCHEMA,
    project: PROJECT,
    version,
    tag,
    published_at: publishedAt,
    notes_url: notesUrl,
    app: { path: names.app, size: app.size, sha256: app.sha256 },
    full: { path: names.full, size: full.size, sha256: full.sha256 },
    chip: CHIP,
  };
}

// esp-web-tools manifest: one build, the merged image written at offset 0.
// Paths are relative to the manifest file. No `new_install_prompt_erase`: its
// "Erase device" checkbox starts unticked, and without the prompt ESP Web
// Tools erases a device that does not speak Improv before writing, which is
// what a first flash wants.
export function buildManifest({ version }) {
  return {
    name: "Omega Stick",
    version,
    builds: [
      {
        chipFamily: CHIP_FAMILY,
        parts: [{ path: fileNames(version).full, offset: 0 }],
      },
    ],
  };
}

// sha256sum format: "<hex>  <name>\n", sorted by name, so `sha256sum -c` works.
export function buildSums(entries) {
  return [...entries]
    .sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0))
    .map((e) => `${e.sha256}  ${e.name}\n`)
    .join("");
}

function json(obj) {
  return JSON.stringify(obj, null, 2) + "\n";
}

export function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i += 2) {
    const k = argv[i];
    if (!k.startsWith("--") || i + 1 >= argv.length) throw new Error(`bad argument '${k}'`);
    out[k.slice(2)] = argv[i + 1];
  }
  for (const req of ["app", "full", "version", "tag", "notes-url", "published-at", "out"]) {
    if (!out[req]) throw new Error(`missing --${req}`);
  }
  return out;
}

export function run(args) {
  checkVersion(args.version, args.tag);
  if (Number.isNaN(Date.parse(args["published-at"]))) {
    throw new Error(`--published-at is not a date: '${args["published-at"]}'`);
  }
  const names = fileNames(args.version);
  mkdirSync(args.out, { recursive: true });

  const info = {};
  for (const [key, src] of [
    ["app", args.app],
    ["full", args.full],
  ]) {
    const bytes = readFileSync(src);
    if (bytes.length === 0) throw new Error(`${src} is empty`);
    copyFileSync(src, join(args.out, names[key]));
    info[key] = { size: statSync(src).size, sha256: sha256Hex(bytes) };
  }

  const latest = buildLatest({
    version: args.version,
    tag: args.tag,
    publishedAt: new Date(args["published-at"]).toISOString(),
    notesUrl: args["notes-url"],
    app: info.app,
    full: info.full,
  });
  const manifest = json(buildManifest({ version: args.version }));
  const latestText = json(latest);
  writeFileSync(join(args.out, "manifest.json"), manifest);
  writeFileSync(join(args.out, "latest.json"), latestText);

  const sums = buildSums([
    { name: names.app, sha256: info.app.sha256 },
    { name: names.full, sha256: info.full.sha256 },
    { name: "manifest.json", sha256: sha256Hex(manifest) },
    { name: "latest.json", sha256: sha256Hex(latestText) },
  ]);
  writeFileSync(join(args.out, "SHA256SUMS"), sums);
  return { latest, files: [names.app, names.full, "manifest.json", "latest.json", "SHA256SUMS"] };
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  try {
    const { files } = run(parseArgs(process.argv.slice(2)));
    console.log(`wrote ${files.join(", ")}`);
  } catch (e) {
    console.error(`${basename(process.argv[1])}: ${e.message}`);
    process.exit(1);
  }
}
