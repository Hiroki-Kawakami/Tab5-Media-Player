// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import type { EncoderReply, EncoderRequest, Pictures } from "./encoder.worker";
import type { ScaleReply, ScaleRequest } from "./scale.worker";

type Pending = { resolve: (reply: EncoderReply) => void; reject: (err: Error) => void };

const KINDS: Record<Exclude<EncoderReply["type"], "error">, EncoderRequest["type"]> = {
  model: "analyze",
  encoded: "encode",
  pictures: "mpeg2",
};

export class EncoderPool {
  private readonly workers: Worker[];
  private readonly pending = new Map<string, Pending>();
  readonly busy: Record<string, number> = {};
  private nextId = 0;

  constructor(size: number) {
    this.workers = Array.from({ length: size }, () => {
      const worker = new Worker(new URL("./encoder.worker.ts", import.meta.url), { type: "module" });
      worker.onmessage = (event: MessageEvent<EncoderReply>) => {
        const reply = event.data;
        const kind = reply.type === "error" ? reply.kind : KINDS[reply.type];
        if (reply.type !== "error") this.busy[kind] = (this.busy[kind] ?? 0) + reply.ms;
        const key = `${kind}:${reply.id}`;
        const pending = this.pending.get(key);
        if (!pending) return;
        this.pending.delete(key);
        if (reply.type === "error") pending.reject(new Error(reply.message));
        else pending.resolve(reply);
      };
      return worker;
    });
  }

  get size(): number {
    return this.workers.length;
  }

  private send(worker: number, request: EncoderRequest, transfer: Transferable[]): Promise<EncoderReply> {
    return new Promise((resolve, reject) => {
      this.pending.set(`${request.type}:${request.id}`, { resolve, reject });
      this.workers[worker % this.workers.length].postMessage(request, transfer);
    });
  }

  async analyze(id: number, pixels: ArrayBuffer, yuv: boolean, width: number, height: number): Promise<Uint32Array> {
    const reply = await this.send(id, { type: "analyze", id, pixels, yuv, width, height }, [pixels]);
    if (reply.type !== "model") throw new Error("unexpected encoder reply");
    return reply.words;
  }

  async encode(
    id: number,
    quality: number,
    limits: { minQuality: number; maxFrame: number; optimal: boolean },
  ): Promise<{ data: Uint8Array; quality: number; fits: boolean }> {
    const reply = await this.send(id, { type: "encode", id, quality, ...limits }, []);
    if (reply.type !== "encoded") throw new Error("unexpected encoder reply");
    return reply;
  }

  async mpeg2(worker: number, config: Float64Array, gop: number, yuv: ArrayBuffer | null): Promise<Pictures> {
    const id = this.nextId++;
    const reply = await this.send(worker, { type: "mpeg2", id, config, gop, yuv }, yuv ? [yuv] : []);
    if (reply.type !== "pictures") throw new Error("unexpected encoder reply");
    return reply;
  }

  close(): void {
    this.workers.forEach((w) => w.terminate());
    for (const pending of this.pending.values()) pending.reject(new Error("cancelled"));
    this.pending.clear();
  }
}

export class ScalePool {
  private readonly workers: Worker[];
  readonly busy: Record<string, number> = {};
  private readonly pending = new Map<number, { resolve: (data: ArrayBuffer) => void; reject: (err: Error) => void }>();
  private nextId = 0;

  constructor(size: number) {
    this.workers = Array.from({ length: size }, () => {
      const worker = new Worker(new URL("./scale.worker.ts", import.meta.url), { type: "module" });
      worker.onmessage = (event: MessageEvent<ScaleReply>) => {
        const reply = event.data;
        const pending = this.pending.get(reply.id);
        if (!pending) return;
        this.pending.delete(reply.id);
        if ("error" in reply) {
          pending.reject(new Error(reply.error));
          return;
        }
        for (const [stage, ms] of Object.entries(reply.times)) this.busy[stage] = (this.busy[stage] ?? 0) + ms;
        pending.resolve(reply.data);
      };
      return worker;
    });
  }

  get size(): number {
    return this.workers.length;
  }

  get inflight(): number {
    return this.pending.size;
  }

  private send(request: ScaleRequest): Promise<ArrayBuffer> {
    return new Promise((resolve, reject) => {
      this.pending.set(request.id, { resolve, reject });
      this.workers[request.id % this.workers.length].postMessage(request, [request.frame]);
    });
  }

  scale(frame: VideoFrame, width: number, height: number): Promise<ArrayBuffer> {
    return this.send({ id: this.nextId++, frame, width, height });
  }

  yuv(frame: VideoFrame, geometry: Float64Array, matrix: number, fullRange: boolean): Promise<ArrayBuffer> {
    return this.send({ id: this.nextId++, frame, geometry, matrix, fullRange });
  }

  close(): void {
    this.workers.forEach((w) => w.terminate());
    for (const pending of this.pending.values()) pending.reject(new Error("cancelled"));
    this.pending.clear();
  }
}
