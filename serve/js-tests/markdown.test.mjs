import { strictEqual } from "node:assert/strict";
import { test } from "node:test";
import * as mod from "../web/modules/markdown.js";

// --- inline: HTML escaping ---
test("inline escapes <b> to &lt;b&gt;", () => {
  strictEqual(mod.inline("<b>"), "&lt;b&gt;");
});

// --- inline: bold ---
test("inline **bold** becomes <strong>", () => {
  strictEqual(mod.inline("**bold**"), "<strong>bold</strong>");
});

// --- inline: backtick code with angle brackets inside ---
test("inline backtick with <b> produces escaped <code class=\"inline\">", () => {
  const result = mod.inline("`a<b`");
  strictEqual(result.includes("<code class=\"inline\">"), true);
  strictEqual(result.includes("&lt;b"), true);
  strictEqual(result.includes("<b>"), false);
});

// --- codeBlock: class and data-code-copy ---
test("codeBlock emits class=\"st-code\" and data-code-copy", () => {
  const html = mod.codeBlock("js", "console.log(1)");
  strictEqual(html.includes("class=\"st-code\""), true);
  strictEqual(html.includes("data-code-copy"), true);
});

// --- markdown: unclosed fence (streaming case) ---
test("markdown with unclosed fence still emits a code block", () => {
  const html = mod.markdown("```\nstill streaming");
  strictEqual(html.includes("class=\"st-code\""), true);
  strictEqual(html.includes("<code>"), true);
});

// --- blocks: simple list ---
test("blocks '- item' emits <ul><li>item</li></ul>", () => {
  const html = mod.blocks("- item");
  strictEqual(html, "<ul><li>item</li></ul>");
});

// --- markdown: two-column table ---
test("markdown table emits <table> with <th> and <td>", () => {
  const html = mod.markdown("| a | b |\n|---|---|\n| 1 | 2 |");
  strictEqual(html.includes("<table>"), true);
  strictEqual(html.includes("<th>a</th>"), true);
  strictEqual(html.includes("<td>1</td>"), true);
});
