// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import type { Defaults, PresetInfo, Settings, Writes } from "./backend";
import { ICONS, h, parts, svg } from "./dom";

type Keys = Record<string, string>;
type Side = "v" | "a";
type Options = [string, string][];

export type Choice = Pick<Settings, "preset" | "video" | "audio">;

const SIZE = ["width", "height", "long", "short"];
// Mirrors preset::EXCLUSIVE in crates/core.
const EXCLUSIVE = [
  [["width", "height"], ["long", "short"]],
  [["fps"], ["maxfps"]],
  [["crf", "qscale", "vbr"], ["bitrate"]],
];
const COMMON = ["width", "height", "long", "short", "scale", "fps", "maxfps"];
const ADVANCED = [
  "minquality", "buffer", "maxframe", "huffman", "dedup", "rotate", "rotatewhen", "rotatemeta",
  "keyint", "preset", "bframes", "gop", "hq",
];
const PRESET_ORDER = ["default", "quality", "small", "tiny"];
const VIDEO_CODECS: Options = [["mjpeg", "MJPEG"], ["mpeg2", "MPEG-2"], ["h264", "H.264"]];
const AUDIO_CODECS: Options = [["aac", "AAC"], ["mp3", "MP3"], ["none", "None"]];
const VIDEO_NAME: Keys = Object.fromEntries(VIDEO_CODECS);
const AUDIO_NAME: Keys = { aac: "AAC", mp3: "MP3", none: "no audio" };
const SIZES: Options = [["1280x720", "1280×720"], ["960x540", "960×540"], ["640x360", "640×360"], ["custom", "Custom"]];
const SCALE_HINT: Keys = {
  contain: "Fit inside the box and never upscale.",
  fit: "Fit inside the box, upscaling smaller videos.",
  cover: "Fill the box and crop what overflows.",
  stretch: "Fill the box and ignore the aspect ratio.",
};
const X264_PRESETS = ["ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"];

function parseSpec(text: string): { codec: string; keys: Keys } {
  const [codec, ...rest] = text.split(",");
  const keys = rest.map((kv) => {
    const i = kv.indexOf("=");
    return [kv.slice(0, i).trim(), kv.slice(i + 1).trim()];
  });
  return { codec: codec.trim(), keys: Object.fromEntries(keys) };
}

function omit(keys: Keys, name: string): Keys {
  const { [name]: _, ...rest } = keys;
  return rest;
}

function layer(base: Keys, over: Keys): Keys {
  const out = { ...base };
  for (const [key, value] of Object.entries(over)) {
    for (const choices of EXCLUSIVE) {
      const mine = choices.find((c) => c.includes(key));
      if (!mine) continue;
      for (const other of choices) if (other !== mine) for (const k of other) delete out[k];
    }
    out[key] = value;
  }
  return out;
}

function effective(defaults: Keys, preset: Keys, over: Keys): Keys {
  const base = { ...defaults };
  if (SIZE.some((k) => k in preset || k in over)) for (const k of SIZE) delete base[k];
  return layer(layer(base, preset), over);
}

function title(name: string): string {
  return name.charAt(0).toUpperCase() + name.slice(1);
}

function videoText(codec: string, e: Keys): string {
  const size = e.width || e.height ? `${e.width ?? "auto"}×${e.height ?? "auto"}` : `${e.long ?? "auto"}×${e.short ?? "auto"}`;
  const rate =
    codec === "mjpeg" ? `q${e.quality}` : e.bitrate ? e.bitrate : codec === "h264" ? `crf ${e.crf}` : `q${e.qscale}`;
  return `${size} ${VIDEO_NAME[codec]} ${rate}`;
}

function audioText(codec: string, a: Keys): string {
  if (codec === "none") return AUDIO_NAME.none;
  return a.vbr ? `${AUDIO_NAME[codec]} VBR ${a.vbr}` : `${AUDIO_NAME[codec]} ${a.bitrate}`;
}

function datalist(id: string, values: string[]): HTMLDataListElement {
  return h("datalist", { id }, ...values.map((value) => h("option", { value })));
}

export class SettingsForm {
  readonly element: HTMLElement;
  private preset = "default";
  private vo: Keys = {};
  private ao: Keys = {};
  private custom = false;
  private axes = false;
  private adv = false;
  private open = false;
  private emitted = "";
  private readonly updaters: (() => void)[] = [];
  private readonly fieldsets: HTMLFieldSetElement[] = [];
  private readonly presets: Map<string, PresetInfo>;

  constructor(
    presets: PresetInfo[],
    private readonly defaults: Defaults,
    private readonly writes: Writes,
    private readonly onChange: (choice: Choice) => void,
  ) {
    const order = (name: string) => {
      const i = PRESET_ORDER.indexOf(name);
      return i < 0 ? PRESET_ORDER.length : i;
    };
    this.presets = new Map([...presets].sort((a, b) => order(a.name) - order(b.name)).map((p) => [p.name, p]));
    if (!this.presets.has(this.preset)) this.preset = presets[0]?.name ?? "";
    this.element = h(
      "aside",
      { class: "split-side", "aria-label": "Encoding settings" },
      this.fieldset(this.presetCard()),
      h("div", { class: "card side-box" }, h("div", { class: "side-scroll" }, this.fieldset(this.videoSection(), this.audioSection()))),
    );
    document.addEventListener("click", (event) => {
      const inside = event.composedPath().some((node) => node instanceof Element && node.classList.contains("preset-pick"));
      if (this.open && !inside) this.setOpen(false);
    });
    document.addEventListener("keydown", (event) => {
      if (this.open && event.key === "Escape") this.setOpen(false);
    });
    this.render();
  }

  choice(): Choice {
    return { preset: this.preset, video: this.spec("v"), audio: this.spec("a") };
  }

  setDisabled(disabled: boolean): void {
    for (const fieldset of this.fieldsets) fieldset.disabled = disabled;
    if (disabled) this.setOpen(false);
  }

  private fieldset(...children: Node[]): HTMLFieldSetElement {
    const node = h("fieldset", {}, ...children);
    this.fieldsets.push(node);
    return node;
  }

  private sides(name: string) {
    const preset = this.presets.get(name);
    return { v: parseSpec(preset?.video ?? "mjpeg"), a: parseSpec(preset?.audio ?? "aac") };
  }

  private ctx() {
    const p = this.sides(this.preset);
    const vc = this.vo.codec ?? p.v.codec;
    const ac = this.ao.codec ?? p.a.codec;
    const vpk = vc === p.v.codec ? p.v.keys : {};
    const apk = ac === p.a.codec ? p.a.keys : {};
    const vov = omit(this.vo, "codec");
    const aov = omit(this.ao, "codec");
    const vdef = this.defaults.video[vc] ?? {};
    const adef = this.defaults.audio[ac] ?? {};
    return {
      p, vc, ac, vpk, apk, vov, aov,
      vbase: effective(vdef, vpk, {}),
      abase: effective(adef, apk, {}),
      v: effective(vdef, vpk, vov),
      a: effective(adef, apk, aov),
    };
  }

  private spec(side: Side): string {
    const o = side === "v" ? this.vo : this.ao;
    const keys = Object.entries(omit(o, "codec"));
    if (!o.codec && keys.length === 0) return "";
    const p = this.sides(this.preset);
    const codec = o.codec ?? (side === "v" ? p.v.codec : p.a.codec);
    return [codec, ...keys.map(([k, v]) => `${k}=${v}`)].join(",");
  }

  private changed(): void {
    this.render();
    const choice = this.choice();
    const key = JSON.stringify(choice);
    if (key === this.emitted) return;
    this.emitted = key;
    this.onChange(choice);
  }

  private render(): void {
    for (const update of this.updaters) update();
  }

  private apply(side: Side, values: Record<string, string | undefined>): void {
    const c = this.ctx();
    const base = side === "v" ? c.vbase : c.abase;
    const o = { ...(side === "v" ? this.vo : this.ao) };
    for (const [key, value] of Object.entries(values)) {
      if (value === undefined || value.trim() === "" || base[key] === value.trim()) delete o[key];
      else o[key] = value.trim();
    }
    if (side === "v") this.vo = o;
    else this.ao = o;
    this.changed();
  }

  private setSize(long: string | undefined, short: string | undefined): void {
    const c = this.ctx();
    const vo = Object.fromEntries(Object.entries(this.vo).filter(([k]) => !SIZE.includes(k)));
    const b = c.vbase;
    const same = !b.width && !b.height && (long || undefined) === b.long && (short || undefined) === b.short;
    if (!same) {
      if (long) vo.long = long;
      if (short) vo.short = short;
    }
    this.vo = vo;
    this.changed();
  }

  private setAxes(width: string | undefined, height: string | undefined): void {
    const vo = Object.fromEntries(Object.entries(this.vo).filter(([k]) => !SIZE.includes(k)));
    if (width) vo.width = width;
    if (height) vo.height = height;
    this.vo = vo;
    this.changed();
  }

  private tag(side: Side, keys: string[]): { changed: boolean; text: string } {
    const c = this.ctx();
    const over = side === "v" ? c.vov : c.aov;
    const preset = side === "v" ? c.vpk : c.apk;
    const changed = keys.some((k) => k in over);
    return { changed, text: changed ? "Changed" : keys.some((k) => k in preset) ? "Preset" : "Default" };
  }

  private value(side: Side, key: string): string {
    const c = this.ctx();
    return (side === "v" ? c.v : c.a)[key] ?? "";
  }

  private show<T extends HTMLElement>(node: T, visible: () => boolean): T {
    this.updaters.push(() => (node.hidden = !visible()));
    return node;
  }

  private text<T extends HTMLElement>(node: T, text: () => string): T {
    this.updaters.push(() => (node.textContent = text()));
    return node;
  }

  private label(text: string, side: Side, keys: string[], reset = false, control?: string): HTMLElement {
    const name = control ? h("label", { class: "t", for: control }, text) : h("span", { class: "t" }, text);
    const tag = h("span", { class: "tag" });
    const button = h("button", { type: "button", class: "reset", "aria-label": `Reset ${text.toLowerCase()}` }, "Reset");
    button.addEventListener("click", () => this.apply(side, Object.fromEntries(keys.map((k) => [k, undefined]))));
    this.updaters.push(() => {
      const t = this.tag(side, keys);
      tag.textContent = t.text;
      tag.classList.toggle("chg", t.changed);
      button.hidden = !(reset && t.changed);
    });
    return h("div", { class: "lbl" }, name, tag, button);
  }

  private seg(
    label: string,
    options: Options,
    current: () => string,
    pick: (value: string) => void,
    disabled?: (value: string) => boolean,
    big = false,
  ): HTMLElement {
    const box = h("div", { class: big ? "seg big" : "seg", role: "radiogroup", "aria-label": label });
    const buttons = options.map(([value, text]) => {
      const button = h("button", { type: "button", role: "radio" }, text);
      button.addEventListener("click", () => pick(value));
      box.append(button);
      return [value, button] as const;
    });
    this.updaters.push(() => {
      const now = current();
      for (const [value, button] of buttons) {
        button.classList.toggle("on", value === now);
        button.setAttribute("aria-checked", String(value === now));
        button.disabled = disabled?.(value) ?? false;
      }
    });
    return box;
  }

  private keySeg(label: string, side: Side, key: string, options: Options, disabled?: (value: string) => boolean): HTMLElement {
    return this.seg(label, options, () => this.value(side, key), (v) => this.apply(side, { [key]: v }), disabled);
  }

  private input(side: Side, key: string, attrs: Record<string, string>, value = () => this.value(side, key)): HTMLInputElement {
    const node = h("input", { class: "inp", spellcheck: "false", autocomplete: "off", ...attrs });
    node.addEventListener("input", () => this.apply(side, { [key]: node.value }));
    node.addEventListener("blur", () => this.render());
    this.updaters.push(() => {
      if (document.activeElement !== node) node.value = value();
    });
    return node;
  }

  private slider(side: Side, key: string, id: string, label: string, min: () => string, max: () => string): HTMLElement {
    const range = h("input", { type: "range", "aria-label": `${label} slider` });
    range.addEventListener("input", () => this.apply(side, { [key]: range.value }));
    this.updaters.push(() => {
      range.min = min();
      range.max = max();
      range.value = this.value(side, key);
    });
    return h("div", { class: "slider" }, range, this.input(side, key, { id, inputmode: "numeric" }));
  }

  private select(side: Side, key: string, id: string, options: Options): HTMLSelectElement {
    const node = h("select", { id, class: "inp" }, ...options.map(([value, text]) => h("option", { value }, text)));
    node.addEventListener("change", () => this.apply(side, { [key]: node.value }));
    this.updaters.push(() => (node.value = this.value(side, key)));
    return node;
  }

  private toggle(side: Side, key: string, id: string, text: string, hint: Node | string, on = "yes", off = "no"): HTMLElement {
    const button = h("button", { type: "button", role: "switch", class: "sw", "aria-labelledby": id }, h("span", { class: "knob" }));
    button.addEventListener("click", () => this.apply(side, { [key]: this.value(side, key) === on ? off : on }));
    this.updaters.push(() => {
      const checked = this.value(side, key) === on;
      button.classList.toggle("on", checked);
      button.setAttribute("aria-checked", String(checked));
    });
    return h(
      "div",
      { class: "toggle-row" },
      h("div", { class: "txt" }, h("span", { id }, text), h("span", { class: "hint" }, hint)),
      button,
    );
  }

  private setOpen(open: boolean): void {
    this.open = open;
    this.render();
  }

  private presetParts(name: string): string[] {
    const p = this.sides(name);
    const v = effective(this.defaults.video[p.v.codec] ?? {}, p.v.keys, {});
    const a = effective(this.defaults.audio[p.a.codec] ?? {}, p.a.keys, {});
    return [videoText(p.v.codec, v), v.fps ? `${v.fps} fps` : `≤${v.maxfps} fps`, audioText(p.a.codec, a)];
  }

  private presetCard(): HTMLElement {
    const count = h("span", { class: "tag chg" });
    const resetAll = h("button", { type: "button", class: "reset" }, "Reset all");
    resetAll.addEventListener("click", () => {
      Object.assign(this, { vo: {}, ao: {}, custom: false, axes: false });
      this.changed();
    });
    const dirty = this.show(h("span", { class: "dirty" }, count, resetAll), () => this.dirtyCount() > 0);
    this.text(count, () => `${this.dirtyCount()} changed`);

    const current = h("span", { class: "preset-text" });
    const button = h(
      "button",
      { type: "button", class: "preset-btn", "aria-haspopup": "listbox", "data-ref": "preset" },
      current,
      svg(ICONS.chevronDownLarge),
    );
    button.addEventListener("click", () => this.setOpen(!this.open));
    this.updaters.push(() => {
      current.replaceChildren(h("span", { class: "title" }, title(this.preset)), parts(this.presetParts(this.preset)));
      button.setAttribute("aria-expanded", String(this.open));
      button.setAttribute("aria-label", `Preset: ${title(this.preset)}`);
    });

    const list = this.show(h("ul", { class: "preset-list", role: "listbox", "aria-label": "Preset" }), () => this.open);
    for (const name of this.presets.keys()) {
      const option = h(
        "button",
        { type: "button", "data-preset": name },
        h("span", { class: "preset-text" }, h("span", { class: "title" }, title(name)), parts(this.presetParts(name))),
        svg(ICONS.check),
      );
      option.addEventListener("click", () => {
        Object.assign(this, { preset: name, vo: {}, ao: {}, custom: false, axes: false, open: false });
        this.changed();
      });
      const item = h("li", { role: "option" }, option);
      this.updaters.push(() => {
        option.classList.toggle("on", name === this.preset);
        item.setAttribute("aria-selected", String(name === this.preset));
      });
      list.append(item);
    }

    return h(
      "section",
      { class: "card" },
      h("div", { class: "sect-h" }, h("h3", {}, "Preset"), dirty),
      h("div", { class: "sect-b" }, h("div", { class: "preset-pick" }, button, list)),
    );
  }

  private dirtyCount(): number {
    return Object.keys(this.vo).length + Object.keys(this.ao).length;
  }

  private sizeMode(): { size: string; axes: boolean } {
    const v = this.ctx().v;
    const axes = !!(v.width || v.height) || (this.custom && this.axes);
    const named = !this.custom && !axes && v.long && v.short ? `${v.long}x${v.short}` : "custom";
    return { size: SIZES.some(([s]) => s === named) ? named : "custom", axes };
  }

  private videoSection(): HTMLElement {
    const v = () => this.ctx().v;
    const vc = () => this.ctx().vc;
    const is = (...codecs: string[]) => () => codecs.includes(vc());
    const codecs = VIDEO_CODECS.filter(([c]) => c in this.defaults.video);
    const codecSeg = this.seg("Video codec", codecs, vc, (codec) => {
      const vo: Keys = Object.fromEntries(Object.entries(this.vo).filter(([k]) => COMMON.includes(k)));
      if (codec !== this.sides(this.preset).v.codec) vo.codec = codec;
      this.vo = vo;
      this.changed();
    }, undefined, true);

    const sizeSeg = this.seg("Resolution", SIZES, () => this.sizeMode().size, (size) => {
      if (size === "custom") {
        this.custom = true;
        this.render();
        return;
      }
      const [long, short] = size.split("x");
      Object.assign(this, { custom: false, axes: false });
      this.setSize(long, short);
    });
    const axisSeg = this.seg(
      "Size by",
      [["sides", "Long × Short"], ["axes", "Width × Height"]],
      () => (this.sizeMode().axes ? "axes" : "sides"),
      (mode) => {
        const e = v();
        Object.assign(this, { custom: true, axes: mode === "axes" });
        if (mode === "axes") this.setAxes(e.long || "1280", e.short || "720");
        else this.setSize(e.width || "1280", e.height || "");
      },
    );
    const sizeInput = (id: string, label: string, placeholder: string, value: () => string, set: (value: string) => void) => {
      const node = h("input", { id, class: "inp", inputmode: "numeric", placeholder, autocomplete: "off" });
      node.addEventListener("input", () => set(node.value));
      node.addEventListener("blur", () => this.render());
      this.updaters.push(() => {
        if (document.activeElement !== node) node.value = value();
      });
      return h("div", { class: "fld" }, h("label", { for: id, class: "sublbl" }, `${label} (px)`), node);
    };
    const custom = this.show(
      h(
        "div",
        { class: "sub tight" },
        axisSeg,
        this.show(
          h(
            "div",
            { class: "row2" },
            sizeInput("slong", "Long Side", "auto", () => v().long ?? "", (x) => this.setSize(x, v().short)),
            sizeInput("sshort", "Short Side", "keep aspect", () => v().short ?? "", (x) => this.setSize(v().long, x)),
          ),
          () => !this.sizeMode().axes,
        ),
        this.show(
          h(
            "div",
            { class: "row2" },
            sizeInput("swid", "Width", "keep aspect", () => v().width ?? "", (x) => this.setAxes(x, v().height)),
            sizeInput("shei", "Height", "keep aspect", () => v().height ?? "", (x) => this.setAxes(v().width, x)),
          ),
          () => this.sizeMode().axes,
        ),
        this.text(h("p", { class: "hint" }), () =>
          this.sizeMode().axes
            ? "Fixed width and height for every video, whatever its orientation. A portrait clip is fitted into this landscape box."
            : "Sides follow each video's orientation, so a portrait clip comes out short × long.",
        ),
      ),
      () => this.sizeMode().size === "custom",
    );
    const sizeHint = this.show(
      this.text(h("p", { class: "hint" }), () => `Long and short follow each video's orientation, so a portrait clip comes out ${v().short}×${v().long}.`),
      () => this.sizeMode().size !== "custom",
    );

    const bothSides = () => {
      const e = v();
      return !!((e.long && e.short) || (e.width && e.height));
    };
    const scaleSeg = this.keySeg(
      "Fit",
      "v",
      "scale",
      [["contain", "Contain"], ["fit", "Fit"], ["cover", "Cover"], ["stretch", "Stretch"]],
      (mode) => (mode === "cover" || mode === "stretch") && !bothSides(),
    );

    const exact = () => !!v().fps;
    const rateSeg = this.seg("Frame rate mode", [["cap", "Up to"], ["exact", "Exactly"]], () => (exact() ? "exact" : "cap"), (mode) => {
      if (mode === "exact") this.apply("v", { fps: v().maxfps || "30", maxfps: undefined });
      else this.apply("v", { fps: undefined });
    });
    const rate = h("input", { id: "fpsv", class: "inp", list: "fpslist", autocomplete: "off" });
    rate.addEventListener("input", () => this.apply("v", { [exact() ? "fps" : "maxfps"]: rate.value }));
    rate.addEventListener("blur", () => this.render());
    this.updaters.push(() => {
      if (document.activeElement !== rate) rate.value = (exact() ? v().fps : v().maxfps) ?? "";
    });

    const rcMode = () => (v().bitrate ? "bitrate" : "quality");
    const rqKey = () => (vc() === "h264" ? "crf" : "qscale");
    const rcSeg = this.seg(
      "Rate control",
      [["quality", "Constant quality"], ["bitrate", "Target bitrate"]],
      rcMode,
      (mode) => {
        if (mode === "bitrate") this.apply("v", { bitrate: vc() === "h264" ? "1.5M" : "4M", [rqKey()]: undefined });
        else this.apply("v", { bitrate: undefined });
      },
      (mode) => mode === "bitrate" && vc() === "mpeg2" && !this.writes.mpeg2Bitrate,
    );
    const quantiser = (key: string, name: string, min: string, max: string) =>
      this.show(
        h(
          "div",
          { class: "fld" },
          this.label(name, "v", [key], true, `rq-${key}`),
          this.slider("v", key, `rq-${key}`, name, () => min, () => max),
          h("div", { class: "scale" }, h("span", {}, "Sharper"), h("span", {}, "Smaller files")),
        ),
        () => rcMode() === "quality" && rqKey() === key && is("h264", "mpeg2")(),
      );

    const advCount = () => ADVANCED.filter((k) => k in this.ctx().vov).length;
    const advButton = h("button", { type: "button", class: "adv" }, svg(ICONS.chevronRight), h("span", {}));
    advButton.addEventListener("click", () => {
      this.adv = !this.adv;
      this.render();
    });
    this.updaters.push(() => {
      advButton.classList.toggle("open", this.adv);
      advButton.setAttribute("aria-expanded", String(this.adv));
      const n = advCount();
      advButton.lastElementChild!.textContent = `Advanced ${VIDEO_NAME[vc()]} options${n ? ` · ${n} changed` : ""}`;
    });

    const mjpegAdvanced = this.show(
      h(
        "div",
        { class: "stack" },
        h(
          "div",
          { class: "fld" },
          this.label("Min quality", "v", ["minquality"], false, "amq"),
          this.input("v", "minquality", { id: "amq", inputmode: "numeric" }),
        ),
        h(
          "div",
          { class: "fld" },
          this.label("Max frame size", "v", ["maxframe"], false, "amf"),
          this.input("v", "maxframe", { id: "amf" }),
        ),
        h(
          "p",
          { class: "hint" },
          "Rate control never goes below Min quality. Frames over Max frame size (up to 1 MB, the player's limit) are re-encoded lower.",
        ),
        h(
          "div",
          { class: "fld" },
          this.label("Burst buffer (bytes)", "v", ["buffer"], false, "abuf"),
          this.input("v", "buffer", { id: "abuf" }),
          h("p", { class: "hint" }, "How far a burst may run ahead of the bitrate ceiling. Default is the player's 1 MB read-ahead."),
        ),
        h(
          "div",
          { class: "fld" },
          this.label("Huffman tables", "v", ["huffman"]),
          this.keySeg("Huffman tables", "v", "huffman", [["optimal", "Optimal per frame"], ["standard", "Standard"]]),
        ),
        this.toggle(
          "v",
          "dedup",
          "ldedup",
          "Skip near-duplicate frames",
          "A frame that barely differs from the last one isn't stored; the previous frame stays on screen longer (at least one frame every 2 s).",
        ),
        h(
          "div",
          { class: "group" },
          h("strong", {}, "Panel rotation"),
          h(
            "div",
            { class: "fld" },
            this.label("Turn counter-clockwise", "v", ["rotate"]),
            this.keySeg("Rotation", "v", "rotate", [["0", "0°"], ["90", "90°"], ["180", "180°"], ["270", "270°"]]),
          ),
          h(
            "div",
            { class: "fld" },
            this.label("Apply to outputs that are", "v", ["rotatewhen"]),
            this.keySeg("Apply rotation to", "v", "rotatewhen", [["landscape", "Landscape"], ["portrait", "Portrait"], ["always", "Always"]]),
          ),
          this.toggle(
            "v",
            "rotatemeta",
            "lrm",
            "Store display rotation",
            "Other players show the original orientation, while Tab5 decodes the stored frames straight into its portrait panel.",
          ),
        ),
      ),
      is("mjpeg"),
    );
    const h264Advanced = this.show(
      h(
        "div",
        { class: "stack" },
        h(
          "div",
          { class: "fld" },
          this.label("Keyframe interval (seconds)", "v", ["keyint"], false, "hki"),
          this.input("v", "keyint", { id: "hki", inputmode: "decimal" }),
        ),
        h(
          "div",
          { class: "fld" },
          this.label("Encoder speed", "v", ["preset"], false, "hps"),
          this.select("v", "preset", "hps", X264_PRESETS.map((p) => [p, p])),
        ),
        h("p", { class: "hint" }, "Slower x264 presets make smaller files at the same quality."),
      ),
      is("h264"),
    );
    const mpeg2Advanced = this.show(
      h(
        "div",
        { class: "stack" },
        h(
          "div",
          { class: "fld" },
          this.label("B-frames in a row", "v", ["bframes"]),
          this.keySeg("B-frames", "v", "bframes", [["0", "0"], ["1", "1"], ["2", "2"], ["3", "3"]]),
        ),
        h(
          "div",
          { class: "fld" },
          this.label("I-frame interval (seconds)", "v", ["keyint"], false, "mki"),
          this.input("v", "keyint", { id: "mki", inputmode: "decimal" }),
        ),
        h(
          "div",
          { class: "fld" },
          this.label("GOP", "v", ["gop"]),
          this.keySeg("GOP", "v", "gop", [["closed", "Closed"], ["open", "Open"]], (g) => g === "open" && !this.writes.openGop),
        ),
        h(
          "p",
          { class: "hint" },
          this.writes.openGop ? "Closed turns off extra I-frames at scene changes." : "The browser encoder writes closed GOPs only.",
        ),
        this.toggle("v", "hq", "lhq", "High-quality decisions", "Rate-distortion decisions and trellis quantisation. Slower to encode."),
      ),
      is("mpeg2"),
    );

    return h(
      "section",
      {},
      h("div", { class: "sect-h" }, h("h3", {}, "Video")),
      h(
        "div",
        { class: "sect-b" },
        h("div", { class: "fld" }, codecSeg),
        h("div", { class: "fld" }, this.label("Resolution", "v", SIZE, true), sizeSeg, custom, sizeHint),
        h(
          "div",
          { class: "fld" },
          this.label("Fit", "v", ["scale"], true),
          scaleSeg,
          this.text(h("p", { class: "hint" }), () => SCALE_HINT[v().scale] + (bothSides() ? "" : " Cover and Stretch need both sides.")),
        ),
        h(
          "div",
          { class: "fld" },
          this.label("Frame rate", "v", ["fps", "maxfps"], true, "fpsv"),
          h("div", { class: "rate" }, rateSeg, rate, h("span", { class: "unit" }, "fps"), datalist("fpslist", ["24", "25", "29.97", "30", "50", "60"])),
          this.text(h("p", { class: "hint" }), () =>
            exact() ? "Frames are dropped or repeated to match." : "Faster videos are slowed to this rate; slower ones stay as they are.",
          ),
        ),
        this.show(
          h(
            "div",
            { class: "fld" },
            this.label("Quality", "v", ["quality"], true, "mq"),
            this.slider("v", "quality", "mq", "Quality", () => "1", () => "100"),
            h("div", { class: "scale" }, h("span", {}, "Smaller files"), h("span", {}, "Sharper")),
          ),
          is("mjpeg"),
        ),
        this.show(
          h(
            "div",
            { class: "fld" },
            this.label("Bitrate ceiling", "v", ["bitrate"], true, "mbr"),
            this.input("v", "bitrate", { id: "mbr", list: "vbrlist" }),
            this.text(h("p", { class: "hint" }), () => `Quality drops only in scenes that would go over this. Default ${this.defaults.video.mjpeg?.bitrate}.`),
          ),
          is("mjpeg"),
        ),
        this.show(
          h(
            "div",
            { class: "fld" },
            this.label("Profile", "v", ["profile"], true),
            this.keySeg("Profile", "v", "profile", [["baseline", "Baseline"], ["main", "Main"], ["high", "High"]]),
          ),
          is("h264"),
        ),
        this.show(h("div", { class: "fld" }, h("div", { class: "lbl" }, h("span", { class: "t" }, "Rate control")), rcSeg), is("h264", "mpeg2")),
        quantiser("crf", "CRF", "0", "51"),
        quantiser("qscale", "Quantiser", "1", "31"),
        this.show(
          h(
            "div",
            { class: "fld" },
            this.label("Bitrate", "v", ["bitrate"], true, "rbr"),
            this.input("v", "bitrate", { id: "rbr", list: "vbrlist" }),
            h("p", { class: "hint" }, "Average video bitrate, e.g. 800k or 1.5M."),
          ),
          () => is("h264", "mpeg2")() && rcMode() === "bitrate",
        ),
        datalist("vbrlist", ["800k", "1.5M", "4M", "12M", "24M", "40M"]),
        advButton,
        this.show(h("div", { class: "sub" }, mjpegAdvanced, h264Advanced, mpeg2Advanced), () => this.adv),
      ),
    );
  }

  private audioSection(): HTMLElement {
    const a = () => this.ctx().a;
    const ac = () => this.ctx().ac;
    const codecSeg = this.seg("Audio codec", AUDIO_CODECS, ac, (codec) => {
      const ao: Keys = {};
      if (codec !== "none") for (const k of ["channels", "samplerate"]) if (k in this.ao) ao[k] = this.ao[k];
      if (codec !== this.sides(this.preset).a.codec) ao.codec = codec;
      this.ao = ao;
      this.changed();
    }, undefined, true);
    const vbr = () => ac() === "mp3" && !!a().vbr;
    const keepHint = h("span", {});
    this.text(keepHint, () => {
      const limit = vbr() ? "" : ` and no more than ${a().bitrate}`;
      return `${AUDIO_NAME[ac()]}${ac() === "aac" ? "-LC" : ""} input with up to 2 channels at 16–48 kHz${limit} is copied without re-encoding.`;
    });
    const mp3Seg = this.seg("MP3 mode", [["cbr", "Constant bitrate"], ["vbr", "Variable (VBR)"]], () => (vbr() ? "vbr" : "cbr"), (mode) => {
      if (mode === "vbr") this.apply("a", { vbr: "2", bitrate: undefined });
      else this.apply("a", { vbr: undefined });
    });

    return h(
      "section",
      {},
      h("div", { class: "sect-h" }, h("h3", {}, "Audio")),
      h(
        "div",
        { class: "sect-b" },
        codecSeg,
        this.show(
          h("p", { class: "hint" }, "The browser can't encode MP3: MP3 inputs are kept as they are, anything else fails. Choose AAC to convert every file."),
          () => ac() === "mp3" && !this.writes.mp3,
        ),
        this.show(
          h(
            "div",
            { class: "stack" },
            this.toggle("a", "keep", "lkeep", "Keep compatible audio as is", keepHint, "auto", "none"),
            this.show(mp3Seg, () => ac() === "mp3"),
            this.show(
              h(
                "div",
                { class: "fld" },
                this.label("VBR quality", "a", ["vbr"], false, "avbr"),
                this.slider("a", "vbr", "avbr", "VBR quality", () => "0", () => "9"),
                h("div", { class: "scale" }, h("span", {}, "Better"), h("span", {}, "Smaller files")),
              ),
              vbr,
            ),
            this.show(
              h(
                "div",
                { class: "fld" },
                this.label("Bitrate", "a", ["bitrate"], true, "abr"),
                this.input("a", "bitrate", { id: "abr", list: "abrlist" }),
                datalist("abrlist", ["64k", "96k", "128k", "160k", "192k", "256k", "320k"]),
              ),
              () => !vbr(),
            ),
            h(
              "div",
              { class: "row2" },
              h(
                "div",
                { class: "fld" },
                this.label("Channels", "a", ["channels"], false, "ach"),
                this.select("a", "channels", "ach", [["", "Same as source"], ["1", "Mono"], ["2", "Stereo"]]),
              ),
              h(
                "div",
                { class: "fld" },
                this.label("Sample rate", "a", ["samplerate"], false, "asr"),
                this.select("a", "samplerate", "asr", [
                  ["", "Same as source"],
                  ["16k", "16 kHz"],
                  ["22.05k", "22.05 kHz"],
                  ["24k", "24 kHz"],
                  ["32k", "32 kHz"],
                  ["44.1k", "44.1 kHz"],
                  ["48k", "48 kHz"],
                ]),
              ),
            ),
            h("p", { class: "hint tight" }, "Channels and sample rate apply only when audio is re-encoded."),
          ),
          () => ac() !== "none",
        ),
      ),
    );
  }
}
