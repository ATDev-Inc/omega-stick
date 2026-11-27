// Regenerates the golden-derived seeds in tests/fuzz/corpus/ from
// tests/protocol/golden_frames.json: one file per frame, named
// request_<name>.bin or reply_<name>.bin (lowercase), holding the frame bytes
// (frame_hex). Only files with those two prefixes are written or removed, so
// seeds added by hand (for example crash regressions) are left alone.
//
//   node scripts/fuzz_seeds.mjs           write the corpus
//   node scripts/fuzz_seeds.mjs --check   exit 1 if the corpus is out of sync
//
// No dependencies (Node 22.2 or newer).

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

// Everything is resolved from this file, never from the current directory.
const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(scriptDir, "..");
const goldenPath = path.join(root, "tests", "protocol", "golden_frames.json");
const corpusDir = path.join(root, "tests", "fuzz", "corpus");

export const PREFIXES = ["request_", "reply_"];
const SEED_NAME = /^(request|reply)_[a-z0-9_]+.bin$/;

// Deletes only ever happen in this one directory, so refuse to run if it is
// not where the repository keeps its corpus.
function assertCorpusDir() {
  const ok =
    path.basename(scriptDir) === "scripts" &&
    corpusDir === path.join(root, "tests", "fuzz", "corpus") &&
    fs.existsSync(goldenPath) &&
    fs.existsSync(path.join(root, "tests", "fuzz", "frame_fuzz.cpp")) &&
    (!fs.existsSync(corpusDir) || fs.realpathSync(corpusDir) === fs.realpathSync(path.join(root, "tests", "fuzz", "corpus")));
  if (!ok || (fs.existsSync(corpusDir) && fs.lstatSync(corpusDir).isSymbolicLink())) {
    console.error(`fuzz_seeds: refusing to run, ${corpusDir} is not the repository tests/fuzz/corpus`);
    process.exit(2);
  }
}

// Map of seed file name to expected bytes.
export function expectedSeeds(golden) {
  const seeds = new Map();
  for (const [key, prefix] of [["requests", "request_"], ["replies", "reply_"]]) {
    for (const entry of golden[key]) {
      seeds.set(`${prefix}${entry.name.toLowerCase()}.bin`, Buffer.from(entry.frame_hex, "hex"));
    }
  }
  return seeds;
}

export function isGoldenSeedName(name) {
  return SEED_NAME.test(name);
}

function main() {
  assertCorpusDir();
  const check = process.argv.includes("--check");
  const golden = JSON.parse(fs.readFileSync(goldenPath, "utf8"));
  const want = expectedSeeds(golden);
  fs.mkdirSync(corpusDir, { recursive: true });
  let problems = 0;

  for (const name of fs.readdirSync(corpusDir).sort()) {
    // Plain files named request_*.bin / reply_*.bin directly in the corpus
    // directory only. No recursion, no directories, no links.
    if (!fs.lstatSync(path.join(corpusDir, name)).isFile()) continue;
    if (isGoldenSeedName(name) && !want.has(name)) {
      console.log(`${check ? "extra" : "remove"}: ${name}`);
      if (check) problems++;
      else fs.unlinkSync(path.join(corpusDir, name));
    }
  }
  for (const [name, bytes] of [...want].sort()) {
    const file = path.join(corpusDir, name);
    const have = fs.existsSync(file) ? fs.readFileSync(file) : null;
    if (have && have.equals(bytes)) continue;
    console.log(`${check ? (have ? "differs" : "missing") : "write"}: ${name}`);
    if (check) problems++;
    else fs.writeFileSync(file, bytes);
  }
  if (check && problems) {
    console.error("Fuzz corpus is out of sync: run node scripts/fuzz_seeds.mjs");
    process.exit(1);
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main();
