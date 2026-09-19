// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import init, { Downscaler, YuvScaler } from "../wasm/tab5conv.js";

export type ScaleRequest =
  | { id: number; frame: VideoFrame; width: number; height: number }
  | { id: number; frame: VideoFrame; geometry: Float64Array; matrix: number; fullRange: boolean };
export type ScaleReply = { id: number; data: ArrayBuffer } | { id: number; error: string };

const ready = init();
let full: OffscreenCanvasRenderingContext2D | null = null;
let scaler: { key: string; downscaler: Downscaler } | null = null;
let yuv: YuvScaler | null = null;

function rgba(frame: VideoFrame, width: number, height: number): Uint8Array {
  const [fw, fh] = [frame.displayWidth, frame.displayHeight];
  if (full?.canvas.width !== fw || full.canvas.height !== fh) {
    full = new OffscreenCanvas(fw, fh).getContext("2d")!;
  }
  full.drawImage(frame, 0, 0);
  const key = `${fw}x${fh}>${width}x${height}`;
  if (scaler?.key !== key) {
    scaler?.downscaler.free();
    scaler = { key, downscaler: new Downscaler(fw, fh, width, height) };
  }
  return scaler.downscaler.rgba(new Uint8Array(full.getImageData(0, 0, fw, fh).data.buffer));
}

async function planar(frame: VideoFrame, geometry: Float64Array, matrix: number, fullRange: boolean): Promise<Uint8Array> {
  const rect = frame.visibleRect!;
  const data = new Uint8Array(frame.allocationSize({ rect }));
  const layout = await frame.copyTo(data, { rect });
  yuv ??= new YuvScaler(geometry);
  const planes = new Uint32Array([rect.width, rect.height, ...layout.flatMap((p) => [p.offset, p.stride])]);
  return yuv.convert(frame.format!, data, planes, new Uint8Array([matrix, fullRange ? 1 : 0]));
}

self.onmessage = async (event: MessageEvent<ScaleRequest>) => {
  await ready;
  const request = event.data;
  const { id, frame } = request;
  try {
    const out =
      "geometry" in request
        ? await planar(frame, request.geometry, request.matrix, request.fullRange)
        : rgba(frame, request.width, request.height);
    frame.close();
    const data = out.buffer as ArrayBuffer;
    self.postMessage({ id, data } satisfies ScaleReply, { transfer: [data] });
  } catch (err) {
    frame.close();
    self.postMessage({ id, error: err instanceof Error ? err.message : String(err) } satisfies ScaleReply);
  }
};
