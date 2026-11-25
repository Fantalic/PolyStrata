// Run-config fields, conversation memory, model restarts and the sampling drawer.
import { $, toast, esc, fmt, gb, headers, store, projectionLoaded, sharedDefaults, DEFAULTS,
         MEM_KEYS, MEM_FIELDS, MODEL_FIELDS, state } from "./runtime.js";
import { loadMcp } from "./mcp.js";
import { loadHealth, loadModels } from "./models.js";

// ------------------------------------------------------------------ Model settings (GET / POST /config, #564)
// A few documented keys of the run config (strata-<model>.json), for every client, from the next start on.  The
// server lists them, checks every value and keeps every other key of the file as it is.

export async function loadConfig() {
  let r;
  try { r = await fetch("config", {headers: headers()}); } catch (e) { return; }
  if (!r.ok) { $("cfg-card").hidden = true; return; }       // no run config, an older server, or no key yet
  const c = await r.json();
  state.cfgKeys = (c.keys || []).filter((k) => k.key !== "context" && !MEM_KEYS.includes(k.key));
  renderMemory(c.keys || [], c.file || "");
  $("cfg-file").textContent = c.file || "";
  state.cfgRows = state.cfgKeys.map((k, i) => {
    const f = MODEL_FIELDS.find((x) => x.key === k.key) || {name: k.key, what: k.help, does: "", eg: "", hint: ""};
    const choices = (k.kind === "bool" || k.kind === "enum")
      ? (k.kind === "bool" ? [["true", "on"], ["false", "off"]] : (k.choices || []).map((x) => [x, x]))
      : null;
    const cur = k.value == null ? "" : String(k.value);
    return {
      id: `cfg-${i}`, key: k.key, kind: k.kind, name: f.name, what: f.what || "", does: f.does || "",
      eg: f.eg || "", hint: f.hint || "", tipId: `tip-cfg-${i}`,
      choices: choices ? choices.map(([value, label]) => ({value, label})) : null,
      blankLabel: k.default != null ? `${k.default} ( default )` : "( not set )",
      value: choices ? cur : (k.value == null ? "" : Array.isArray(k.value) ? k.value.join(", ") : String(k.value)),
      original: k.value,
      placeholder: (k.value == null && k.default) ? `${k.default} ( default )` : ""
    };
  });
  $("cfg-card").hidden = false;
}
function configValue(row) {
  const s = String(row.value == null ? "" : row.value).trim();
  if (s === "") return null;
  if (row.kind === "bool") return s === "true";
  if (row.kind === "number") return Number(s);
  return s;                                         // enum, or names (the server splits them at commas)
}
export async function saveConfig() {
  const set = {};
  for (const row of state.cfgRows) {
    const v = configValue(row);
    const old = Array.isArray(row.original) ? row.original.join(", ") : row.original;
    if (JSON.stringify(v) !== JSON.stringify(old ?? null)) set[row.key] = v;
  }
  if (!Object.keys(set).length) { $("cfg-msg").textContent = "Nothing changed."; return true; }
  try {
    const r = await fetch("config", {method: "POST", headers: headers(true), body: JSON.stringify({set})});
    const b = await r.json();
    if (!r.ok) throw new Error((b.error || {}).message || `HTTP ${r.status}`);
    $("cfg-msg").textContent = b.changed.length
      ? `Saved (${b.changed.join(", ")}); the earlier file is ${b.file}.bak.` : "Nothing changed.";
    loadConfig();
    return true;
  } catch (e) {
    $("cfg-msg").textContent = "";
    toast("error", "Not saved", String(e.message || e), 6000);
    return false;
  }
}

// ------------------------------------------------------------------ Conversation memory panel
// The value the server has in the run config for each key, so the form can tell "unchanged" from "edited".

// The localStorage key is per model: switching models reloads that model's panel.  The server's start values
// overwrite the saved edits (they are what the engine actually runs with), so the cache is only a fallback for
// a field the server does not list — e.g. an older server that does not know the new keys.
function memCacheKey() { return "mem." + state.memModel; }

function memValue(k, raw) {
  const s = String(raw == null ? "" : raw).trim();
  if (s === "") return null;                       // default / unset
  if (k.auto && s.toLowerCase() === "auto") return "auto";
  const n = Number(s);
  return Number.isFinite(n) && n >= 0 ? n : null;
}

export function renderMemory(keys, file) {
  const byKey = Object.fromEntries(keys.map((k) => [k.key, k.value]));
  const byDef = Object.fromEntries(keys.map((k) => [k.key, k.default]));
  state.memServer = Object.fromEntries(MEM_KEYS.map((key) => {
    const v = byKey[key];
    return [key, v == null ? null : String(v)];
  }));
  state.memModel = file || "default";
  const cached = store.get(memCacheKey(), {}) || {};
  state.memRows = MEM_FIELDS.map((f) => {
    const dv = byDef[f.key], sv = byKey[f.key];
    return {
      key: f.key, id: `mem-${f.key}`, tipId: `mem-tip-${f.key}`,
      name: f.name, what: f.what, does: f.does, eg: f.eg, hint: f.hint, auto: !!f.auto,
      value: sv == null ? (cached[f.key] != null ? String(cached[f.key]) : "") : String(sv),
      placeholder: sv == null && dv ? `${dv} ( default )` : ""
    };
  });
  $("mem-card").hidden = false;
  $("mem-msg").textContent = "";
  renderMemLive(state.lastMetrics && state.lastMetrics.conversation_cache);
}

// The live readout comes from the same 1 s metrics poll as the Monitor's cache card.  The engine's effective
// policy (settings) is parsed by ConvCacheLog from its CACHE line, so Apply now shows up here within a second.
export function renderMemLive(cc) {
  const el = $("mem-live");
  if (!el || $("mem-card").hidden) return;
  if (!cc) { el.textContent = ""; return; }
  const s = cc.settings || {};
  const parked = cc.parked != null ? `${fmt(cc.parked)} of ${fmt(cc.slots)} chats ready` : "";
  const ram = cc.budget_mib != null ? `${fmt(cc.budget_mib)} MiB RAM budget` : "";
  const disk = s.disk != null ? `disk ${s.disk}` : "";
  const target = s.target != null ? `goal ${s.target} parkable` : "";
  el.textContent = [parked, ram, disk, target].filter(Boolean).join(" · ");
}

// The five values as config keys: an unchanged field is omitted so Save touches only what the user edited.
function memChanges() {
  const set = {};
  for (const row of state.memRows) {
    const v = memValue(row.key, row.value);
    const old = memValue(row.key, state.memServer[row.key]);
    if (JSON.stringify(v) !== JSON.stringify(old)) set[row.key] = v;
  }
  return set;
}

function memCacheSave() {
  const vals = {};
  for (const row of state.memRows) vals[row.key] = row.value;
  store.set(memCacheKey(), vals);
}

// Apply now: the runtime CACHE line, no config write.  Only the engine's five runtime keys are sent.
function memRuntime(set) {
  const map = {cache_mib: "mib", cache_disk_mib: "disk", cache_slots: "slots", cache_focus: "focus",
               cache_ttl_s: "ttl"};
  const out = {};
  for (const [k, v] of Object.entries(set)) if (map[k] && v !== null) out[map[k]] = v;
  return out;
}

$("mem-apply").addEventListener("click", async () => {
  const values = memRuntime(memChanges());
  $("mem-msg").textContent = "Applying…";
  try {
    const r = await fetch("v1/cache/settings", {method: "POST", headers: headers(true),
                                                body: JSON.stringify(values)});
    const b = await r.json();
    if (!r.ok) throw new Error((b.error || {}).message || `HTTP ${r.status}`);
    memCacheSave();
    $("mem-msg").textContent = b.status === "not loaded" ? "Engine not loaded."
      : "Applied to this engine. Save into config to keep it.";
    // Show the new policy immediately rather than waiting for the next log line and metrics poll.
    if (b.slots != null) {
      const s = {disk: b.disk, target: b.target};
      const parked = state.lastMetrics && state.lastMetrics.conversation_cache;
      renderMemLive(parked ? {...parked, settings: s} : ccFromPolicy(b));
    }
  } catch (e) {
    $("mem-msg").textContent = "";
    toast("error", "Not applied", String(e.message || e), 6000);
  }
});

function ccFromPolicy(p) {
  return {parked: null, slots: p.slots, budget_mib: p.park,
          settings: {disk: p.disk, target: p.target}};
}

$("mem-save").addEventListener("click", async () => {
  const set = memChanges();
  if (!Object.keys(set).length) { $("mem-msg").textContent = "Nothing changed."; return true; }
  $("mem-msg").textContent = "Saving…";
  try {
    const r = await fetch("config", {method: "POST", headers: headers(true), body: JSON.stringify({set})});
    const b = await r.json();
    if (!r.ok) throw new Error((b.error || {}).message || `HTTP ${r.status}`);
    memCacheSave();
    $("mem-msg").textContent = b.changed.length
      ? `Saved into config (${b.changed.join(", ")}).` : "Nothing changed.";
    loadConfig();                                   // server values now win again
    return true;
  } catch (e) {
    $("mem-msg").textContent = "";
    toast("error", "Not saved", String(e.message || e), 6000);
    return false;
  }
});

// Save the context from the top dropdown, then stop and load the current model with it.
$("ctx-restart").addEventListener("click", async () => {
  const name = state.currentModelId;
  const ctx = Number(state.contextValue);
  if (!name || !ctx) { toast("warn", "Choose a model and a context window"); return; }
  toast("info", `Restarting ${name}`, `Stopping it and loading it again with ${ctx / 1024}K tokens.`, 8000);
  let message = "";
  try {
    const r = await fetch("config", {method: "POST", headers: headers(true), body: JSON.stringify({set: {context: ctx}})});
    if (!r.ok) message = ((await r.json()).error || {}).message || `the server answered ${r.status}`;
  } catch (e) { message = String(e); }
  if (!message) {
    try { await fetch("api/models/unload", {method: "POST", headers: headers(true), body: "{}"}); } catch (e) { /* it was not loaded */ }
    try {
      const r = await fetch("api/models/load", {method: "POST", headers: headers(true), body: JSON.stringify({model: name})});
      if (!r.ok) message = ((await r.json()).error || {}).message || `the server answered ${r.status}`;
    } catch (e) { message = String(e); }
  }
  await loadHealth();
  await loadModels();
  if (message) { toast("error", "The model was not restarted", message, 10000); return; }
  toast("success", `${name} restarted`, `The context window is ${ctx / 1024}K tokens.`);
});

// Save the settings, then stop and load the current model so a new context size takes effect now.
$("cfg-restart").addEventListener("click", async () => {
  if (!await saveConfig()) return;
  const name = state.currentModelId;
  if (!name) { toast("warn", "No model selected", "Reload the page and try again."); return; }
  toast("info", `Restarting ${name}`, "The model is stopped and loaded again with the saved settings.", 8000);
  try { await fetch("api/models/unload", {method: "POST", headers: headers(true), body: "{}"}); } catch (e) {}
  let message = "";
  try {
    const r = await fetch("api/models/load", {method: "POST", headers: headers(true), body: JSON.stringify({model: name})});
    if (!r.ok) message = ((await r.json()).error || {}).message || `the server answered ${r.status}`;
  } catch (e) { message = String(e); }
  await loadHealth();
  await loadModels();
  if (message) { toast("error", "The model was not restarted", message, 10000); return; }
  toast("success", `${name} restarted`, "The new context size is in effect.");
});

// ------------------------------------------------------------------ the sampling drawer
// Alpine binds the controls to a draft. Apply copies the draft into state.settings; until then chat requests
// keep using the old sampling, which is why the draft is a separate object.
export function openDrawer(open) {
  if (open) {
    state.drawer.draft = {...state.settings};
    state.drawer.shared = !!state.sharedOn;
    state.drawer.espVisible = projectionLoaded();
    loadShared();
    loadMcp();
  }
  state.drawer.open = open;
}
export function closeDrawer() { openDrawer(false); }
export function resetSampling() { state.drawer.draft = {...DEFAULTS}; }

// "Use for other apps too": the server keeps these settings as every client's defaults (GET/POST /settings)
async function loadShared() {
  try {
    const r = await fetch("settings", {headers: headers()});
    if (r.ok) {
      state.sharedOn = !!(await r.json()).shared;
      if (state.drawer.open) state.drawer.shared = !!state.sharedOn;
    }
  } catch (e) { /* an older server: the switch just stays off */ }
}
async function saveShared(on, s) {
  const r = await fetch("settings", {method: "POST", headers: headers(true),
                                      body: JSON.stringify({defaults: on ? sharedDefaults(s, projectionLoaded()) : null})});
  if (!r.ok) {
    let msg = `HTTP ${r.status}`;
    try { msg = (await r.json()).error.message || msg; } catch (e) { /* not json */ }
    throw new Error(msg);
  }
  state.sharedOn = !!(await r.json()).shared;
}
export async function applySampling() {
  const d = state.drawer.draft;
  state.settings = {...state.settings, thinking: d.thinking, temperature: +d.temperature,
    top_p: +d.top_p, top_k: +d.top_k, max: String(d.max || "").trim(), seed: String(d.seed || "").trim(),
    show: !!d.show, esp: d.esp !== false, mcp: d.mcp !== false};
  store.set("sampling", state.settings);
  const share = !!state.drawer.shared;
  openDrawer(false);
  if (share || state.sharedOn) {
    try {
      await saveShared(share, state.settings);
      toast("success", "Sampling saved", share ? "Other apps (omp, API clients) use these settings from their next request."
                                               : "Other apps use their own settings again.");
    } catch (e) {
      toast("error", "Saved here, but not for other apps", e.message, 6000);
    }
    return;
  }
  toast("success", "Sampling saved", state.settings.temperature === 0 ? "Greedy: the same question gives the same answer." : "");
}
document.addEventListener("keydown", (e) => { if (e.key === "Escape" && state.drawer.open) openDrawer(false); });
