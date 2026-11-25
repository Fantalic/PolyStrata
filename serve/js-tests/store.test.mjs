import { strictEqual, deepStrictEqual } from "node:assert/strict";
import { test } from "node:test";
import { store } from "../web/modules/store.js";

let fakeStore;
function makeStorage(map) {
  globalThis.localStorage = {
    getItem(key) { return map.has(key) ? map.get(key) : null; },
    setItem(key, val) { map.set(key, val); },
    removeItem(key) { map.delete(key); },
    get length() { return map.size; },
    key(i) { return [...map.keys()][i] ?? null; },
  };
}

// --- default value on missing key ---
test("store.get returns default for missing key", () => {
  fakeStore = new Map();
  makeStorage(fakeStore);
  strictEqual(store.get("missing", 42), 42);
});

// --- "strata." prefix ---
test('store.get uses "strata." prefix', () => {
  fakeStore = new Map();
  makeStorage(fakeStore);
  store.set("key", "val");
  strictEqual(fakeStore.has("strata.key"), true);
  strictEqual(store.get("key"), "val");
});

// --- JSON round-trip ---
test("store.get/set JSON round-trips", () => {
  fakeStore = new Map();
  makeStorage(fakeStore);
  const obj = { a: 1, b: [2, 3] };
  store.set("rt", obj);
  deepStrictEqual(store.get("rt"), obj);
});

// --- invalid JSON falls back to default ---
test("store.get falls back to default on invalid JSON", () => {
  fakeStore = new Map();
  makeStorage(fakeStore);
  fakeStore.set("strata.bad", "{invalid json");
  strictEqual(store.get("bad", 99), 99);
});

// --- set never throws when storage throws ---
test("store.set never throws on storage failure", () => {
  globalThis.localStorage = {
    setItem() { throw new Error("quota"); },
    getItem() { return null; },
  };
  store.set("nope", true); // should not throw
});
