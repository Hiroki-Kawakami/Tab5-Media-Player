// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";

import { MjpegWorker, Mpeg2Worker, YuvScaler, initSync } from "../src/wasm/tab5conv.js";

initSync({ module: readFileSync(new URL("../src/wasm/tab5conv_bg.wasm", import.meta.url)) });

const FRAMES = 60;

function source(width, height) {
  return execFileSync(
    "ffmpeg",
    [
      "-hide_banner", "-loglevel", "error",
      "-f", "lavfi", "-i", `testsrc2=s=1920x1080:r=30,scale=${width}:${height}:flags=bicubic,noise=alls=12:allf=t`,
      "-frames:v", String(FRAMES), "-pix_fmt", "yuv420p", "-f", "rawvideo", "-",
    ],
    { maxBuffer: 1 << 30 },
  );
}

function frames(raw, width, height) {
  const size = (width * height * 3) / 2;
  return Array.from({ length: FRAMES }, (_, i) => raw.subarray(i * size, (i + 1) * size));
}

function time(label, frameCount, run) {
  const hash = createHash("sha256");
  run(hash);
  const start = performance.now();
  run(null);
  const ms = (performance.now() - start) / frameCount;
  console.log(`${label.padEnd(34)} ${ms.toFixed(2).padStart(7)} ms/frame  ${hash.digest("hex").slice(0, 12)}`);
}

const hd = frames(source(1920, 1080), 1920, 1080);
const layout = new Uint32Array([1920, 1080, 0, 1920, 1920 * 1080, 960, 1920 * 1080 * 1.25, 960]);
for (const [label, geometry, color] of [
  ["yuv 1080p>720p bt601 full", [1280, 720, 1280, 720, 0, 0, 1280, 720, 1], [1, 0]],
  ["yuv 1080p>720p bt601 full rot", [1280, 720, 1280, 720, 0, 270, 720, 1280, 1], [1, 0]],
  ["yuv 1080p>360p limited", [640, 360, 640, 360, 0, 0, 640, 360, 0], [1, 0]],
]) {
  const scaler = new YuvScaler(new Float64Array(geometry));
  time(label, FRAMES, (hash) => {
    for (const f of hd) {
      const out = scaler.convert("I420", f, layout, new Uint8Array(color));
      hash?.update(out);
    }
  });
  scaler.free();
}

for (const [width, height] of [[1280, 720]]) {
  const input = frames(source(width, height), width, height);
  const worker = new MjpegWorker();
  time(`mjpeg analyze ${width}x${height}`, FRAMES, (hash) => {
    input.forEach((f, i) => {
      const words = worker.analyzeYuv(i, f, width, height);
      hash?.update(new Uint8Array(words.buffer));
    });
  });
  const encode = (hash) => {
    input.forEach((_, i) => {
      const frame = worker.encode(i, 75, 30, 1 << 20, true);
      hash?.update(frame.data);
      frame.free();
    });
  };
  encode(null);
  input.forEach((f, i) => worker.analyzeYuv(i, f, width, height));
  const start = performance.now();
  const hash = createHash("sha256");
  encode(hash);
  const ms = (performance.now() - start) / FRAMES;
  console.log(`${`mjpeg encode q75 ${width}x${height}`.padEnd(34)} ${ms.toFixed(2).padStart(7)} ms/frame  ${hash.digest("hex").slice(0, 12)}`);
  worker.free();
}

for (const [width, height, hq] of [[640, 360, 1], [1280, 720, 1], [640, 360, 0]]) {
  const input = frames(source(width, height), width, height);
  time(`mpeg2 ${width}x${height} hq=${hq}`, FRAMES, (hash) => {
    const worker = new Mpeg2Worker(new Float64Array([width, height, 30, 1, 8, 2, hq, 2, FRAMES]));
    const take = (p) => {
      hash?.update(p.data);
      p.free();
    };
    for (const f of input) take(worker.push(0, f));
    take(worker.finish(0));
    worker.free();
  });
}
