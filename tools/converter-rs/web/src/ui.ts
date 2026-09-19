// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import type { Backend, Input, Outcome, PlanItem, PresetInfo, Settings, Status } from "./backend";

type RowState = "idle" | "running" | "done" | "failed" | "cancelled";

interface Row {
  input: Input;
  plan?: PlanItem;
  state: RowState;
  fraction: number;
  message?: string;
  report?: string[];
}

interface RowElements {
  badge: HTMLElement;
  bar: HTMLProgressElement;
}

const PLAN_DELAY_MS = 250;

const TEMPLATE = `
<header class="bar">
  <h1>Tab5 Media Converter</h1>
  <div class="tools">
    <span class="status" data-ref="status"></span>
    <span class="actions" data-ref="actions"></span>
  </div>
</header>
<main>
  <section class="settings">
    <label class="field"><span>Preset</span><select data-ref="preset"></select></label>
    <label class="field"><span>Video</span><input data-ref="video" spellcheck="false" autocomplete="off" /></label>
    <label class="field"><span>Audio</span><input data-ref="audio" spellcheck="false" autocomplete="off" /></label>
    <div class="field" data-ref="outputField">
      <span>Output</span>
      <div class="output">
        <label><input type="radio" name="output" data-ref="outNext" checked /> Next to each input</label>
        <label><input type="radio" name="output" data-ref="outDir" /> Folder</label>
        <button type="button" data-ref="chooseDir">Choose…</button>
        <span class="path" data-ref="outdir"></span>
      </div>
    </div>
    <p class="applied" data-ref="applied"></p>
    <details>
      <summary>Option reference</summary>
      <pre data-ref="help"></pre>
    </details>
  </section>
  <section class="files" data-ref="drop">
    <div class="toolbar">
      <button type="button" data-ref="add">Add videos…</button>
      <button type="button" data-ref="clear">Clear</button>
      <span class="spacer"></span>
      <button type="button" data-ref="cancel">Cancel</button>
      <button type="button" class="primary" data-ref="convert">Convert</button>
    </div>
    <ul class="list" data-ref="list"></ul>
    <p class="empty" data-ref="empty">Drop videos here, or use Add videos…</p>
  </section>
</main>
`;

function message(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

function element<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className: string,
  text?: string,
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

export class App {
  private rows: Row[] = [];
  private presets: PresetInfo[] = [];
  private settings: Settings = { preset: "default", video: "", audio: "", outdir: null };
  private planError: string | null = null;
  private applied = "";
  private planSeq = 0;
  private plannedSeq = 0;
  private planTimer: number | undefined;
  private running: Row | null = null;
  private stopping = false;
  private rowElements = new Map<Row, RowElements>();
  private readonly refs: Record<string, HTMLElement>;

  constructor(
    private readonly backend: Backend,
    root: HTMLElement,
  ) {
    root.innerHTML = TEMPLATE;
    this.refs = Object.fromEntries(
      Array.from(root.querySelectorAll<HTMLElement>("[data-ref]")).map((node) => [node.dataset.ref!, node]),
    );
    this.bind();
  }

  private ref<T extends HTMLElement>(name: string): T {
    return this.refs[name] as T;
  }

  async start(): Promise<void> {
    const [presets, help, status] = await Promise.all([
      this.backend.presets(),
      this.backend.help(),
      this.backend.status(),
    ]);
    this.presets = presets;
    const select = this.ref<HTMLSelectElement>("preset");
    for (const preset of presets) {
      select.append(new Option(preset.name, preset.name, false, preset.name === this.settings.preset));
    }
    this.ref("help").textContent = [help.preset, help.video, help.audio].join("\n");
    this.ref("outputField").hidden = this.backend.outputs !== "folder";
    this.showStatus(status);
    for (const action of this.backend.actions) {
      const button = element("button", "", action.label);
      button.type = "button";
      button.addEventListener("click", async () => {
        const status = await action.run().catch((err) => ({ ok: false, text: message(err) }));
        if (status) {
          this.showStatus(status);
          this.schedulePlan();
        }
      });
      this.ref("actions").append(button);
    }
    this.render();
    this.schedulePlan();
  }

  private bind(): void {
    this.ref<HTMLSelectElement>("preset").addEventListener("change", (event) => {
      this.settings.preset = (event.target as HTMLSelectElement).value;
      this.settingsChanged();
    });
    for (const name of ["video", "audio"] as const) {
      this.ref<HTMLInputElement>(name).addEventListener("input", (event) => {
        this.settings[name] = (event.target as HTMLInputElement).value;
        this.settingsChanged();
      });
    }
    this.ref("outNext").addEventListener("change", () => {
      this.settings.outdir = null;
      this.settingsChanged();
    });
    this.ref("outDir").addEventListener("change", () => void this.chooseOutdir());
    this.ref("chooseDir").addEventListener("click", () => void this.chooseOutdir());
    this.ref("add").addEventListener("click", async () => this.add(await this.backend.pickInputs()));
    this.ref("clear").addEventListener("click", () => {
      this.rows = [];
      this.schedulePlan();
    });
    this.ref("convert").addEventListener("click", () => void this.convertAll());
    this.ref("cancel").addEventListener("click", () => void this.cancel());
    this.backend.watchDrops(this.ref("drop"), {
      hover: (active) => this.ref("drop").classList.toggle("hover", active && !this.running),
      drop: (inputs) => {
        if (!this.running) this.add(inputs);
      },
    });
  }

  private showStatus(status: Status): void {
    const node = this.ref("status");
    node.textContent = status.text;
    node.title = status.detail ?? "";
    node.classList.toggle("bad", !status.ok);
  }

  private async chooseOutdir(): Promise<void> {
    const dir = await this.backend.pickOutdir();
    if (dir) this.settings.outdir = dir;
    this.settingsChanged();
  }

  private add(inputs: Input[]): void {
    const known = new Set(this.rows.map((row) => row.input.key));
    for (const input of inputs) {
      if (known.has(input.key)) continue;
      known.add(input.key);
      this.rows.push({ input, state: "idle", fraction: 0 });
    }
    this.schedulePlan();
  }

  private remove(row: Row): void {
    this.rows = this.rows.filter((r) => r !== row);
    this.schedulePlan();
  }

  private settingsChanged(): void {
    for (const row of this.rows) {
      if (row.state !== "running") {
        row.state = "idle";
        row.message = undefined;
        row.report = undefined;
      }
    }
    this.schedulePlan();
  }

  private schedulePlan(): void {
    this.planSeq += 1;
    window.clearTimeout(this.planTimer);
    this.planTimer = window.setTimeout(() => void this.refreshPlan(), PLAN_DELAY_MS);
    this.render();
  }

  private async refreshPlan(): Promise<void> {
    const seq = this.planSeq;
    const rows = [...this.rows];
    const settings = { ...this.settings };
    try {
      const plan = await this.backend.plan(
        rows.map((row) => row.input),
        settings,
      );
      if (seq !== this.planSeq) return;
      this.plannedSeq = seq;
      rows.forEach((row, i) => (row.plan = plan.items[i]));
      this.planError = null;
      const { preset, video, audio } = plan.applied;
      this.applied = `preset: ${preset} (--video "${video}" --audio "${audio}")`;
    } catch (err) {
      if (seq !== this.planSeq) return;
      this.plannedSeq = seq;
      rows.forEach((row) => (row.plan = undefined));
      this.planError = message(err);
    }
    this.render();
  }

  private convertible(row: Row): boolean {
    const plan = row.plan;
    return !!plan && !plan.error && !plan.skip && !!plan.output && row.state !== "done";
  }

  private async convertAll(): Promise<void> {
    const queue = this.rows.filter((row) => this.convertible(row));
    if (queue.length === 0 || this.running) return;
    const existing = queue.filter((row) => row.plan!.exists).map((row) => row.plan!.output);
    if (existing.length > 0) {
      const replace = await this.backend.confirm(
        `These files already exist and will be replaced:\n\n${existing.join("\n")}`,
      );
      if (!replace) return;
    }
    const settings = { ...this.settings };
    this.stopping = false;
    for (const row of queue) {
      if (this.stopping) break;
      this.running = row;
      Object.assign(row, { state: "running", fraction: 0, message: undefined, report: undefined });
      this.render();
      const outcome: Outcome = await this.backend
        .convert(row.input, row.plan!.output!, settings, (seconds) => this.progress(row, seconds))
        .catch((err) => ({ kind: "failed", message: message(err) }));
      switch (outcome.kind) {
        case "done":
          Object.assign(row, { state: "done", fraction: 1, report: outcome.report });
          row.plan!.exists = true;
          break;
        case "cancelled":
          row.state = "cancelled";
          break;
        case "failed":
          Object.assign(row, { state: "failed", message: outcome.message });
          break;
      }
    }
    this.running = null;
    this.render();
  }

  private async cancel(): Promise<void> {
    if (!this.running) return;
    this.stopping = true;
    await this.backend.cancel(this.running.input);
  }

  private progress(row: Row, seconds: number): void {
    const duration = row.plan?.duration;
    if (!duration) return;
    row.fraction = Math.min(1, Math.max(0, seconds / duration));
    const elements = this.rowElements.get(row);
    if (elements) {
      elements.bar.value = row.fraction;
      elements.badge.textContent = this.badge(row);
    }
  }

  private badge(row: Row): string {
    const plan = row.plan;
    switch (row.state) {
      case "running":
        return plan?.duration ? `Converting ${Math.floor(row.fraction * 100)}%` : "Converting…";
      case "done":
        return "Done";
      case "failed":
        return "Failed";
      case "cancelled":
        return "Cancelled";
      case "idle":
        if (this.planError) return "—";
        if (!plan) return "Checking…";
        if (plan.error) return "Cannot convert";
        if (plan.skip) return "Skipped";
        return plan.exists ? "Ready (replaces a file)" : "Ready";
    }
  }

  private renderRow(row: Row): HTMLLIElement {
    const plan = row.plan;
    const item = element("li", `row ${row.state}${plan?.error ? " error" : ""}`);
    const head = element("div", "head");
    const badge = element("span", "badge", this.badge(row));
    head.append(element("strong", "", row.input.name), badge);
    if (!this.running) {
      const remove = element("button", "remove", "×");
      remove.type = "button";
      remove.title = "Remove";
      remove.addEventListener("click", () => this.remove(row));
      head.append(remove);
    }
    item.append(head);
    if (plan?.output) item.append(element("div", "detail", `→ ${plan.output}`));
    if (plan?.video) item.append(element("div", "detail", `video: ${plan.video}`));
    if (plan?.audio) item.append(element("div", "detail", `audio: ${plan.audio}`));
    const problem = row.message ?? plan?.error ?? (plan?.skip ? `skipped: ${plan.skip}` : undefined);
    if (problem) item.append(element("pre", "problem", problem));
    const bar = element("progress", "");
    bar.max = 1;
    bar.value = row.fraction;
    bar.hidden = row.state !== "running";
    item.append(bar);
    if (row.report?.length) item.append(element("pre", "report", row.report.join("\n")));
    this.rowElements.set(row, { badge, bar });
    return item;
  }

  private render(): void {
    const busy = !!this.running;
    const preset = this.presets.find((p) => p.name === this.settings.preset);
    const video = this.ref<HTMLInputElement>("video");
    const audio = this.ref<HTMLInputElement>("audio");
    video.placeholder = preset ? `${preset.video}  (preset)` : "";
    audio.placeholder = preset ? `${preset.audio}  (preset)` : "";
    for (const name of ["preset", "video", "audio", "outNext", "outDir", "chooseDir", "add", "clear"]) {
      (this.ref(name) as HTMLInputElement).disabled = busy;
    }
    (this.ref("outDir") as HTMLInputElement).checked = this.settings.outdir !== null;
    (this.ref("outNext") as HTMLInputElement).checked = this.settings.outdir === null;
    this.ref("outdir").textContent = this.settings.outdir ?? "";

    const applied = this.ref("applied");
    applied.textContent = this.planError ?? this.applied;
    applied.classList.toggle("bad", !!this.planError);

    this.rowElements.clear();
    this.ref("list").replaceChildren(...this.rows.map((row) => this.renderRow(row)));
    this.ref("empty").hidden = this.rows.length > 0;
    (this.ref("convert") as HTMLButtonElement).disabled =
      busy ||
      this.plannedSeq !== this.planSeq ||
      !!this.planError ||
      !this.rows.some((row) => this.convertible(row));
    (this.ref("cancel") as HTMLButtonElement).hidden = !busy;
  }
}
