// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import init, { MjpegWorker, Mpeg2Worker } from "../wasm/tab5conv.js";

export type EncoderRequest =
  | { type: "analyze"; id: number; pixels: ArrayBuffer; yuv: boolean; width: number; height: number }
  | { type: "encode"; id: number; quality: number; minQuality: number; maxFrame: number; optimal: boolean }
  | { type: "mpeg2"; id: number; config: Float64Array; gop: number; yuv: ArrayBuffer | null };

export interface Pictures {
  data: Uint8Array;
  sizes: Uint32Array;
  indexes: Uint32Array;
  kinds: Uint8Array;
}

export type EncoderReply =
  | { type: "model"; id: number; ms: number; words: Uint32Array }
  | { type: "encoded"; id: number; ms: number; data: Uint8Array; quality: number; fits: boolean }
  | ({ type: "pictures"; id: number; ms: number } & Pictures)
  | { type: "error"; id: number; kind: EncoderRequest["type"]; message: string };

const ready = init();
let mjpeg: MjpegWorker | null = null;
let mpeg2: Mpeg2Worker | null = null;

self.onmessage = async (event: MessageEvent<EncoderRequest>) => {
  await ready;
  const m = event.data;
  const post = (reply: EncoderReply, transfer: Transferable[]) => self.postMessage(reply, { transfer });
  const start = performance.now();
  const ms = () => performance.now() - start;
  try {
    if (m.type === "analyze") {
      mjpeg ??= new MjpegWorker();
      const pixels = new Uint8Array(m.pixels);
      const words = m.yuv
        ? mjpeg.analyzeYuv(m.id, pixels, m.width, m.height)
        : mjpeg.analyze(m.id, pixels, m.width, m.height);
      post({ type: "model", id: m.id, ms: ms(), words }, [words.buffer]);
    } else if (m.type === "encode") {
      mjpeg ??= new MjpegWorker();
      const frame = mjpeg.encode(m.id, m.quality, m.minQuality, m.maxFrame, m.optimal);
      const data = frame.data;
      post({ type: "encoded", id: m.id, ms: ms(), data, quality: frame.quality, fits: frame.fits }, [data.buffer]);
      frame.free();
    } else {
      mpeg2 ??= new Mpeg2Worker(m.config);
      const out = m.yuv ? mpeg2.push(m.gop, new Uint8Array(m.yuv)) : mpeg2.finish(m.gop);
      const pictures = { data: out.data, sizes: out.sizes, indexes: out.indexes, kinds: out.kinds };
      out.free();
      post({ type: "pictures", id: m.id, ms: ms(), ...pictures }, [pictures.data.buffer, pictures.sizes.buffer, pictures.indexes.buffer, pictures.kinds.buffer]);
    }
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    post({ type: "error", id: m.id, kind: m.type, message }, []);
  }
};
