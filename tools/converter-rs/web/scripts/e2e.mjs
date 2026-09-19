// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import { execFileSync, spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { chromium } from "playwright-core";
import { preview } from "vite";

const cli = process.argv[2];
const dir = mkdtempSync(join(tmpdir(), "tab5conv-e2e-"));
mkdirSync(join(dir, "out"));
const failures = [];
const check = (ok, what) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
  if (!ok) failures.push(what);
};

function interleaveLag(file) {
  const out = execFileSync("ffprobe", ["-v", "error", "-show_entries", "packet=stream_index,pts_time,pos", "-of", "compact", file]).toString();
  const packets = out
    .trim()
    .split("\n")
    .map((line) => Object.fromEntries(line.split("|").slice(1).map((kv) => kv.split("="))))
    .map((p) => ({ stream: Number(p.stream_index), time: Number(p.pts_time), pos: Number(p.pos) }))
    .sort((a, b) => a.pos - b.pos);
  const last = [null, null];
  let worst = 0;
  for (const p of packets) {
    last[p.stream] = p.time;
    if (last[0] !== null && last[1] !== null) worst = Math.max(worst, Math.abs(last[1] - last[0]));
  }
  return worst;
}

const ffmpeg = (...args) => execFileSync("ffmpeg", ["-hide_banner", "-loglevel", "error", "-y", ...args]);
const probe = (file, ...entries) =>
  execFileSync("ffprobe", ["-v", "error", ...entries, "-of", "default=nw=1:nk=1", file]).toString().trim();

const tag709 = [
  "-vf", "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p",
  "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
];
const inputs = {
  "hd.mp4": ["-f", "lavfi", "-i", "testsrc2=s=1920x1080:r=30000/1001:d=3", "-f", "lavfi", "-i", "sine=r=48000:d=3", ...tag709, "-c:v", "libx264", "-c:a", "aac", "-b:a", "128k"],
  "portrait.mkv": ["-f", "lavfi", "-i", "testsrc2=s=1080x1920:r=60:d=2", "-f", "lavfi", "-i", "sine=r=44100:d=2", ...tag709, "-c:v", "libx264", "-c:a", "libmp3lame"],
  "sd.webm": ["-f", "lavfi", "-i", "testsrc2=s=640x480:r=25:d=2", "-f", "lavfi", "-i", "sine=r=48000:d=2", "-c:v", "libvpx-vp9", "-deadline", "realtime", "-colorspace", "smpte170m", "-c:a", "libopus"],
  "long.mkv": ["-f", "lavfi", "-i", "testsrc2=s=1280x720:r=30:d=20", "-f", "lavfi", "-i", "sine=r=44100:d=20", ...tag709, "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "libmp3lame"],
  "low.mov": ["-f", "lavfi", "-i", "testsrc2=s=720x480:r=24:d=2", "-f", "lavfi", "-i", "sine=r=8000:d=2", "-aspect", "16:9", "-colorspace", "smpte170m", "-c:v", "libx264", "-c:a", "aac"],
};
for (const [name, args] of Object.entries(inputs)) ffmpeg(...args, join(dir, name));
ffmpeg("-display_rotation", "90", "-i", join(dir, "low.mov"), "-c", "copy", join(dir, "rotated.mov"));
const files = [...Object.keys(inputs), "rotated.mov"].map((n) => join(dir, n));

const server = await preview({ root: new URL("..", import.meta.url).pathname, preview: { port: 0 }, logLevel: "error" });
const url = server.resolvedUrls.local[0];
const browser = await chromium.launch({ channel: "chrome", headless: true });
try {
  const page = await browser.newPage({ acceptDownloads: true });
  const errors = [];
  page.on("pageerror", (e) => errors.push(String(e)));

  await page.goto(`${url}?backend=mock`);
  await page.click("[data-ref=add]");
  await page.waitForSelector(".row .badge:text('Ready')");
  await page.fill("[data-ref=video]", "mjpeg,error=1");
  await page.waitForSelector("[data-ref=applied].bad");
  check(await page.isDisabled("[data-ref=convert]"), "mock: a bad spec disables Convert");
  await page.fill("[data-ref=video]", "");
  await page.waitForFunction(() => !document.querySelector("[data-ref=convert]").disabled);
  await page.click("[data-ref=convert]");
  await page.waitForSelector(".row:nth-child(2) .badge:text-matches('Converting')");
  await page.click("[data-ref=cancel]");
  await page.waitForSelector(".row:nth-child(2) .badge:text('Cancelled')");
  const badges = await page.$$eval(".badge", (b) => b.map((x) => x.textContent));
  check(badges.join() === "Done,Cancelled", `mock: convert then cancel (${badges})`);

  await page.goto(url);
  const chooser = page.waitForEvent("filechooser");
  await page.click("[data-ref=add]");
  await (await chooser).setFiles(files);
  await page.waitForFunction(
    (n) => [...document.querySelectorAll(".row .badge")].filter((b) => /Ready|Cannot/.test(b.textContent)).length === n,
    files.length,
    { timeout: 60_000 },
  );
  const downloads = [];
  page.on("download", (d) => downloads.push(d));
  const start = Date.now();
  await page.click("[data-ref=convert]");
  await page.waitForFunction(
    () => [...document.querySelectorAll(".row .badge")].every((b) => /Done|Failed|Cannot/.test(b.textContent)),
    null,
    { timeout: 600_000 },
  );
  console.log(`browser conversion: ${(Date.now() - start) / 1000} s`);
  for (const row of await page.$$(".row")) {
    const text = (await row.innerText()).split("\n");
    check(text[1] === "Done", `browser: ${text[0]} ${text[1]}${text[1] === "Done" ? "" : ": " + text.slice(3).join(" ")}`);
  }
  await new Promise((r) => setTimeout(r, 500));
  for (const d of downloads) await d.saveAs(join(dir, "out", d.suggestedFilename()));

  const long = join(dir, "long.mp4");
  ffmpeg("-f", "lavfi", "-i", "testsrc2=s=1920x1080:r=30:d=30", "-c:v", "libx264", "-preset", "ultrafast", long);
  await page.click("[data-ref=clear]");
  const again = page.waitForEvent("filechooser");
  await page.click("[data-ref=add]");
  await (await again).setFiles([long]);
  await page.waitForSelector(".row .badge:text('Ready')", { timeout: 60_000 });
  const before = downloads.length;
  await page.click("[data-ref=convert]");
  await page.waitForSelector(".row .badge:text-matches('Converting [1-9]')", { timeout: 60_000 });
  await page.click("[data-ref=cancel]");
  await page.waitForSelector(".row .badge:text('Cancelled')", { timeout: 60_000 });
  await new Promise((r) => setTimeout(r, 500));
  check(downloads.length === before, "browser: a cancelled conversion downloads nothing");
  check(errors.length === 0, `no page errors ${errors}`);

  for (const input of files) {
    const base = input.replace(/\.[^.]+$/, "");
    const name = base.split("/").pop();
    const ours = join(dir, "out", `${name}.tab5.mp4`);
    const theirs = join(dir, "out", `${name}.cli.mp4`);
    execFileSync(cli, [input, "-o", theirs, "-y"], { stdio: "ignore" });
    const video = (f) =>
      probe(f, "-select_streams", "v", "-show_entries", "stream=codec_name,codec_tag_string,width,height,avg_frame_rate,time_base,nb_frames:stream_side_data=rotation").replace(/\n/g, " ");
    check(video(ours) === video(theirs), `${name}: same video stream as the CLI (${video(ours)})`);
    const audio = (f) => probe(f, "-select_streams", "a", "-show_entries", "stream=codec_name,channels,sample_rate").split("\n");
    const [a, b] = [audio(ours), audio(theirs)];
    const rateOk = a[1] === b[1] || (Number(b[1]) < 44100 && Number(a[1]) >= 44100);
    check(a[0] === b[0] && a[2] === b[2] && rateOk, `${name}: audio ${a.join(" ")} (CLI ${b.join(" ")})`);
    if (a[0]) {
      const lag = interleaveLag(ours);
      check(lag < 0.5, `${name}: audio and video interleaved within ${lag.toFixed(3)} s (CLI ${interleaveLag(theirs).toFixed(3)} s)`);
    }
    const decode = execFileSync("ffmpeg", ["-v", "error", "-i", ours, "-f", "null", "-"]).toString();
    check(decode === "", `${name}: decodes without errors`);
    const size = probe(theirs, "-select_streams", "v", "-show_entries", "stream=width,height").replace("\n", "x");
    const raw = (f, o) => ffmpeg("-noautorotate", "-i", f, "-f", "rawvideo", "-pix_fmt", "yuv420p", o);
    raw(ours, join(dir, "a.yuv"));
    raw(theirs, join(dir, "b.yuv"));
    const yuv = (f) => ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", size, "-i", f];
    const stderr = spawnSync(
      "ffmpeg",
      ["-hide_banner", ...yuv(join(dir, "a.yuv")), ...yuv(join(dir, "b.yuv")), "-lavfi", "psnr", "-f", "null", "-"],
      { encoding: "utf8" },
    ).stderr;
    const psnr = Number(/PSNR y:([0-9.]+)/.exec(stderr)?.[1]);
    check(psnr > 30, `${name}: luma PSNR against the CLI ${psnr.toFixed(1)} dB`);
  }
} finally {
  await browser.close();
  server.httpServer.close();
  if (failures.length === 0) rmSync(dir, { recursive: true });
  else console.log(`kept ${dir}: ${readdirSync(dir).join(" ")}`);
}
console.log(failures.length ? `${failures.length} failed` : "all passed");
process.exit(failures.length ? 1 : 0);
