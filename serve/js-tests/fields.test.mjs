import { strictEqual } from "node:assert/strict";
import { test } from "node:test";
import * as mod from "../web/modules/fields.js";

// --- MEM_KEYS ---
test("MEM_KEYS.length === 5", () => {
  strictEqual(mod.MEM_KEYS.length, 5);
});

// --- MEM_FIELDS: every key is in MEM_KEYS ---
test("every MEM_FIELDS key appears in MEM_KEYS", () => {
  const fieldKeys = mod.MEM_FIELDS.map((f) => f.key);
  for (const key of fieldKeys) {
    strictEqual(mod.MEM_KEYS.includes(key), true, `key ${key} should be in MEM_KEYS`);
  }
});

// --- MODEL_FIELDS ---
test("MODEL_FIELDS.length === 22", () => {
  strictEqual(mod.MODEL_FIELDS.length, 22);
});

test("every MODEL_FIELDS entry has key, name, what, does, eg, hint", () => {
  const required = ["key", "name", "what", "does", "eg", "hint"];
  for (const field of mod.MODEL_FIELDS) {
    for (const prop of required) {
      strictEqual(field.hasOwnProperty(prop), true, `field ${field.key} missing ${prop}`);
    }
  }
});

test("no MODEL_FIELDS key is in MEM_KEYS", () => {
  for (const field of mod.MODEL_FIELDS) {
    const baseKey = field.key.split(".")[0];
    strictEqual(mod.MEM_KEYS.includes(field.key), false, `${field.key} should not overlap with MEM_KEYS`);
    strictEqual(mod.MEM_KEYS.includes(baseKey), false, `${baseKey} should not overlap with MEM_KEYS`);
  }
});
