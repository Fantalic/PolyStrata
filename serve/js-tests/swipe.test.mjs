import { strictEqual, deepStrictEqual } from "node:assert/strict";
import { test } from "node:test";
import { swipeInit, swipeSnapshot, swipeSave, swipeLoad } from "../web/modules/swipe.js";

// --- swipeInit: creates one variant at index 0 ---
test("swipeInit creates a single variant at index 0", () => {
  const m = { text: "hello", reasoning: "why", tools: null, meta: null, thinkSecs: 2 };
  swipeInit(m);
  strictEqual(Array.isArray(m.swipes), true);
  strictEqual(m.swipes.length, 1);
  strictEqual(m.swipeIndex, 0);
});

// --- swipeSave: captures text/reasoning/tools/meta/thinkSecs ---
test("swipeSave captures all fields", () => {
  const m = { text: "t1", reasoning: "r1", tools: ["a"], meta: { x: 1 }, thinkSecs: 5 };
  swipeInit(m);
  swipeSave(m);
  const snap = swipeSnapshot(m);
  strictEqual(snap.text, "t1");
  strictEqual(snap.reasoning, "r1");
  deepStrictEqual(snap.tools, ["a"]);
  deepStrictEqual(snap.meta, { x: 1 });
  strictEqual(snap.thinkSecs, 5);
});

// --- swipeLoad: restores text after change ---
test("swipeLoad restores text after m.text is changed", () => {
  const m = { text: "orig", reasoning: "r", tools: null, meta: null, thinkSecs: 1 };
  swipeInit(m);
  swipeSave(m);
  m.text = "changed";
  m.swipeIndex = 0;
  swipeLoad(m);
  strictEqual(m.text, "orig");
});

// --- second variant loaded via swipeIndex = 1 ---
test("a second saved variant loads when swipeIndex = 1", () => {
  const m = { text: "v0", reasoning: "r", tools: null, meta: null, thinkSecs: 1 };
  swipeInit(m);
  swipeSave(m);                       // variant 0 captured
  m.text = "v1";                      // change text
  m.swipes.push(swipeSnapshot(m));    // push variant 1
  m.swipeIndex = 1;                   // point at variant 1
  swipeLoad(m);                       // restore from variant 1
  strictEqual(m.text, "v1");
});
