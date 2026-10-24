// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import { ask, open } from "@tauri-apps/plugin-dialog";

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
} from "./backend";

interface ToolsStatus {
  configured: string | null;
  path: string | null;
  probe: string | null;
  version: string | null;
  error: string | null;
}

const VIDEO_EXTENSIONS = [
  "mp4", "m4v", "mov", "mkv", "webm", "avi", "mpg", "mpeg", "ts", "m2ts", "mts",
  "wmv", "flv", "3gp", "ogv",
];

function toStatus(tools: ToolsStatus): Status {
  const paths = [tools.path, tools.probe].filter((p): p is string => !!p);
  if (tools.version) {
    const where = tools.configured ? "in the chosen folder" : "found";
    return { ok: true, text: tools.version, title: `${tools.version} ${where}`, lines: paths };
  }
  return {
    ok: false,
    text: "ffmpeg not available",
    title: tools.error ?? "ffmpeg is not available",
    lines: paths.length > 0 ? paths : ["Install ffmpeg, or choose the folder that contains ffmpeg and ffprobe."],
  };
}

function toInput(path: string): Input {
  return { key: path, name: path.split(/[\\/]/).pop() ?? path };
}

export class NativeBackend implements Backend {
  readonly name = "native";
  readonly outputs = "folder";
  readonly writes = { mpeg2Bitrate: true, openGop: true, mp3: true };
  private readonly listeners = new Map<string, (seconds: number) => void>();
  private readonly ready: Promise<unknown>;

  readonly actions = [
    {
      label: "Choose folder…",
      run: async (): Promise<Status | null> => {
        const dir = await open({ directory: true, title: "Folder with ffmpeg and ffprobe" });
        if (typeof dir !== "string") return null;
        return toStatus(await invoke<ToolsStatus>("set_ffmpeg_dir", { dir }));
      },
    },
    {
      label: "Find automatically",
      run: async (): Promise<Status | null> =>
        toStatus(await invoke<ToolsStatus>("set_ffmpeg_dir", { dir: null })),
    },
  ];

  constructor() {
    this.ready = listen<{ key: string; seconds: number }>("progress", (event) => {
      this.listeners.get(event.payload.key)?.(event.payload.seconds);
    });
  }

  async status(): Promise<Status> {
    return toStatus(await invoke<ToolsStatus>("tools_status"));
  }

  presets(): Promise<PresetInfo[]> {
    return invoke("presets");
  }

  defaults(): Promise<Defaults> {
    return invoke("defaults");
  }

  async pickInputs(): Promise<Input[]> {
    const picked = await open({
      multiple: true,
      title: "Videos to convert",
      filters: [{ name: "Videos", extensions: VIDEO_EXTENSIONS }],
    });
    return (picked ?? []).map(toInput);
  }

  async pickOutdir(): Promise<string | null> {
    const dir = await open({ directory: true, title: "Output folder" });
    return typeof dir === "string" ? dir : null;
  }

  watchDrops(_target: HTMLElement, handlers: DropHandlers): void {
    void getCurrentWebview().onDragDropEvent((event) => {
      switch (event.payload.type) {
        case "enter":
        case "over":
          handlers.hover(true);
          break;
        case "leave":
          handlers.hover(false);
          break;
        case "drop":
          handlers.hover(false);
          handlers.drop(event.payload.paths.map(toInput));
          break;
      }
    });
  }

  plan(inputs: Input[], settings: Settings): Promise<Plan> {
    return invoke("plan", { inputs: inputs.map((i) => i.key), settings });
  }

  async thumbnail(input: Input): Promise<string | null> {
    const jpeg = await invoke<ArrayBuffer>("thumbnail", { input: input.key });
    return URL.createObjectURL(new Blob([jpeg], { type: "image/jpeg" }));
  }

  async convert(
    input: Input,
    output: string,
    settings: Settings,
    progress: (seconds: number) => void,
  ): Promise<Outcome> {
    await this.ready;
    this.listeners.set(input.key, progress);
    try {
      return await invoke<Outcome>("convert", { key: input.key, input: input.key, output, settings });
    } finally {
      this.listeners.delete(input.key);
    }
  }

  cancel(input: Input): Promise<void> {
    return invoke("cancel", { key: input.key });
  }

  confirm(message: string): Promise<boolean> {
    return ask(message, { title: "Tab5 Media Converter", kind: "warning" });
  }
}
