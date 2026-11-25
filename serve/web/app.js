// serve/web/app.js - the PolyStrata web app: Chat, Monitor, About. No network beyond this server.
// The Monitor tab rebuilds PR #22's dashboard idea (code-martin) on the server's own /metrics.
// This file now only wires the modules together; each UI area lives in serve/web/modules/.
import "./modules/alpine.js";
import { $, toast, state, store } from "./modules/runtime.js";
import { loadMcp } from "./modules/mcp.js";
import { render, poll } from "./modules/monitor.js";
import { loadModels, loadHealth, loadVision, loadKvResident, switchModel, toggleVision, toggleKv } from "./modules/models.js";
import { loadConfig, renderMemLive, openDrawer, closeDrawer, resetSampling, applySampling } from "./modules/settings.js";
import { saveChat, setBusy, renderChat, send } from "./modules/chat.js";
import { generateImage } from "./modules/images.js";

// ------------------------------------------------------------------ theme and tabs
// The head script already set data-theme before first paint; Alpine binds the icon and switch to state.theme.
function applyTheme(t, save) {
  state.theme = t;
  document.documentElement.dataset.theme = t;
  if (save) try { localStorage.setItem("strata.theme", t); } catch (e) { /* ignore */ }
}
const toggleTheme = () => applyTheme(state.theme === "dark" ? "light" : "dark", true);
applyTheme(document.documentElement.dataset.theme || "light", false);
matchMedia("(prefers-color-scheme: dark)").addEventListener("change", (e) => {
  let saved = null;
  try { saved = localStorage.getItem("strata.theme"); } catch (err) { /* ignore */ }
  if (!saved) applyTheme(e.matches ? "dark" : "light", false);
});

const TABS = ["chat", "images", "monitor", "requests", "models", "about"];
function showTab(name) {
  state.tab = TABS.includes(name) ? name : "chat";
  if (state.tab === "images") $("img-prompt").focus();
  if (location.hash.slice(1) !== state.tab) history.replaceState(null, "", state.tab === "chat" ? location.pathname : `#${state.tab}`);
  if (state.tab === "chat") $("input").focus();
  if (state.tab === "monitor") loadMcp();
  if (state.tab === "models") { loadModels(); loadConfig(); loadVision(); loadKvResident(); }
  if (state.tab === "about") loadConfig();
  if (state.tab === "requests") showRequests();
  if (state.lastMetrics) render(state.lastMetrics);
}
// #142: the Requests tab shows the /api-monitor page, loaded the first time the tab opens, or how to turn it on.
// Nothing shows until /health has answered.
function showRequests() {
  const on = state.health.api_monitor, frame = $("requests-frame");
  $("requests-off").hidden = on !== false;
  frame.hidden = on !== true;
  if (on && !frame.getAttribute("src")) frame.src = "api-monitor";
}
window.addEventListener("hashchange", () => showTab(location.hash.slice(1)));

function saveApiKey() {
  const key = state.apiKey.trim();
  state.apiKey = key;
  store.set("apikey", key);
  toast("success", "API key saved", "Kept in this browser only.");
}
state.apiKey = store.get("apikey", "");

// Modules call these through shared state so they never import app.js (which would cycle).
state.showTab = showTab;
state.showRequests = showRequests;
state.saveChat = saveChat;
state.renderChat = renderChat;
state.renderMemLive = renderMemLive;
state.toggleTheme = toggleTheme;
state.saveApiKey = saveApiKey;
state.generateImage = generateImage;
state.openDrawer = openDrawer;
state.closeDrawer = closeDrawer;
state.resetSampling = resetSampling;
state.applySampling = applySampling;
state.switchModel = switchModel;
state.toggleVision = toggleVision;
state.toggleKv = toggleKv;

// ------------------------------------------------------------------ start
setBusy(false);
renderChat();
const startQuestion = new URLSearchParams(location.search).get("q");   // /?q=... starts a chat (a shortcut)
if (startQuestion) history.replaceState(null, "", location.pathname + location.hash);
loadModels();
loadHealth().then(loadMcp).then(() => { if (startQuestion) { $("input").value = startQuestion; send(); } });
showTab(location.hash.slice(1) || "chat");
poll();
