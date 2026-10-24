// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

export interface PresetInfo {
  name: string;
  video: string;
  audio: string;
}

export interface Defaults {
  video: Record<string, Record<string, string>>;
  audio: Record<string, Record<string, string>>;
}

export interface Settings {
  preset: string;
  video: string;
  audio: string;
  outdir: string | null;
}

export interface PlanItem {
  input: string;
  output?: string | null;
  skip?: string | null;
  video?: string | null;
  audio?: string | null;
  sourceParts?: string[] | null;
  outputParts?: string[] | null;
  error?: string | null;
  exists: boolean;
  duration?: number | null;
}

export interface Plan {
  applied: { preset: string; video: string; audio: string };
  items: PlanItem[];
}

export type Outcome =
  | { kind: "done"; report: string[] }
  | { kind: "cancelled" }
  | { kind: "failed"; message: string };

export interface Input {
  key: string;
  name: string;
}

export interface Status {
  ok: boolean;
  text: string;
  title?: string;
  lines?: string[];
}

export interface Writes {
  mpeg2Bitrate: boolean;
  openGop: boolean;
  mp3: boolean;
}

export interface Action {
  label: string;
  run(): Promise<Status | null>;
}

export interface DropHandlers {
  hover(active: boolean): void;
  drop(inputs: Input[]): void;
}

export interface Backend {
  readonly name: string;
  readonly outputs: "folder" | "download";
  readonly writes: Writes;
  readonly actions: Action[];
  status(): Promise<Status>;
  presets(): Promise<PresetInfo[]>;
  defaults(): Promise<Defaults>;
  pickInputs(): Promise<Input[]>;
  pickOutdir(): Promise<string | null>;
  watchDrops(target: HTMLElement, handlers: DropHandlers): void;
  plan(inputs: Input[], settings: Settings): Promise<Plan>;
  thumbnail(input: Input): Promise<string | null>;
  convert(
    input: Input,
    output: string,
    settings: Settings,
    progress: (seconds: number) => void,
  ): Promise<Outcome>;
  cancel(input: Input): Promise<void>;
  confirm(message: string): Promise<boolean>;
}
