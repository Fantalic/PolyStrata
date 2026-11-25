// Shared runtime for the PolyStrata web UI: DOM helpers, small utilities and state.
// UI modules import this file instead of importing each other's private state.
import { SPRITE, icon, esc, fmt, kfmt, ctxfmt, gb } from "./format.js";
import { store } from "./store.js";
import { markdown } from "./markdown.js";
import { DEFAULTS, samplingUntouched, loadSettings, sharedDefaults } from "./sampling.js";
import { swipeInit, swipeSnapshot, swipeSave, swipeLoad } from "./swipe.js";
import { MEM_KEYS, MEM_FIELDS, MODEL_FIELDS } from "./fields.js";

export { SPRITE, icon, esc, fmt, kfmt, ctxfmt, gb, store, markdown,
         DEFAULTS, samplingUntouched, loadSettings, sharedDefaults,
         swipeInit, swipeSnapshot, swipeSave, swipeLoad,
         MEM_KEYS, MEM_FIELDS, MODEL_FIELDS };

export const $ = (id) => document.getElementById(id);

// ------------------------------------------------------------------ toasts
export function toast(kind, title, text = "", ms = 3500, action = null) {
  const names = {info: "info", success: "check", warn: "warning", error: "error"};
  const el = document.createElement("div");
  el.className = `st-toast st-toast--${kind}`;
  el.innerHTML = `${icon(names[kind] || "info")}<div><div class="st-toast__title"></div><div class="t-text"></div></div>`;
  el.querySelector(".st-toast__title").textContent = title;
  el.querySelector(".t-text").textContent = text;
  if (action) {
    const b = document.createElement("button");
    b.className = "st-btn st-btn--secondary";
    b.style.height = "32px";
    b.style.marginLeft = "auto";
    b.textContent = action.label;
    b.onclick = () => { action.run(); el.remove(); };
    el.appendChild(b);
  }
  $("toasts").appendChild(el);
  setTimeout(() => el.remove(), ms);
}

export async function copyText(text, btn) {
  try {
    await navigator.clipboard.writeText(text);
  } catch (e) {                                   // http on another host: no async clipboard
    const ta = document.createElement("textarea");
    ta.value = text; document.body.appendChild(ta); ta.select(); document.execCommand("copy"); ta.remove();
  }
  if (btn) {
    const use = btn.querySelector("use");
    use.setAttribute("href", `${SPRITE}#i-check`);
    setTimeout(() => use.setAttribute("href", `${SPRITE}#i-copy`), 1500);
  }
  toast("success", "Copied to clipboard", "", 1800);
}

export function headers(json = false) {
  const h = {};
  const key = store.get("apikey", "");
  if (key) h.Authorization = "Bearer " + key;
  if (json) h["Content-Type"] = "application/json";
  return h;
}

// ------------------------------------------------------------------ shared state
// Modules mutate properties here; app.js fills showTab/showRequests after the modules are loaded.
export let state = {
  tab: "chat",
  health: {model: "strata", images: false, max_context: 0},
  lastMetrics: null,
  metricsFailures: 0,
  keyWarned: false,
  mcpTick: 0,
  reqShowAll: false,
  queueExpanded: false,
  dragId: null,
  lcBadgeCount: 0,
  mcpInfo: {servers: [], tools: 0},
  mcpRetry: null,
  settings: loadSettings(store),
  messages: store.get("chat", []),
  attachments: [],
  busy: null,
  editUndo: null,
  cfgKeys: [],
  memServer: {},
  memModel: "",
  sharedOn: false,
  showTab: null,
  showRequests: null,
  saveChat: null,
  renderChat: null,
  renderMemLive: null,

  // Alpine bindings. Each panel module fills its own fields before Alpine starts.
  theme: "light",
  sprite: "web/sprite.svg",
  apiKey: "",
  imageTabVisible: false,
  models: [],
  currentModelId: "",
  previousModelId: "",
  modelSelectDisabled: false,
  contextOptions: [
    {value: "8192", label: "8K"}, {value: "16384", label: "16K"}, {value: "32768", label: "32K"},
    {value: "65536", label: "64K"}, {value: "98304", label: "96K"}, {value: "131072", label: "128K"},
    {value: "196608", label: "192K"}, {value: "262144", label: "256K"}
  ],
  contextValue: "32768",
  vision: {present: false, enabled: false, elastic: false, running: false},
  kv: {present: false, enabled: false},
  cfgRows: [],
  memRows: [],
  mcpServers: [],
  mcpSummary: "",
  mcpRowSub: "",
  mcpCardHidden: true,
  mcpRowHidden: true,
  drawer: {open: false, draft: {...DEFAULTS}, shared: false, espVisible: false},
  image: {prompt: "", size: "1024x1024", seed: "", busy: false, hint: "", results: []},
  syncSelectors: null,
};

// Alpine wraps the shared state once, at alpine:init, so every module sees the same reactive proxy.
export function activateState(Alpine) {
  state = Alpine.reactive(state);
  return state;
}

// the engine was started with the experimental-speed-projection control vector (INFO cvec=...)
export function projectionLoaded() {
  const c = state.lastMetrics && state.lastMetrics.engine ? state.lastMetrics.engine.cvec : 0;
  return !!c && c !== "0";
}
