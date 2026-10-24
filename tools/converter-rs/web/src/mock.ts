// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import type {
  Backend,
  Defaults,
  DropHandlers,
  Input,
  Outcome,
  Plan,
  PlanItem,
  PresetInfo,
  Settings,
  Status,
} from "./backend";

const PRESETS: PresetInfo[] = [
  { name: "tiny", video: "h264,long=640,short=360,maxfps=30", audio: "aac,bitrate=128k" },
  { name: "small", video: "mpeg2,long=640,short=360,maxfps=30", audio: "aac,bitrate=128k" },
  { name: "default", video: "mjpeg", audio: "aac" },
  { name: "quality", video: "mjpeg,long=1280,short=720,maxfps=60,quality=80,bitrate=40M", audio: "aac" },
];
const SIZE = { long: "640", scale: "contain", maxfps: "30" };
const DEFAULTS: Defaults = {
  video: {
    mjpeg: {
      ...SIZE,
      long: "1280",
      short: "720",
      quality: "75",
      minquality: "30",
      bitrate: "24M",
      buffer: "1048576",
      maxframe: "1048576",
      huffman: "optimal",
      dedup: "yes",
      rotate: "90",
      rotatewhen: "landscape",
      rotatemeta: "yes",
    },
    mpeg2: { ...SIZE, qscale: "8", bframes: "2", keyint: "2", gop: "closed", hq: "yes" },
    h264: { ...SIZE, profile: "main", crf: "32", keyint: "4", preset: "medium" },
  },
  audio: { aac: { keep: "auto", bitrate: "160k" }, mp3: { keep: "auto", bitrate: "192k" }, none: {} },
};
const DURATION = 4;
const STEP_MS = 100;

function merged(base: string, explicit: string): string {
  const text = explicit.trim();
  if (!text) return base;
  const [codec, ...keys] = text.split(",");
  const [baseCodec, ...baseKeys] = base.split(",");
  if (codec !== baseCodec) return text;
  const names = new Set(keys.map((k) => k.split("=")[0]));
  return [codec, ...baseKeys.filter((k) => !names.has(k.split("=")[0])), ...keys].join(",");
}

function stem(name: string): string {
  const dot = name.lastIndexOf(".");
  return dot > 0 ? name.slice(0, dot) : name;
}

export class MockBackend implements Backend {
  readonly name = "mock";
  readonly outputs = "folder";
  readonly writes = { mpeg2Bitrate: true, openGop: true, mp3: true };
  readonly actions = [];
  private readonly cancelled = new Set<string>();
  private picked = 0;

  async status(): Promise<Status> {
    return { ok: true, text: "Mock backend", title: "Nothing is converted" };
  }

  async presets(): Promise<PresetInfo[]> {
    return PRESETS;
  }

  async defaults(): Promise<Defaults> {
    return DEFAULTS;
  }

  async pickInputs(): Promise<Input[]> {
    this.picked += 1;
    return ["clip.mov", "holiday.mkv"].map((name) => ({
      key: `/videos/${this.picked}/${name}`,
      name,
    }));
  }

  async pickOutdir(): Promise<string | null> {
    return "/videos/out";
  }

  watchDrops(target: HTMLElement, handlers: DropHandlers): void {
    target.addEventListener("dragover", (event) => {
      event.preventDefault();
      handlers.hover(true);
    });
    target.addEventListener("dragleave", () => handlers.hover(false));
    target.addEventListener("drop", (event) => {
      event.preventDefault();
      handlers.hover(false);
      const files = Array.from(event.dataTransfer?.files ?? []);
      handlers.drop(files.map((file) => ({ key: `/dropped/${file.name}`, name: file.name })));
    });
  }

  async plan(inputs: Input[], settings: Settings): Promise<Plan> {
    const preset = PRESETS.find((p) => p.name === settings.preset);
    if (!preset) throw new Error(`unknown preset '${settings.preset}'`);
    const video = merged(preset.video, settings.video);
    const audio = merged(preset.audio, settings.audio);
    if (video.includes("error")) throw new Error(`${video.split(",")[0]}: unknown key 'error'`);
    const items = inputs.map((input): PlanItem => {
      const output = settings.outdir
        ? `${settings.outdir}/${stem(input.name)}.mp4`
        : input.key.replace(/[^/]*$/, `${stem(input.name)}.tab5.mp4`);
      if (input.name.includes("broken")) {
        return { input: input.key, output, error: "no video stream", exists: false };
      }
      return {
        input: input.key,
        output,
        video: `1920x1080 -> (mock) ${video}`,
        audio: `(mock) ${audio}`,
        sourceParts: ["1920×1080 H.264", "29.97 fps", "0:04"],
        outputParts: [`(mock) ${video.split(",")[0]}`, "29.97 fps", `(mock) ${audio.split(",")[0]}`],
        exists: input.name.includes("exists"),
        duration: DURATION,
      };
    });
    return { applied: { preset: preset.name, video, audio }, items };
  }

  async thumbnail(input: Input): Promise<string | null> {
    if (input.name.includes("broken")) return null;
    const canvas = new OffscreenCanvas(160, 90);
    const ctx = canvas.getContext("2d")!;
    const gradient = ctx.createLinearGradient(0, 0, 160, 90);
    gradient.addColorStop(0, "#2b5ce6");
    gradient.addColorStop(1, "#e6a82b");
    ctx.fillStyle = gradient;
    ctx.fillRect(0, 0, 160, 90);
    return URL.createObjectURL(await canvas.convertToBlob({ type: "image/jpeg" }));
  }

  async convert(
    input: Input,
    _output: string,
    _settings: Settings,
    progress: (seconds: number) => void,
  ): Promise<Outcome> {
    this.cancelled.delete(input.key);
    for (let t = 0; t <= DURATION * 1000; t += STEP_MS * 4) {
      if (this.cancelled.has(input.key)) return { kind: "cancelled" };
      progress(t / 1000);
      await new Promise((resolve) => setTimeout(resolve, STEP_MS));
    }
    if (input.name.includes("fail")) return { kind: "failed", message: "ffmpeg failed (mock)" };
    return { kind: "done", report: ["mjpeg: (mock) 40 frames, 80.0 KiB average", "warning: (mock) 1 frames went over the budget"] };
  }

  async cancel(input: Input): Promise<void> {
    this.cancelled.add(input.key);
  }

  async confirm(message: string): Promise<boolean> {
    return window.confirm(message);
  }
}
