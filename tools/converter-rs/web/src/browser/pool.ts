// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import type { EncoderReply, EncoderRequest } from "./encoder.worker";

type Pending = { resolve: (reply: EncoderReply) => void; reject: (err: Error) => void };

export class EncoderPool {
  private readonly workers: Worker[];
  private readonly pending = new Map<string, Pending>();

  constructor(size: number) {
    this.workers = Array.from({ length: size }, () => {
      const worker = new Worker(new URL("./encoder.worker.ts", import.meta.url), { type: "module" });
      worker.onmessage = (event: MessageEvent<EncoderReply>) => {
        const reply = event.data;
        const kind = reply.type === "error" ? reply.kind : reply.type === "model" ? "analyze" : "encode";
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

  private send(id: number, kind: EncoderRequest["type"], request: EncoderRequest, transfer: Transferable[]): Promise<EncoderReply> {
    return new Promise((resolve, reject) => {
      this.pending.set(`${kind}:${id}`, { resolve, reject });
      this.workers[id % this.workers.length].postMessage(request, transfer);
    });
  }

  async analyze(id: number, rgba: ArrayBuffer, width: number, height: number): Promise<Uint32Array> {
    const reply = await this.send(id, "analyze", { type: "analyze", id, rgba, width, height }, [rgba]);
    if (reply.type !== "model") throw new Error("unexpected encoder reply");
    return reply.words;
  }

  async encode(
    id: number,
    quality: number,
    limits: { minQuality: number; maxFrame: number; optimal: boolean },
  ): Promise<{ data: Uint8Array; quality: number; fits: boolean }> {
    const reply = await this.send(id, "encode", { type: "encode", id, quality, ...limits }, []);
    if (reply.type !== "encoded") throw new Error("unexpected encoder reply");
    return reply;
  }

  close(): void {
    this.workers.forEach((w) => w.terminate());
    for (const pending of this.pending.values()) pending.reject(new Error("cancelled"));
    this.pending.clear();
  }
}
