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
const SEED_NAME = /^(request|reply)_[a-z0-9_]+\.bin$/;

// Deletes only ever happen in this one directory, so refuse to run if it is
// not where the repository keeps its corpus. The repository root is found a
// second way, by walking up from the corpus directory to the first .git, and
// must agree with the one derived from this script's location.
function refuse(why) {
  console.error(`fuzz_seeds: refusing to run: ${why} (${corpusDir})`);
  process.exit(2);
}

function assertCorpusDir() {
  if (path.basename(scriptDir) !== "scripts") refuse("not run from the repository scripts directory");
  if (!fs.existsSync(corpusDir)) refuse("tests/fuzz/corpus does not exist");
  if (fs.lstatSync(corpusDir).isSymbolicLink()) refuse("the corpus directory is a link");
  const realRoot = fs.realpathSync(root);
  const realCorpus = fs.realpathSync(corpusDir);
  if (realCorpus !== path.join(realRoot, "tests", "fuzz", "corpus")) refuse("not tests/fuzz/corpus of this repository");
  let dir = path.dirname(realCorpus);
  while (!fs.existsSync(path.join(dir, ".git"))) {
    const up = path.dirname(dir);
    if (up === dir) refuse("no .git above the corpus directory");
    dir = up;
  }
  if (dir !== realRoot) refuse("the repository root found from the corpus differs from the script's");
  if (!fs.existsSync(path.join(realRoot, "tests", "protocol", "golden_frames.json"))) {
    refuse("tests/protocol/golden_frames.json is missing");
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
    // Never write through a link or into a non-file.
    let have = null;
    try {
      if (!fs.lstatSync(file).isFile()) refuse(`${name} exists and is not a regular file`);
      have = fs.readFileSync(file);
    } catch (e) {
      if (e.code !== "ENOENT") throw e;
    }
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
