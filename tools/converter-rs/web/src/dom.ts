// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

export function h<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  attrs: Record<string, string> = {},
  ...children: (Node | string)[]
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attrs)) node.setAttribute(name, value);
  node.append(...children);
  return node;
}

export function svg(markup: string): SVGElement {
  const template = document.createElement("template");
  template.innerHTML = markup.trim();
  return template.content.firstChild as SVGElement;
}

export const ICONS = {
  chevronDown: '<svg width="12" height="12" viewBox="0 0 12 12" fill="none" aria-hidden="true"><path d="M3 4.5l3 3 3-3" stroke="currentColor" stroke-width="1.5"/></svg>',
  chevronDownLarge: '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" aria-hidden="true"><path d="M4 6l4 4 4-4" stroke="currentColor" stroke-width="1.6"/></svg>',
  chevronRight: '<svg width="12" height="12" viewBox="0 0 12 12" fill="none" aria-hidden="true"><path d="M4.5 3l3 3-3 3" stroke="currentColor" stroke-width="1.6"/></svg>',
  check: '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" aria-hidden="true"><path d="M3 8.5l3.2 3L13 4.5" stroke="currentColor" stroke-width="1.8"/></svg>',
  plus: '<svg width="14" height="14" viewBox="0 0 14 14" fill="none" aria-hidden="true"><path d="M7 2v10M2 7h10" stroke="currentColor" stroke-width="1.6"/></svg>',
  close: '<svg width="14" height="14" viewBox="0 0 14 14" fill="none" aria-hidden="true"><path d="M3.5 3.5l7 7M10.5 3.5l-7 7" stroke="currentColor" stroke-width="1.5"/></svg>',
  video: '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" aria-hidden="true"><rect x="2" y="3" width="12" height="10" rx="1.5" stroke="currentColor" stroke-width="1.3"/><path d="M6.5 6v4l3.5-2z" fill="currentColor"/></svg>',
  videoLarge: '<svg width="40" height="40" viewBox="0 0 40 40" fill="none" aria-hidden="true"><rect x="6" y="9" width="28" height="22" rx="3" stroke="var(--text-4)" stroke-width="2"/><path d="M17 15.5v9l7.5-4.5z" fill="var(--text-4)"/></svg>',
  folder: '<svg width="14" height="14" viewBox="0 0 14 14" fill="none" aria-hidden="true"><path d="M1.5 3.5h4l1.2 1.5h5.8v6.5h-11z" stroke="currentColor" stroke-width="1.3"/></svg>',
};

export function parts(texts: string[]): HTMLElement {
  const box = h("span", { class: "parts" });
  texts.forEach((text, i) => {
    if (i > 0) box.append(h("span", { class: "sep", "aria-hidden": "true" }));
    box.append(h("span", {}, text));
  });
  return box;
}
