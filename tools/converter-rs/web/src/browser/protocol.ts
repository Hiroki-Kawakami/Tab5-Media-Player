// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import type { Help, Outcome, Plan, PresetInfo, Settings } from "../backend";

export interface Requests {
  presets: { args: Record<string, never>; result: PresetInfo[] };
  help: { args: Record<string, never>; result: Help };
  plan: { args: { files: File[]; keys: string[]; settings: Settings }; result: Plan };
  convert: {
    args: { file: File; settings: Settings; temp: string };
    result: Outcome;
  };
}

export type Method = keyof Requests;

export type Request = {
  [M in Method]: { id: number; method: M; args: Requests[M]["args"] };
}[Method];

export type Message =
  | Request
  | { id: number; method: "cancel" };

export type Reply =
  | { id: number; result: unknown }
  | { id: number; error: string }
  | { id: number; progress: number };

export const TEMP_DIR = "tab5conv";
