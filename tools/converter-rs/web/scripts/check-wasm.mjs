// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";

import { MjpegWorker, Mpeg2Worker, YuvScaler, initSync } from "../src/wasm/tab5conv.js";

const examples = process.argv[2];
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

function i420(width, height, frames) {
  return execFileSync(
    "ffmpeg",
    [
      "-hide_banner", "-loglevel", "error",
      "-f", "lavfi", "-i", `testsrc2=s=${width}x${height}:r=30,noise=alls=16:allf=t`,
      "-frames:v", String(frames), "-pix_fmt", "yuv420p", "-f", "rawvideo", "-",
    ],
    { maxBuffer: 1 << 30 },
  );
}

let failures = 0;
const report = (label, ours, theirs) => {
  const same = Buffer.compare(Buffer.from(ours), theirs) === 0;
  console.log(`${label}: ${ours.length} bytes, ${same ? "identical" : "DIFFERENT"}`);
  if (!same) failures++;
};

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
  const theirs = execFileSync(`${examples}/jpeg`, [String(width), String(height), String(quality)], { input: rgba });
  report(`jpeg ${width}x${height} q${quality}`, ours, theirs);
}

for (const [width, height, geometry, color] of [
  [1920, 1080, [1280, 720, 1280, 720, 0, 0, 1280, 720, 1], [1, 0]],
  [1920, 1080, [1280, 720, 1280, 720, 0, 270, 720, 1280, 1], [6, 0]],
  [1920, 1080, [640, 360, 640, 360, 0, 0, 640, 360, 0], [1, 0]],
  [1920, 1080, [640, 360, 640, 360, 0, 0, 640, 360, 0], [6, 1]],
  [642, 362, [350, 198, 350, 198, 90, 0, 350, 198, 0], [6, 0]],
  [320, 240, [642, 482, 640, 480, 0, 0, 640, 480, 1], [6, 0]],
]) {
  const input = i420(width, height, 3);
  const layout = new Uint32Array([width, height, 0, width, width * height, width / 2, width * height * 1.25, width / 2]);
  const scaler = new YuvScaler(new Float64Array(geometry));
  const frame = width * height * 1.5;
  const ours = Buffer.concat(
    [0, 1, 2].map((i) =>
      scaler.convert("I420", input.subarray(i * frame, (i + 1) * frame), layout, new Uint8Array(color)),
    ),
  );
  scaler.free();
  const args = [width, height, ...geometry, ...color].map(String);
  const theirs = execFileSync(`${examples}/yuv`, args, { input, maxBuffer: 1 << 30 });
  report(`yuv ${width}x${height} > ${geometry.slice(6, 8).join("x")} out ${geometry[8]} in ${color}`, ours, theirs);
}

for (const [width, height, bframes, hq] of [
  [640, 360, 2, 1],
  [350, 198, 3, 1],
  [1280, 720, 2, 0],
  [720, 1280, 1, 1],
]) {
  const keyint = 6;
  const config = [width, height, 30, 1, 8, bframes, hq, 2, keyint];
  const input = i420(width, height, 14);
  const frame = width * height * 1.5;
  const worker = new Mpeg2Worker(new Float64Array(config));
  const parts = [];
  const take = (p) => {
    parts.push(p.data);
    p.free();
  };
  const count = input.length / frame;
  for (let i = 0; i < count; i++) {
    const gop = Math.floor(i / keyint);
    take(worker.push(gop, input.subarray(i * frame, (i + 1) * frame)));
    if ((i + 1) % keyint === 0 || i + 1 === count) take(worker.finish(gop));
  }
  worker.free();
  const theirs = execFileSync(`${examples}/mpeg2`, config.map(String), { input, maxBuffer: 1 << 30 });
  report(`mpeg2 ${width}x${height} b${bframes} hq${hq}`, Buffer.concat(parts), theirs);
}
process.exit(failures ? 1 : 0);
