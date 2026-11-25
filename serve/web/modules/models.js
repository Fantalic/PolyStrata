// Health, model list, context and the vision/KV toggles. Alpine owns the rendered state.
import { $, toast, headers, state } from "./runtime.js";

// ------------------------------------------------------------------ health and models
export async function loadHealth() {
  try {
    state.health = await (await fetch("health")).json();
    // The dropdown shows the context the engine actually holds (/health.max_context), not just the config.
    if (state.health.max_context) {
      const v = String(state.health.max_context);
      if (!state.contextOptions.some((o) => o.value === v)) state.contextOptions.push({value: v, label: `${Math.round(Number(v) / 1024)}K`});
      state.contextValue = v;
      state.syncSelectors?.();
    }
    $("attach-btn").title = state.health.images ? "Attach a text file or a picture (or drop it here)"
                                              : "Attach a text file (or drop it here)";
    $("chat-empty-sub").textContent = `${state.health.model} runs on this PC. Nothing leaves it.`;
    state.imageTabVisible = !!state.health.image_generation;
    if (location.hash === "#images" && state.health.image_generation) state.showTab("images");
    if (state.tab === "requests") state.showRequests();
  } catch (e) {
    setTimeout(loadHealth, 2000);
  }
}

// The installed models (GET api/models).  The list is shown when there is more than one to choose from; choosing
// one switches the server to it, which stops the model in memory first and starts a new chat.
export async function loadModels() {
  let models = [];
  try {
    const r = await fetch("api/models", {headers: headers()});
    if (r.ok) models = (await r.json()).models || [];
  } catch (e) { return; }
  state.models = models.map((m) => ({
    id: m.id,
    label: m.display_name && m.display_name !== m.id ? `${m.display_name} (${m.id})` : m.id,
    disabled: m.status === "unavailable"
  }));
  const cur = models.find((m) => m.current) || {};
  state.currentModelId = cur.id || "";
  state.previousModelId = state.currentModelId;
  // Prefer the engine's live context when this is the loaded model; the config is the fallback.
  const actual = (state.health.model === cur.id && state.health.max_context) ? state.health.max_context : cur.context;
  if (actual) {
    const v = String(actual);
    if (!state.contextOptions.some((o) => o.value === v)) state.contextOptions.push({value: v, label: `${Math.round(Number(v) / 1024)}K`});
    state.contextValue = v;
  }
  state.syncSelectors?.();
}
export async function switchModel() {
  const name = state.currentModelId, before = state.previousModelId;
  if (state.busy) { toast("warn", "Still writing", "Stop the answer first."); state.currentModelId = before; state.syncSelectors?.(); return; }
  state.modelSelectDisabled = true;
  toast("info", `Switching to ${name}`, "The model in memory is stopped first, then this one is loaded.", 8000);
  let message = "";
  try {
    const r = await fetch("api/models/load", {method: "POST", headers: headers(true), body: JSON.stringify({model: name})});
    if (!r.ok) message = ((await r.json()).error || {}).message || `the server answered ${r.status}`;
  } catch (e) { message = String(e); }
  state.modelSelectDisabled = false;
  await loadHealth();
  await loadModels();
  if (message) { toast("error", "The model was not switched", message, 10000); return; }
  state.messages = [];
  state.saveChat();
  state.renderChat();
  state.previousModelId = name;
  toast("success", `${name} is loaded`, "A new chat was started.");
}

// ------------------------------------------------------------------ vision toggle
export async function loadVision() {
  try {
    const r = await fetch("api/vision", {headers: headers()});
    if (!r.ok) return;
    const s = await r.json();
    state.vision = {present: !!s.present, enabled: !!s.enabled, elastic: !!s.elastic, running: !!s.running};
  } catch (e) { /* ignore; the button is still usable */ }
}
export async function toggleVision() {
  if (!state.vision.elastic) return;
  const next = !state.vision.enabled;
  try {
    const r = await fetch("api/vision", {method: "POST", headers: headers(true), body: JSON.stringify({on: next})});
    if (!r.ok) { const j = await r.json(); toast("error", "Vision toggle", (j.error || {}).message || `server answered ${r.status}`, 8000); return; }
    const s = await r.json();
    state.vision.enabled = !!s.enabled;
    toast("success", `Vision ${next ? "enabled" : "disabled"}`, "The elastic encoder is now " + (next ? "running" : "stopped"), 4000);
  } catch (e) { toast("error", "Vision toggle", String(e), 6000); }
}
// ------------------------------------------------------------------ kv-resident toggle
export async function loadKvResident() {
  try {
    const r = await fetch("config", {headers: headers()});
    if (!r.ok) return;
    const v = await r.json();
    const key = (v.keys || []).find((k) => k.key === "kv_resident");
    // A null value means the engine uses its default path: no toggle, byte-identical to the release.
    state.kv = {present: !!key && key.value != null, enabled: !!key && key.value != null};
  } catch (e) { /* ignore; the button is still usable */ }
}
export async function toggleKv() {
  if (!state.kv.present) return;
  const next = !state.kv.enabled;
  try {
    const body = next ? {set: {kv_resident: 16384, kv: "q4_0"}} : {set: {kv_resident: null, kv: null}};
    const r = await fetch("config", {method: "POST", headers: headers(true), body: JSON.stringify(body)});
    if (!r.ok) { const j = await r.json(); toast("error", "KV resident toggle", (j.error || {}).message || `server answered ${r.status}`, 8000); return; }
    state.kv.enabled = next;
    toast("success", `KV resident ${next ? "enabled" : "disabled"}`, "Used from the next model start — use \"Save & restart model\"", 4000);
  } catch (e) { toast("error", "KV resident toggle", String(e), 6000); }
}
