// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";

import { MjpegWorker, initSync } from "../src/wasm/tab5conv.js";

const native = process.argv[2];
initSync({ module: readFileSync(new URL("../src/wasm/tab5conv_bg.wasm", import.meta.url)) });

function frame(width, height, seed) {
  const rgba = new Uint8Array(width * height * 4);
  let state = seed;
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      state = (state * 1103515245 + 12345) >>> 0;
      const noise = state >>> 27;
      const i = (y * width + x) * 4;
      rgba[i] = (x * 255) / width + noise;
      rgba[i + 1] = (y * 255) / height + noise;
      rgba[i + 2] = ((x ^ y) & 0xff) >> 1;
      rgba[i + 3] = 255;
    }
  }
  return rgba;
}

let failures = 0;
for (const [width, height, quality, seed] of [
  [320, 240, 75, 1],
  [720, 1280, 75, 2],
  [854, 480, 30, 3],
  [642, 362, 95, 4],
]) {
  const rgba = frame(width, height, seed);
  const worker = new MjpegWorker();
  worker.analyze(0, rgba, width, height);
  const encoded = worker.encode(0, quality, 30, 1 << 20, true);
  const ours = Buffer.from(encoded.data);
  const theirs = execFileSync(native, [String(width), String(height), String(quality)], { input: rgba });
  const same = Buffer.compare(ours, theirs) === 0;
  console.log(`${width}x${height} q${quality}: ${ours.length} bytes, ${same ? "identical" : "DIFFERENT"}`);
  if (!same) failures++;
}
process.exit(failures ? 1 : 0);
