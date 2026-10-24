// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import type { Backend, Input, Outcome, PlanItem, Settings, Status } from "./backend";
import { ICONS, h, parts, svg } from "./dom";
import { SettingsForm } from "./settings";

type RowState = "idle" | "running" | "done" | "failed" | "cancelled";

interface Row {
  input: Input;
  plan?: PlanItem;
  state: RowState;
  fraction: number;
  message?: string;
  report?: string[];
  thumb?: string;
}

interface RowElements {
  badge: HTMLElement;
  bar: HTMLElement;
  thumb: HTMLElement;
}

const PLAN_DELAY_MS = 250;

const TEMPLATE = `
<header class="top">
  <h1>Tab5 Media Converter</h1>
  <div class="status-wrap" data-ref="statusWrap"></div>
</header>
<main class="app-main">
  <section class="card q-card" aria-labelledby="qh" data-ref="drop">
    <div class="q-head">
      <h2 id="qh">Videos <span class="mono count" data-ref="count"></span></h2>
      <button type="button" class="btn" data-ref="add">${ICONS.plus}Add videos…</button>
      <button type="button" class="btn ghost" data-ref="clear">Clear</button>
    </div>
    <div class="q-scroll">
      <ul class="q-list" data-ref="list"></ul>
      <div class="drop" data-ref="empty">
        ${ICONS.videoLarge}
        <div class="drop-text">
          <strong>Drop videos here</strong>
          <span>MP4, MOV, MKV, WebM — any size or orientation</span>
        </div>
        <button type="button" class="btn" data-ref="choose">Choose videos…</button>
      </div>
    </div>
    <div class="q-foot">
      <div class="save" data-ref="saveTo">
        <span class="label" id="savel">Save to</span>
        <div class="seg" role="radiogroup" aria-labelledby="savel">
          <button type="button" role="radio" data-ref="outNext">Next to each video</button>
          <button type="button" role="radio" data-ref="outDir">Folder</button>
        </div>
        <button type="button" class="btn folder" data-ref="chooseDir">${ICONS.folder}<span class="name" data-ref="outName"></span><span class="change">Change…</span></button>
        <span class="hint" data-ref="outHint">Saved as <span class="mono">name.tab5.mp4</span> beside each source</span>
      </div>
      <div class="action">
        <div class="sum" data-ref="idle">
          <span class="sum-main" data-ref="summary"></span>
          <span class="sum-sub" data-ref="summarySub">Downloads as name.tab5.mp4</span>
        </div>
        <button type="button" class="btn primary lg" data-ref="convert">Convert</button>
        <div class="run-info" data-ref="runInfo">
          <div class="run-line"><span><strong data-ref="runCount"></strong> · <span data-ref="runName"></span></span><span class="mono eta" data-ref="eta"></span></div>
          <div class="bar"><i data-ref="total"></i></div>
        </div>
        <button type="button" class="btn lg" data-ref="cancel">Cancel</button>
      </div>
    </div>
  </section>
</main>
`;

function message(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

function basename(path: string): string {
  return path.split(/[\\/]/).pop() ?? path;
}

function remaining(seconds: number): string {
  if (seconds < 60) return "less than a minute left";
  return `about ${Math.round(seconds / 60)} min left`;
}

export class App {
  private rows: Row[] = [];
  private settings: Settings = { preset: "default", video: "", audio: "", outdir: null };
  private folder: string | null = null;
  private planError: string | null = null;
  private planSeq = 0;
  private plannedSeq = 0;
  private planTimer: number | undefined;
  private running: Row | null = null;
  private queue: Row[] = [];
  private started = 0;
  private stopping = false;
  private form: SettingsForm | null = null;
  private closeStatus = () => {};
  private thumbnails = Promise.resolve();
  private rowElements = new Map<Row, RowElements>();
  private readonly refs: Record<string, HTMLElement>;

  constructor(
    private readonly backend: Backend,
    private readonly root: HTMLElement,
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
    const [presets, defaults, status] = await Promise.all([
      this.backend.presets(),
      this.backend.defaults(),
      this.backend.status(),
    ]);
    this.form = new SettingsForm(presets, defaults, this.backend.writes, (choice) => {
      Object.assign(this.settings, choice);
      this.settingsChanged();
    });
    Object.assign(this.settings, this.form.choice());
    this.root.querySelector(".app-main")!.append(this.form.element);
    this.ref("saveTo").hidden = this.backend.outputs !== "folder";
    this.ref("summarySub").hidden = this.backend.outputs !== "download";
    this.showStatus(status);
    this.render();
    this.schedulePlan();
  }

  private bind(): void {
    this.ref("outNext").addEventListener("click", () => {
      this.settings.outdir = null;
      this.settingsChanged();
    });
    this.ref("outDir").addEventListener("click", () => {
      if (this.folder) {
        this.settings.outdir = this.folder;
        this.settingsChanged();
      } else {
        void this.chooseOutdir();
      }
    });
    this.ref("chooseDir").addEventListener("click", () => void this.chooseOutdir());
    const add = async () => this.add(await this.backend.pickInputs());
    this.ref("add").addEventListener("click", add);
    this.ref("choose").addEventListener("click", add);
    this.ref("clear").addEventListener("click", () => {
      for (const row of this.rows) this.forget(row);
      this.rows = [];
      this.schedulePlan();
    });
    document.addEventListener("click", (event) => {
      if (!this.ref("statusWrap").contains(event.target as Node)) this.closeStatus();
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
    const wrap = this.ref("statusWrap");
    const text = h("span", {}, status.text);
    const dot = h("span", { class: "dot" });
    if (this.backend.actions.length === 0) {
      const chip = h("span", { class: `chip static${status.ok ? "" : " bad"}` }, dot, text);
      chip.title = [status.title, ...(status.lines ?? [])].filter(Boolean).join("\n");
      wrap.replaceChildren(chip);
      return;
    }
    const chip = h("button", { type: "button", class: `chip${status.ok ? "" : " bad"}`, "aria-expanded": "false" }, dot, text, svg(ICONS.chevronDown));
    const actions = this.backend.actions.map((action) => {
      const button = h("button", { type: "button", class: "btn" }, action.label);
      button.addEventListener("click", async () => {
        const next = await action.run().catch((err) => ({ ok: false, text: "ffmpeg not available", title: message(err) }));
        if (next) {
          this.showStatus(next);
          this.schedulePlan();
        }
      });
      return button;
    });
    const pop = h(
      "div",
      { class: "card pop", role: "dialog", "aria-label": "ffmpeg location" },
      h(
        "div",
        { class: "pop-head" },
        h("strong", {}, status.title ?? status.text),
        ...(status.lines ?? []).map((line) => h("span", { class: "mono" }, line)),
      ),
      h("div", { class: "pop-actions" }, ...actions),
    );
    pop.hidden = true;
    const setOpen = (open: boolean) => {
      pop.hidden = !open;
      chip.setAttribute("aria-expanded", String(open));
    };
    chip.addEventListener("click", () => setOpen(pop.hidden === true));
    this.closeStatus = () => setOpen(false);
    wrap.replaceChildren(chip, pop);
  }

  private async chooseOutdir(): Promise<void> {
    const dir = await this.backend.pickOutdir();
    if (dir) {
      this.folder = dir;
      this.settings.outdir = dir;
    }
    this.settingsChanged();
  }

  private add(inputs: Input[]): void {
    const known = new Set(this.rows.map((row) => row.input.key));
    for (const input of inputs) {
      if (known.has(input.key)) continue;
      known.add(input.key);
      const row: Row = { input, state: "idle", fraction: 0 };
      this.rows.push(row);
      this.thumbnails = this.thumbnails.then(() => this.loadThumbnail(row));
    }
    this.schedulePlan();
  }

  private async loadThumbnail(row: Row): Promise<void> {
    if (!this.rows.includes(row)) return;
    const url = await this.backend.thumbnail(row.input).catch((err) => {
      console.warn(`thumbnail of ${row.input.name}: ${message(err)}`);
      return null;
    });
    if (!url) return;
    row.thumb = url;
    if (!this.rows.includes(row)) this.forget(row);
    else this.rowElements.get(row)?.thumb.replaceChildren(h("img", { src: url, alt: "" }));
  }

  private forget(row: Row): void {
    if (row.thumb) URL.revokeObjectURL(row.thumb);
    row.thumb = undefined;
  }

  private remove(row: Row): void {
    this.forget(row);
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
    this.queue = queue;
    this.started = performance.now();
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
    this.queue = [];
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
      elements.bar.style.width = `${row.fraction * 100}%`;
      elements.badge.textContent = this.badge(row)[0];
    }
    this.renderProgress();
  }

  private badge(row: Row): [string, string] {
    const plan = row.plan;
    switch (row.state) {
      case "running":
        return [plan?.duration ? `Converting ${Math.floor(row.fraction * 100)}%` : "Converting…", "run"];
      case "done":
        return ["Done", "done"];
      case "failed":
        return ["Failed", "err"];
      case "cancelled":
        return ["Cancelled", "wait"];
      case "idle":
        if (this.running && this.queue.includes(row)) return ["Waiting", "wait"];
        if (this.planError) return ["—", "wait"];
        if (!plan) return ["Checking…", "wait"];
        if (plan.error) return ["Cannot convert", "err"];
        if (plan.skip) return ["Skipped", "wait"];
        return plan.exists ? ["Replaces a file", "warn"] : ["Ready", "ready"];
    }
  }

  private renderRow(row: Row): HTMLLIElement {
    const plan = row.plan;
    const [text, kind] = this.badge(row);
    const badge = h("span", { class: `st ${kind}` }, text);
    if (row.state === "done" && row.report?.length) {
      badge.classList.add("has-tip");
      badge.tabIndex = 0;
      badge.append(h("pre", { class: "tip", role: "tooltip" }, row.report.join("\n")));
    }
    const main = h("div", { class: "qmain" }, h("div", { class: "qname" }, h("span", { class: "n" }, row.input.name), badge));
    if (plan?.sourceParts && plan.outputParts) {
      const out = h("span", { class: "out" }, h("span", { class: "arrow", "aria-label": "becomes" }, "→"), parts(plan.outputParts));
      out.title = [plan.video, plan.audio].filter(Boolean).join("\n");
      main.append(h("div", { class: "qmeta" }, parts(plan.sourceParts), out));
    }
    if (plan?.output) {
      const path = this.backend.outputs === "folder" ? plan.output : `${basename(plan.output)} → Downloads`;
      main.append(h("div", { class: "qmeta" }, h("span", { class: "path", title: plan.output }, path)));
    }
    const problem = row.message ?? plan?.error ?? (plan?.skip ? `Skipped: ${plan.skip}` : undefined);
    if (problem) main.append(h("pre", { class: "qerr" }, problem));
    const fill = h("i", {});
    fill.style.width = `${row.fraction * 100}%`;
    const bar = h("div", { class: "bar", role: "progressbar", "aria-label": "Progress" }, fill);
    bar.hidden = row.state !== "running";
    main.append(bar);
    const thumb = h("div", { class: "thumb" }, row.thumb ? h("img", { src: row.thumb, alt: "" }) : svg(ICONS.video));
    const item = h("li", { class: "qrow" }, thumb, main);
    if (!this.running) {
      const remove = h("button", { type: "button", class: "iconbtn", "aria-label": `Remove ${row.input.name}` }, svg(ICONS.close));
      remove.addEventListener("click", () => this.remove(row));
      item.append(remove);
    }
    this.rowElements.set(row, { badge, bar: fill, thumb });
    return item;
  }

  private renderProgress(): void {
    const row = this.running;
    if (!row) return;
    const index = this.queue.indexOf(row);
    this.ref("runCount").textContent = `Converting ${index + 1} of ${this.queue.length}`;
    this.ref("runName").textContent = row.input.name;
    const weight = (r: Row) => r.plan?.duration ?? 1;
    const total = this.queue.reduce((sum, r) => sum + weight(r), 0);
    const done = this.queue.slice(0, index).reduce((sum, r) => sum + weight(r), 0) + weight(row) * row.fraction;
    const fraction = total > 0 ? done / total : 0;
    this.ref("total").style.width = `${fraction * 100}%`;
    const elapsed = (performance.now() - this.started) / 1000;
    this.ref("eta").textContent = fraction > 0.01 && elapsed > 2 ? remaining((elapsed * (1 - fraction)) / fraction) : "";
  }

  private render(): void {
    const busy = !!this.running;
    this.form?.setDisabled(busy);
    for (const name of ["add", "clear", "outNext", "outDir", "chooseDir", "choose"]) {
      (this.ref(name) as HTMLButtonElement).disabled = busy;
    }
    const folder = this.settings.outdir !== null;
    for (const [name, on] of [["outNext", !folder], ["outDir", folder]] as const) {
      this.ref(name).classList.toggle("on", on);
      this.ref(name).setAttribute("aria-checked", String(on));
    }
    this.ref("chooseDir").hidden = !folder;
    this.ref("chooseDir").title = this.settings.outdir ?? "";
    this.ref("outName").textContent = basename(this.settings.outdir ?? "");
    this.ref("outHint").hidden = folder;

    this.rowElements.clear();
    this.ref("list").replaceChildren(...this.rows.map((row) => this.renderRow(row)));
    this.ref("list").hidden = this.rows.length === 0;
    this.ref("empty").hidden = this.rows.length > 0;
    this.ref("count").textContent = String(this.rows.length);

    const ready = this.rows.filter((row) => this.convertible(row)).length;
    const failed = this.rows.filter((row) => row.plan?.error || row.state === "failed").length;
    const done = this.rows.filter((row) => row.state === "done").length;
    const summary = this.ref("summary");
    summary.classList.toggle("bad", !!this.planError);
    if (this.planError) summary.textContent = this.planError;
    else if (this.rows.length === 0) summary.textContent = "No videos yet";
    else
      summary.textContent = [`${ready} ready`, done ? `${done} done` : "", failed ? `${failed} can't convert` : ""]
        .filter(Boolean)
        .join(" · ");

    const convert = this.ref<HTMLButtonElement>("convert");
    convert.textContent = ready === 0 ? "Convert" : `Convert ${ready} ${ready === 1 ? "video" : "videos"}`;
    convert.disabled = busy || this.plannedSeq !== this.planSeq || !!this.planError || ready === 0;
    for (const name of ["idle", "convert"]) this.ref(name).hidden = busy;
    for (const name of ["runInfo", "cancel"]) this.ref(name).hidden = !busy;
    this.renderProgress();
  }
}
