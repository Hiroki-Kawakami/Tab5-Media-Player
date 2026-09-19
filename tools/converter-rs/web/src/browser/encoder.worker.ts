// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import init, { MjpegWorker } from "../wasm/tab5conv.js";

export type EncoderRequest =
  | { type: "analyze"; id: number; rgba: ArrayBuffer; width: number; height: number }
  | { type: "encode"; id: number; quality: number; minQuality: number; maxFrame: number; optimal: boolean };

export type EncoderReply =
  | { type: "model"; id: number; words: Uint32Array }
  | { type: "encoded"; id: number; data: Uint8Array; quality: number; fits: boolean }
  | { type: "error"; id: number; kind: EncoderRequest["type"]; message: string };

const worker = init().then(() => new MjpegWorker());

self.onmessage = async (event: MessageEvent<EncoderRequest>) => {
  const w = await worker;
  const m = event.data;
  const post = (reply: EncoderReply, transfer: Transferable[]) => self.postMessage(reply, { transfer });
  try {
    if (m.type === "analyze") {
      const words = w.analyze(m.id, new Uint8Array(m.rgba), m.width, m.height);
      post({ type: "model", id: m.id, words }, [words.buffer]);
    } else {
      const frame = w.encode(m.id, m.quality, m.minQuality, m.maxFrame, m.optimal);
      const data = frame.data;
      post({ type: "encoded", id: m.id, data, quality: frame.quality, fits: frame.fits }, [data.buffer]);
      frame.free();
    }
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    post({ type: "error", id: m.id, kind: m.type, message }, []);
  }
};
