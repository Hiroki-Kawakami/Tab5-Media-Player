// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import type {
  Backend,
  Defaults,
  DropHandlers,
  Input,
  Outcome,
  Plan,
  PresetInfo,
  Settings,
  Status,
} from "../backend";
import { type Method, type Reply, type Requests, TEMP_DIR } from "./protocol";

const ACCEPT = "video/*,.mkv,.mov,.mp4,.m4v,.webm";
const THUMBNAIL_AT = 10;
const THUMBNAIL_LONG = 160;
const THUMBNAIL_TIMEOUT_MS = 10_000;

interface Call {
  resolve: (value: unknown) => void;
  reject: (err: Error) => void;
  progress?: (fraction: number) => void;
}

function once(target: HTMLVideoElement, event: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`no ${event}`)), THUMBNAIL_TIMEOUT_MS);
    target.addEventListener(event, () => (clearTimeout(timer), resolve()), { once: true });
    target.addEventListener("error", () => (clearTimeout(timer), reject(new Error("cannot play"))), { once: true });
  });
}

// Chrome's drawImage resamples with a 2x2 bilinear whatever the setting, so a
// single big reduction skips pixels; halving step by step keeps it smooth.
function shrink(video: HTMLVideoElement): OffscreenCanvas {
  const [w, h] = [video.videoWidth, video.videoHeight];
  const scale = Math.min(1, THUMBNAIL_LONG / Math.max(w, h));
  const target = [Math.max(1, Math.round(w * scale)), Math.max(1, Math.round(h * scale))];
  let source: CanvasImageSource = video;
  let [sw, sh] = [w, h];
  for (;;) {
    const [dw, dh] = sw / 2 > target[0] ? [Math.round(sw / 2), Math.round(sh / 2)] : target;
    const canvas = new OffscreenCanvas(dw, dh);
    canvas.getContext("2d")!.drawImage(source, 0, 0, dw, dh);
    if (dw === target[0] && dh === target[1]) return canvas;
    [source, sw, sh] = [canvas, dw, dh];
  }
}

function missingFeatures(): string[] {
  const needed: [string, boolean][] = [
    ["WebCodecs", typeof VideoDecoder !== "undefined" && typeof AudioEncoder !== "undefined"],
    ["OffscreenCanvas", typeof OffscreenCanvas !== "undefined"],
    ["Origin private file system", !!navigator.storage?.getDirectory],
  ];
  return needed.filter(([, ok]) => !ok).map(([name]) => name);
}

export class BrowserBackend implements Backend {
  readonly name = "browser";
  readonly outputs = "download";
  readonly writes = { mpeg2Bitrate: false, openGop: false, mp3: false };
  readonly actions = [];
  private readonly files = new Map<string, File>();
  private readonly engine = new Worker(new URL("./engine.worker.ts", import.meta.url), { type: "module" });
  private readonly calls = new Map<number, Call>();
  private readonly running = new Map<string, number>();
  private nextId = 1;
  private nextKey = 1;

  constructor() {
    this.engine.onmessage = (event: MessageEvent<Reply>) => {
      const reply = event.data;
      const call = this.calls.get(reply.id);
      if (!call) return;
      if ("progress" in reply) {
        call.progress?.(reply.progress);
        return;
      }
      this.calls.delete(reply.id);
      if ("error" in reply) call.reject(new Error(reply.error));
      else call.resolve(reply.result);
    };
    void this.clearTemp();
  }

  private call<M extends Method>(
    method: M,
    args: Requests[M]["args"],
    progress?: (fraction: number) => void,
    id = this.nextId++,
  ): Promise<Requests[M]["result"]> {
    return new Promise((resolve, reject) => {
      this.calls.set(id, { resolve: resolve as (value: unknown) => void, reject, progress });
      this.engine.postMessage({ id, method, args });
    });
  }

  private async temp(): Promise<FileSystemDirectoryHandle> {
    return (await navigator.storage.getDirectory()).getDirectoryHandle(TEMP_DIR, { create: true });
  }

  private async clearTemp(): Promise<void> {
    if (!navigator.storage?.getDirectory) return;
    await (await navigator.storage.getDirectory()).removeEntry(TEMP_DIR, { recursive: true }).catch(() => {});
  }

  async status(): Promise<Status> {
    const missing = missingFeatures();
    if (missing.length > 0) {
      return { ok: false, text: "This browser cannot convert", title: `Missing: ${missing.join(", ")}` };
    }
    return { ok: true, text: "Converts in this browser", title: "WebCodecs, MJPEG and MPEG-2" };
  }

  presets(): Promise<PresetInfo[]> {
    return this.call("presets", {});
  }

  defaults(): Promise<Defaults> {
    return this.call("defaults", {});
  }

  private add(files: File[]): Input[] {
    return files.map((file) => {
      const key = `file-${this.nextKey++}`;
      this.files.set(key, file);
      return { key, name: file.name };
    });
  }

  pickInputs(): Promise<Input[]> {
    return new Promise((resolve) => {
      const input = document.createElement("input");
      input.type = "file";
      input.multiple = true;
      input.accept = ACCEPT;
      input.addEventListener("change", () => resolve(this.add(Array.from(input.files ?? []))));
      input.addEventListener("cancel", () => resolve([]));
      input.click();
    });
  }

  async pickOutdir(): Promise<string | null> {
    return null;
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
      handlers.drop(this.add(Array.from(event.dataTransfer?.files ?? [])));
    });
  }

  plan(inputs: Input[], settings: Settings): Promise<Plan> {
    const files = inputs.map((input) => this.files.get(input.key)!);
    return this.call("plan", { files, keys: inputs.map((i) => i.key), settings });
  }

  async thumbnail(input: Input): Promise<string | null> {
    const file = this.files.get(input.key);
    if (!file) return null;
    const url = URL.createObjectURL(file);
    const video = document.createElement("video");
    video.muted = true;
    video.preload = "auto";
    try {
      const loaded = once(video, "loadedmetadata");
      video.src = url;
      await loaded;
      const seeked = once(video, "seeked");
      video.currentTime = Number.isFinite(video.duration) ? Math.min(THUMBNAIL_AT, video.duration / 2) : 0;
      await seeked;
      const blob = await shrink(video).convertToBlob({ type: "image/jpeg", quality: 0.85 });
      return URL.createObjectURL(blob);
    } finally {
      video.removeAttribute("src");
      video.load();
      URL.revokeObjectURL(url);
    }
  }

  async convert(
    input: Input,
    output: string,
    settings: Settings,
    progress: (seconds: number) => void,
  ): Promise<Outcome> {
    const file = this.files.get(input.key)!;
    const temp = `${input.key}.mp4`;
    const id = this.nextId++;
    this.running.set(input.key, id);
    const plan = await this.call("plan", { files: [file], keys: [input.key], settings });
    const duration = plan.items[0]?.duration ?? 0;
    try {
      const outcome = await this.call(
        "convert",
        { file, settings, temp },
        (fraction) => progress(fraction * duration),
        id,
      );
      if (outcome.kind === "done") await this.download(temp, output);
      return outcome;
    } finally {
      this.running.delete(input.key);
    }
  }

  private async download(temp: string, name: string): Promise<void> {
    const file = await (await (await this.temp()).getFileHandle(temp)).getFile();
    const url = URL.createObjectURL(file);
    const link = document.createElement("a");
    link.href = url;
    link.download = name;
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 60_000);
  }

  async cancel(input: Input): Promise<void> {
    const id = this.running.get(input.key);
    if (id !== undefined) this.engine.postMessage({ id, method: "cancel" });
  }

  async confirm(message: string): Promise<boolean> {
    return window.confirm(message);
  }
}
