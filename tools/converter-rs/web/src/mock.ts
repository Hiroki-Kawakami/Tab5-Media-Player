// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import type {
  Backend,
  DropHandlers,
  Help,
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
  readonly actions = [];
  private readonly cancelled = new Set<string>();
  private picked = 0;

  async status(): Promise<Status> {
    return { ok: true, text: "Mock backend", detail: "Nothing is converted" };
  }

  async presets(): Promise<PresetInfo[]> {
    return PRESETS;
  }

  async help(): Promise<Help> {
    return { preset: "(mock) preset help", video: "(mock) video help", audio: "(mock) audio help" };
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
        exists: input.name.includes("exists"),
        duration: DURATION,
      };
    });
    return { applied: { preset: preset.name, video, audio }, items };
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
    return { kind: "done", report: [] };
  }

  async cancel(input: Input): Promise<void> {
    this.cancelled.add(input.key);
  }

  async confirm(message: string): Promise<boolean> {
    return window.confirm(message);
  }
}
