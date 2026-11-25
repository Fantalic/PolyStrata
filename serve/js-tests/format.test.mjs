import { strictEqual, deepStrictEqual } from "node:assert/strict";
import { test } from "node:test";
import * as mod from "../web/modules/format.js";

// --- esc: all five HTML-special characters ---
test("esc escapes &, <, >, \", '", () => {
  strictEqual(mod.esc("a<b&>\"'"), "a&lt;b&amp;&gt;&quot;&#39;");
});

// --- icon: SVG href reference ---
test("icon builds correct SVG string", () => {
  const svg = mod.icon("copy");
  strictEqual(svg.includes("web/sprite.svg#i-copy"), true);
  strictEqual(svg.includes("aria-hidden"), true);
});

// --- fmt: null → "–", and locale-independent via Intl ---
test("fmt returns '–' for null / NaN", () => {
  strictEqual(mod.fmt(null), "–");
  strictEqual(mod.fmt(NaN), "–");
});

test("fmt matches Intl.NumberFormat (no decimals)", () => {
  const nf = (n) => new Intl.NumberFormat(undefined).format(n);
  strictEqual(mod.fmt(1000), nf(1000));
  strictEqual(mod.fmt(1234567), nf(1234567));
});

test("fmt matches Intl.NumberFormat (with decimals)", () => {
  const nf = (n, d) => new Intl.NumberFormat(undefined, { maximumFractionDigits: d, minimumFractionDigits: d }).format(n);
  strictEqual(mod.fmt(3.14159, 2), nf(3.14159, 2));
  strictEqual(mod.fmt(1000.1, 2), nf(1000.1, 2));
});

// --- kfmt: null → "–" ---
test("kfmt returns '–' for null", () => {
  strictEqual(mod.kfmt(null), "–");
});

test("kfmt formats thousands", () => {
  const nf = (n) => new Intl.NumberFormat(undefined).format(n);
  strictEqual(mod.kfmt(1500), `${nf(1.5)}k`);
  strictEqual(mod.kfmt(15000), `${nf(15)}k`);
});

// --- ctxfmt: power-of-two context sizes ---
test("ctxfmt maps 32768 to '32K'", () => {
  strictEqual(mod.ctxfmt(32768), "32K");
});

test("ctxfmt falls back to kfmt for non-powers-of-two", () => {
  strictEqual(mod.ctxfmt(1000), mod.kfmt(1000));
});

// --- gb: null → "–" ---
test("gb returns '–' for null", () => {
  strictEqual(mod.gb(null), "–");
});

test("gb converts bytes to GB with Intl", () => {
  const nf = (n, d) => new Intl.NumberFormat(undefined, { maximumFractionDigits: d, minimumFractionDigits: d }).format(n);
  const bytes = 1073741824; // 1 GiB
  strictEqual(mod.gb(bytes), nf(1, 1));
});
