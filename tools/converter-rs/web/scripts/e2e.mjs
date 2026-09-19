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
mkdirSync(join(dir, "mpeg2"));
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
  "untagged.mp4": ["-f", "lavfi", "-i", "testsrc2=s=1280x720:r=30:d=2", "-f", "lavfi", "-i", "sine=r=48000:d=2", "-c:v", "libx264", "-c:a", "aac"],
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
  const downloads = [];
  page.on("download", (d) => downloads.push(d));
  const convertAll = async (list, out, label) => {
    await page.click("[data-ref=clear]");
    const chooser = page.waitForEvent("filechooser");
    await page.click("[data-ref=add]");
    await (await chooser).setFiles(list);
    await page.waitForFunction(
      (n) => [...document.querySelectorAll(".row .badge")].filter((b) => /Ready|Cannot/.test(b.textContent)).length === n,
      list.length,
      { timeout: 60_000 },
    );
    const before = downloads.length;
    const start = Date.now();
    await page.click("[data-ref=convert]");
    await page.waitForFunction(
      () => [...document.querySelectorAll(".row .badge")].every((b) => /Done|Failed|Cannot/.test(b.textContent)),
      null,
      { timeout: 600_000 },
    );
    console.log(`browser ${label} conversion: ${(Date.now() - start) / 1000} s`);
    for (const row of await page.$$(".row")) {
      const text = (await row.innerText()).split("\n");
      check(text[1] === "Done", `browser ${label}: ${text[0]} ${text[1]}${text[1] === "Done" ? "" : ": " + text.slice(3).join(" ")}`);
    }
    await new Promise((r) => setTimeout(r, 500));
    for (const d of downloads.slice(before)) await d.saveAs(join(dir, out, d.suggestedFilename()));
  };
  await convertAll(files, "out", "mjpeg");
  const mpeg2Files = ["hd.mp4", "portrait.mkv", "sd.webm", "long.mkv", "untagged.mp4", "rotated.mov"].map((n) => join(dir, n));
  await page.selectOption("[data-ref=preset]", "small");
  await convertAll(mpeg2Files, "mpeg2", "mpeg2");
  await page.selectOption("[data-ref=preset]", "default");

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

  const compare = (input, out, cliArgs, label) => {
    const base = input.replace(/\.[^.]+$/, "");
    const name = base.split("/").pop();
    const ours = join(dir, out, `${name}.tab5.mp4`);
    const theirs = join(dir, out, `${name}.cli.mp4`);
    execFileSync(cli, [input, "-o", theirs, "-y", ...cliArgs], { stdio: "ignore" });
    const tag = `${label} ${name}`;
    const video = (f) =>
      probe(f, "-select_streams", "v", "-show_entries", "stream=codec_name,codec_tag_string,width,height,avg_frame_rate,time_base,nb_frames:stream_side_data=rotation").replace(/\n/g, " ");
    check(video(ours) === video(theirs), `${tag}: same video stream as the CLI (${video(ours)})`);
    const audio = (f) => probe(f, "-select_streams", "a", "-show_entries", "stream=codec_name,channels,sample_rate").split("\n");
    const [a, b] = [audio(ours), audio(theirs)];
    const rateOk = a[1] === b[1] || (Number(b[1]) < 44100 && Number(a[1]) >= 44100);
    check(a[0] === b[0] && a[2] === b[2] && rateOk, `${tag}: audio ${a.join(" ")} (CLI ${b.join(" ")})`);
    if (a[0]) {
      const lag = interleaveLag(ours);
      check(lag < 0.5, `${tag}: audio and video interleaved within ${lag.toFixed(3)} s (CLI ${interleaveLag(theirs).toFixed(3)} s)`);
    }
    const decode = execFileSync("ffmpeg", ["-v", "error", "-i", ours, "-f", "null", "-"]).toString();
    check(decode === "", `${tag}: decodes without errors`);
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
    const db = "([0-9.]+|inf)";
    const match = new RegExp(`PSNR y:${db} u:${db} v:${db}`).exec(stderr) ?? [];
    const [y, u, v] = match.slice(1, 4).map((x) => (x === "inf" ? Infinity : Number(x)));
    check(Math.min(y, u, v) > 35, `${tag}: PSNR against the CLI y ${y.toFixed(1)} u ${u.toFixed(1)} v ${v.toFixed(1)} dB`);
    return { ours, theirs };
  };
  for (const input of files) compare(input, "out", [], "mjpeg");
  for (const input of mpeg2Files) {
    const { ours, theirs } = compare(input, "mpeg2", ["--preset", "small"], "mpeg2");
    const bytes = (f) => Number(probe(f, "-select_streams", "v", "-show_entries", "stream=bit_rate"));
    const ratio = bytes(ours) / bytes(theirs);
    console.log(`     mpeg2 ${ours.split("/").pop()}: video bitrate ${(ratio * 100).toFixed(0)}% of the CLI's`);
  }
} finally {
  await browser.close();
  server.httpServer.close();
  if (failures.length === 0) rmSync(dir, { recursive: true });
  else console.log(`kept ${dir}: ${readdirSync(dir).join(" ")}`);
}
console.log(failures.length ? `${failures.length} failed` : "all passed");
process.exit(failures.length ? 1 : 0);
