// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

import { chromium } from "playwright-core";
import { preview } from "vite";

const CASES = [
  { label: "mjpeg default", preset: "default", video: "" },
  { label: "mpeg2 small", preset: "small", video: "" },
  { label: "mpeg2 720p", preset: "small", video: "mpeg2,long=1280,short=720,maxfps=30" },
];

const dir = mkdtempSync(join(tmpdir(), "tab5conv-bench-"));
let inputs = process.argv.slice(2).map((f) => resolve(f));
if (inputs.length === 0) {
  const clip = join(dir, "testsrc2-1080p-30s.mp4");
  execFileSync("ffmpeg", [
    "-hide_banner", "-loglevel", "error", "-y",
    "-f", "lavfi", "-i", "testsrc2=s=1920x1080:r=30:d=30",
    "-f", "lavfi", "-i", "sine=r=48000:d=30",
    "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac", clip,
  ]);
  inputs = [clip];
}

const server = await preview({ root: new URL("..", import.meta.url).pathname, preview: { port: 0 }, logLevel: "error" });
const browser = await chromium.launch({ channel: "chrome", headless: true });
try {
  const page = await browser.newPage({ acceptDownloads: true });
  let timing = null;
  page.on("console", (m) => {
    const text = m.text();
    if (text.startsWith("[timing] ")) timing = JSON.parse(text.slice(9));
  });
  page.on("download", (d) => d.cancel());
  await page.goto(server.resolvedUrls.local[0]);
  for (const input of inputs) {
    for (const c of CASES) {
      await page.click("[data-ref=clear]");
      await page.selectOption("[data-ref=preset]", c.preset);
      await page.fill("[data-ref=video]", c.video);
      const chooser = page.waitForEvent("filechooser");
      await page.click("[data-ref=add]");
      await (await chooser).setFiles([input]);
      await page.waitForSelector(".row .badge:text('Ready')", { timeout: 60_000 });
      timing = null;
      await page.click("[data-ref=convert]");
      await page.waitForSelector(".row .badge:text-matches('Done|Failed')", { timeout: 1_800_000 });
      const badge = await page.textContent(".row .badge");
      console.log(`\n${c.label}: ${input.split("/").pop()} ${badge}`);
      if (!timing) {
        console.log("  no timing reported");
        continue;
      }
      const t = timing;
      const wall = t.wallMs;
      console.log(`  wall ${(wall / 1000).toFixed(2)} s, ${t.frames} frames, ${((t.frames * 1000) / wall).toFixed(1)} fps`);
      const pool = (name, count, busy) => {
        const total = Object.values(busy).reduce((a, b) => a + b, 0);
        const parts = Object.entries(busy).map(([k, v]) => `${k} ${(v / t.frames).toFixed(2)}`).join(", ");
        const use = count ? ((100 * total) / (wall * count)).toFixed(0) : 0;
        console.log(`  ${name} x${count}: ${use}% busy; ms/frame: ${parts || "-"}`);
      };
      pool("encoders", t.encoders, t.encoderBusyMs);
      pool("scalers", t.scalers, t.scaleBusyMs);
      const engine = Object.entries(t.engine).map(([k, v]) => `${k} ${(v / 1000).toFixed(2)}`).join(", ");
      console.log(`  engine s: ${engine}`);
    }
  }
} finally {
  await browser.close();
  server.httpServer.close();
  rmSync(dir, { recursive: true });
}
