import { strictEqual, deepStrictEqual } from "node:assert/strict";
import { test } from "node:test";
import * as mod from "../web/modules/sampling.js";

// --- samplingUntouched ---
test("samplingUntouched accepts default values", () => {
  strictEqual(mod.samplingUntouched({temperature: 1.0, top_p: 0.95, top_k: 20}), true);
});

test("samplingUntouched accepts legacy 0.6 temperature", () => {
  strictEqual(mod.samplingUntouched({temperature: 0.6, top_p: 0.95, top_k: 20}), true);
});

test("samplingUntouched rejects changed top_k", () => {
  strictEqual(mod.samplingUntouched({temperature: 1.0, top_p: 0.95, top_k: 10}), false);
});

// --- loadSettings ---
test("loadSettings with empty store returns DEFAULTS with temperature restored", () => {
  const fakeStore = {get: () => ({})};
  const result = mod.loadSettings(fakeStore);
  deepStrictEqual(result, {...mod.DEFAULTS, temperature: mod.DEFAULTS.temperature});
});

test("loadSettings with stored 0.6 sets temperature to DEFAULTS.temperature", () => {
  const fakeStore = {get: () => ({sampling: {temperature: 0.6}})};
  const result = mod.loadSettings(fakeStore);
  strictEqual(result.temperature, mod.DEFAULTS.temperature);
});

// --- sharedDefaults ---
test("sharedDefaults with temperature 0 returns reasoning_effort and temperature 0", () => {
  const s = {thinking: "high", temperature: 0, top_p: 0.95, top_k: 20};
  const result = mod.sharedDefaults(s, false);
  strictEqual(result.reasoning_effort, "high");
  strictEqual(result.temperature, 0);
});

test("sharedDefaults with untouched sampling returns only reasoning_effort (plus speed flag)", () => {
  const s = {thinking: "high", temperature: 1.0, top_p: 0.95, top_k: 20};
  const result = mod.sharedDefaults(s, true);
  strictEqual(result.reasoning_effort, "high");
  strictEqual(result.temperature, undefined);
  strictEqual(result.experimental_speed_projection, true);
});

test("sharedDefaults with changed sampling includes temperature/top_p/top_k", () => {
  const s = {thinking: "high", temperature: 0.7, top_p: 0.8, top_k: 10};
  const result = mod.sharedDefaults(s, false);
  strictEqual(result.reasoning_effort, "high");
  strictEqual(result.temperature, 0.7);
  strictEqual(result.top_p, 0.8);
  strictEqual(result.top_k, 10);
});
