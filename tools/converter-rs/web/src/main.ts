// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import "./style.css";

import type { Backend } from "./backend";
import { App } from "./ui";

async function createBackend(): Promise<Backend> {
  const mock = new URLSearchParams(location.search).get("backend") === "mock";
  if (mock) {
    const { MockBackend } = await import("./mock");
    return new MockBackend();
  }
  if ("__TAURI_INTERNALS__" in window) {
    const { NativeBackend } = await import("./native");
    return new NativeBackend();
  }
  const { BrowserBackend } = await import("./browser/backend");
  return new BrowserBackend();
}

await new App(await createBackend(), document.getElementById("app")!).start();
