"use strict";
const $ = id => document.getElementById(id);
let selected = null, detail = null, pane = "input", records = [], state = null, operating = false, stamp = "", actionError = "";
// #142: inside the web app's Requests tab, the app's header, theme and saved API key stand in for this page's own
let host = null;
try { if (window.parent !== window) host = parent.document.documentElement; } catch (e) { /* framed by another site */ }
let apiKey = sessionStorage.getItem("strata.monitor.key") || (host && JSON.parse(localStorage.getItem("strata.apikey") || '""')) || "";
document.documentElement.dataset.theme = localStorage.getItem("strata.theme") || "dark";
if (host) {
  const sync = () => { document.documentElement.dataset.theme = host.dataset.theme; };
  document.documentElement.classList.add("embedded");
  sync();
  new MutationObserver(sync).observe(host, {attributes: true, attributeFilter: ["data-theme"]});
}
const seconds = n => typeof n === "number" ? `${n.toFixed(2)} s` : "—";
const speed = n => typeof n === "number" ? n.toFixed(1) : "—";
const active = r => !["completed", "error", "disconnected"].includes(r.state);
function text(id, value) { const el = $(id); if (el.textContent !== String(value)) el.textContent = value; }
async function api(path, body) {
  const response = await fetch(path, {cache:"no-store", headers:{...(apiKey ? {Authorization:`Bearer ${apiKey}`} : {}), ...(body !== undefined ? {"Content-Type":"application/json"} : {})}, ...(body !== undefined ? {method:"POST", body:JSON.stringify(body)} : {})});
  const value = await response.json();
  if (!response.ok) {
    if (response.status === 401) $("auth").hidden = false;
    throw new Error(value.error?.message || `HTTP ${response.status}`);
  }
  return value;
}
function notice(message) { $("error").hidden = !message; text("error", message || ""); }
function list() {
  const query = $("search").value.toLowerCase(), filter = $("filter").value;
  const visible = records.filter(r => `${r.id} ${r.path} ${r.model}`.toLowerCase().includes(query) && (filter === "all" || (filter === "active" ? active(r) : r.state === filter)));
  const nextStamp = JSON.stringify([visible, selected]);
  if (stamp === nextStamp) return;
  stamp = nextStamp;
  const focusId = document.activeElement?.dataset.request;
  $("requests").replaceChildren();
  if (!visible.length) {
    const empty = document.createElement("div"); empty.className = "empty";
    empty.textContent = records.length ? "No matching requests." : "No requests yet. API calls will appear here automatically.";
    $("requests").append(empty);
  }
  for (const r of visible) {
    const button = document.createElement("button"); button.className = `request${selected === r.id ? " selected" : ""}`;
    button.dataset.request = r.id; button.setAttribute("aria-label", `Request ${r.id}`); button.setAttribute("aria-pressed", String(selected === r.id));
    const row = document.createElement("div"); row.className = "row";
    const time = document.createElement("span"); time.textContent = new Date(r.started_at * 1000).toLocaleTimeString();
    const tag = document.createElement("span"); tag.className = `tag ${r.state}`; tag.textContent = r.state;
    row.append(time, tag);
    const endpoint = document.createElement("div"); endpoint.className = "endpoint"; endpoint.textContent = `POST ${r.path}`;
    const meta = document.createElement("div"); meta.className = "small";
    meta.textContent = `${r.id} · ${seconds(r.wallclock_s)} · ${r.stream ? "stream" : "JSON response"}${r.http_status ? ` · HTTP ${r.http_status}` : ""}`;
    button.append(row, endpoint, meta);
    button.onclick = async () => { selected = r.id; detail = null; list(); await showDetail(); };
    $("requests").append(button);
    if (focusId === r.id) button.focus({preventScroll:true});
  }
}
function content() {
  if (!detail) return;
  let value = detail[pane] || (pane === "response" && detail.stream ? "Streaming response: see Output and the request error, if any." : "No content.");
  if (pane === "output") { try { value = JSON.stringify(JSON.parse(value), null, 2); } catch (_) {} }
  text("content", value);
  const note = pane === "output" && detail.error ? "Raw model output retained for diagnosis. The API returned an error." : pane === "input" ? "Original request body." : pane === "output" ? "Model answer. JSON is formatted here for readability." : pane === "reasoning" ? "Separate reasoning content, when enabled." : "API response body.";
  text("content-note", note + (detail[`${pane}_truncated`] ? " Monitor capture truncated at 256K characters; the API response was not shortened." : ""));
}
async function showDetail() {
  if (!selected) return;
  const id = selected;
  const value = await api(`/api/requests?id=${encodeURIComponent(id)}`);
  if (selected !== id) return;
  detail = value; $("selection").hidden = false; $("no-selection").hidden = true;
  text("detail-title", `${value.id} · ${value.path}`);
  text("detail-status", value.state); $("detail-status").className = `tag ${value.state}`;
  text("detail-time", new Date(value.started_at * 1000).toLocaleString());
  text("detail-wall", seconds(value.wallclock_s ?? ((Date.now() / 1000) - value.started_at)));
  text("detail-load", seconds(value.load_s)); text("detail-queue", seconds(value.queue_s)); text("detail-first", seconds(value.first_token_s));
  text("detail-prompt", value.usage?.prompt_tokens ?? value.usage?.input_tokens ?? "—");
  text("detail-tokens", value.usage?.completion_tokens ?? value.usage?.output_tokens ?? "—");
  text("detail-speed", speed(value.timings?.predicted_per_second)); text("detail-format", value.response_format || "text");
  $("detail-error").hidden = !value.error; text("detail-error", value.error?.message || ""); content();
}
async function refresh() {
  try {
    const [status, history, queue] = await Promise.all([api("/v1/status"), api("/api/requests"), api("/api/queue")]);
    state = status; records = history.requests;
    status.loaded = history.loaded; status.auto_load = history.auto_load;
    renderQueue(queue);
    if (selected && !records.some(r => r.id === selected)) {
      selected = detail = null; $("selection").hidden = true; $("no-selection").hidden = false;
    }
    const inflight = Math.max(status.activity.in_flight, records.filter(active).length);
    const loading = records.some(r => r.state === "loading");
    text("loaded", loading ? "Loading…" : inflight ? "In use" : status.loaded ? "Loaded" : "Unloaded");
    $("model-state").className = `value ${inflight || loading ? "busy" : status.loaded ? "loaded" : ""}`;
    text("model", status.model); text("count", records.length);
    text("active", inflight ? `${inflight} active / queued` : status.auto_load ? "Loads automatically on the next request" : "No active requests");
    const last = records.find(r => !active(r));
    text("wall", last ? seconds(last.wallclock_s) : "—"); text("speed", speed(last?.timings?.predicted_per_second));
    $("load").disabled = operating || Boolean(inflight) || status.loaded; $("unload").disabled = operating || Boolean(inflight) || !status.loaded;
    list(); if (selected) await showDetail();
    text("updated", `Live · updated ${new Date().toLocaleTimeString()}`);
    if (!operating) notice(actionError);
    // Update the compressed badge whenever the monitor refreshes.
    updateCompressedBadge();
  } catch (error) { notice(error.message); text("updated", "Disconnected · retrying"); $("load").disabled = $("unload").disabled = true; }
}

// ------------------------------------------------------------------ request queue drag-and-drop
let dragId = null;

async function reorderQueue(order) {
  try {
    await api("/api/queue/reorder", {order});
    await refresh();
  } catch (e) { notice(e.message); }
}

function renderQueue(q) {
  const open = !$("queue-body").hidden;
  $("aff-on").checked = !!q.settings.enabled;
  if (document.activeElement !== $("hold-s")) $("hold-s").value = q.settings.hold_s;
  $("keep-def").checked = !!q.settings.wanted_default;
  $("aff-apply").disabled = !open;
  $("cc-clear").disabled = !open;
  $("skip-wait").disabled = !open || !(q.hold || q.focused);
  const state = [];
  if (q.focused) state.push(`Focused: ${q.focused.snippet || q.focused.id} (waits without timeout for its follow-up)`);
  if (q.hold) state.push(`Holding the turn for the last conversation's follow-up: ${q.hold.remaining_s} s left`);
  if (!q.settings.enabled) state.push("Affinity is off: requests take the turn in arrival order.");
  text("queue-state", state.join("  ·  "));
  const rows = $("queue-rows");
  const next = JSON.stringify([q.waiting.map(w => [w.id, w.wanted ? 1 : 0]), q.focused?.id]);
  if (rows.dataset.stamp === next) return;
  rows.dataset.stamp = next;
  rows.replaceChildren();
  if (!q.waiting.length) {
    const empty = document.createElement("div"); empty.className = "queue-empty";
    empty.textContent = "No requests waiting. Queued API calls appear here with their session.";
    rows.append(empty);
  }
  for (const w of q.waiting) {
    const row = document.createElement("div"); row.className = "qrow";
    row.draggable = true;
    row.dataset.id = w.id;
    // grip icon
    const grip = document.createElement("span"); grip.className = "qgrip";
    grip.innerHTML = `<svg xmlns="http://www.w3.org/2000/svg" class="icon" style="width:14px;height:14px;display:block"><use href="/web/sprite.svg#i-grip"></use></svg>`;
    row.append(grip);
    const time = document.createElement("span"); time.className = "qtime"; time.textContent = new Date(w.arrived * 1000).toLocaleTimeString();
    const tok = document.createElement("span"); tok.className = "qtok"; tok.textContent = `${w.prompt_tokens.toLocaleString()} tok${w.small ? " · small" : ""}`;
    const tag = document.createElement("span"); tag.className = `tag ${w.state}`; tag.textContent = w.state;
    const snip = document.createElement("span"); snip.className = "qsnippet"; snip.title = w.snippet || w.id; snip.textContent = w.snippet || w.id;
    const path = document.createElement("span"); path.className = "qpath"; path.textContent = w.path.replace(/^\/v1/, "");
    row.append(time, tok, tag, snip, path);
    const act = document.createElement("button");
    if (q.focused?.id === w.id) {
      act.textContent = "Unfocus"; act.onclick = async () => { try { await api("/api/queue/unfocus", {}); await refresh(); } catch (e) { notice(e.message); } };
    } else {
      act.textContent = "Focus"; act.onclick = async () => { try { await api("/api/queue/focus", {id: w.id}); await refresh(); } catch (e) { notice(e.message); } };
    }
    row.append(act);
    // keep: protects this session's parked bank from the engine's TTL and RAM-pressure eviction
    const keep = document.createElement("button");
    keep.textContent = w.wanted ? "Kept" : "Keep";
    keep.disabled = !open;
    keep.onclick = async () => { try { await api("/api/queue/wanted", {id: w.id, wanted: !w.wanted}); await refresh(); } catch (e) { notice(e.message); } };
    row.append(keep);
    rows.append(row);
  }
  // drag-and-drop delegation on the tbody
  rows.addEventListener("dragstart", (e) => {
    const row = e.target.closest(".qrow");
    if (!row) return;
    dragId = row.dataset.id;
    row.classList.add("dragging");
    e.dataTransfer.effectAllowed = "move";
    e.dataTransfer.setData("text/plain", dragId);
  });
  rows.addEventListener("dragend", (e) => {
    const row = e.target.closest(".qrow");
    if (row) row.classList.remove("dragging");
    dragId = null;
  });
  rows.addEventListener("dragover", (e) => {
    const row = e.target.closest(".qrow");
    if (!row) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = "move";
    // remove previous drag-over, set on target
    const prev = rows.querySelector(".qrow.drag-over");
    if (prev) prev.classList.remove("drag-over");
    row.classList.add("drag-over");
  });
  rows.addEventListener("dragleave", (e) => {
    const row = e.target.closest(".qrow");
    if (row) row.classList.remove("drag-over");
  });
  rows.addEventListener("drop", async (e) => {
    e.preventDefault();
    const target = e.target.closest(".qrow");
    if (!target) return;
    // remove drag-over from all
    rows.querySelectorAll(".qrow.drag-over").forEach(r => r.classList.remove("drag-over"));
    rows.querySelectorAll(".qrow.dragging").forEach(r => r.classList.remove("dragging"));
    const draggedId = e.dataTransfer.getData("text/plain");
    if (!draggedId) return;
    // build new order from visible rows
    const newOrder = [...rows.querySelectorAll(".qrow")]
      .filter(r => r.dataset.id)
      .map(r => r.dataset.id);
    // check if order actually changed
    const oldOrder = q.waiting.map(w => w.id);
    if (newOrder.join(",") === oldOrder.join(",")) return;
    await reorderQueue(newOrder);
  });
}
async function control(load) {
  actionError = "";
  operating = true; $("load").disabled = $("unload").disabled = true;
  text("loaded", load ? "Loading…" : "Unloading…");
  try { await api(load ? "/load" : "/unload", {}); notice(""); }
  catch (error) { actionError = error.message; notice(actionError); }
  finally { operating = false; await refresh(); }
}
$("load").onclick = () => control(true); $("unload").onclick = () => control(false);
$("queue-toggle").onclick = () => { const hidden = !$("queue-body").hidden; $("queue-body").hidden = hidden; $("queue-toggle").textContent = hidden ? "Show" : "Hide"; $("aff-apply").disabled = hidden; if (!hidden) refresh(); };
$("aff-apply").onclick = async () => { try { await api("/api/queue/settings", {enabled: $("aff-on").checked, hold_s: Number($("hold-s").value), wanted_default: $("keep-def").checked}); notice(""); await refresh(); } catch (e) { notice(e.message); } };
$("skip-wait").onclick = async () => { try { await api("/api/queue/skip", {}); await refresh(); } catch (e) { notice(e.message); } };
// clear-parked: the main page's #cc-clear does the same POST; the engine frees every parked bank
// except the running conversation and answers with its figures ({"status":"dropped",dropped,bytes})
$("cc-clear").onclick = async () => { try { const d = await api("/v1/cache/drop", {}); text("cc-clear-note", d.status === "dropped" ? `Cleared ${d.dropped} bank${d.dropped === 1 ? "" : "s"} (${(d.bytes / 1073741824).toFixed(1)} GiB)` : (d.note || d.status || "nothing parked")); } catch (e) { notice(e.message); } };
$("key").onclick = () => { $("auth").hidden = !$("auth").hidden; if (!$("auth").hidden) $("api-key").focus(); };
$("auth").onsubmit = async event => { event.preventDefault(); apiKey = $("api-key").value.trim(); sessionStorage.setItem("strata.monitor.key", apiKey); $("auth").hidden = true; await refresh(); };
$("theme").onclick = () => { const theme = document.documentElement.dataset.theme === "dark" ? "light" : "dark"; document.documentElement.dataset.theme = theme; localStorage.setItem("strata.theme", theme); };
$("search").oninput = list; $("filter").onchange = list;
document.querySelectorAll("[data-pane]").forEach(button => { button.onclick = () => { pane = button.dataset.pane; document.querySelectorAll("[data-pane]").forEach(b => b.setAttribute("aria-selected", String(b === button))); content(); }; });
$("copy").onclick = async () => { try { await navigator.clipboard.writeText($("content").textContent); text("copy", "Copied"); setTimeout(() => text("copy", "Copy"), 1500); } catch (_) { notice("Clipboard unavailable; select the text to copy it."); } };
async function poll() { await refresh(); setTimeout(poll, 2000); }
poll();

// ------------------------------------------------------------------ Compressed badge: local-context compression indicator
// Shows a "Compressed" badge when the local-context plugin has folded/
// compressed the current session's context. Checks the local-context debug
// server (port 8765) and looks for any session with folded===true and
// chunkCount>0. The badge appears inline with the model state indicator.
async function checkLocalContextCompression() {
  try {
    const res = await fetch("http://127.0.0.1:8765/api/local-context/state", {
      cache: "no-store",
    });
    if (!res.ok) return false;
    const data = await res.json();
    // Show the badge when any session has been compressed (folded with chunks).
    // The monitor does not track individual sessions, so we check all sessions.
    const sessions = data.sessions || [];
    return sessions.some(
      (s) => s.folded === true && s.chunkCount > 0,
    );
  } catch {
    /* local-context server not running */
    return false;
  }
}

async function updateCompressedBadge() {
  const compressed = await checkLocalContextCompression();
  const badge = $("compressed-badge");
  if (badge) {
    badge.hidden = !compressed;
    badge.style.display = compressed ? "" : "none";
  }
}

// ------------------------------------------------------------------ Local context debug badge
let lastCompressionCount = -1;
async function pollLocalContext() {
  try {
    const res = await fetch("http://127.0.0.1:8765/api/local-context/state", {cache:"no-store"});
    if (!res.ok) return;
    const state = await res.json();
    const count = (state.compression || []).length;
    if (count !== lastCompressionCount) {
      lastCompressionCount = count;
      const badge = $("debug-badge");
      if (count > 0) {
        badge.textContent = count;
        badge.hidden = false;
        badge.style.background = "var(--st-accent-tint)";
        badge.style.color = "var(--st-accent-text)";
      } else {
        badge.hidden = true;
      }
    }
  } catch { /* local-context server not running */ }
  // Also update the compressed badge from the local-context poll.
  updateCompressedBadge();
}
setInterval(pollLocalContext, 5000);
pollLocalContext();
