// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import "./style.css";

import type { Backend } from "./backend";
import { App } from "./ui";

async function createBackend(): Promise<Backend> {
  const mock = new URLSearchParams(location.search).get("backend") === "mock";
  if ("__TAURI_INTERNALS__" in window && !mock) {
    const { NativeBackend } = await import("./native");
    return new NativeBackend();
  }
  const { MockBackend } = await import("./mock");
  return new MockBackend();
}

await new App(await createBackend(), document.getElementById("app")!).start();
