"use strict";
// The session view: one conversation bundle's requests (the queue panel's Session button
// opens /web/session.html?bundle=<root_session || session_id>).  The running request's
// output streams live (the monitor keeps appending the streamed deltas to its record);
// finished requests open a detail pane with prompt, answer and timings.
const $ = id => document.getElementById(id);
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
const speed = n => typeof n === "number" ? `${n.toFixed(1)} tok/s` : "—";
const TERMINAL = ["completed", "error", "disconnected"];
const bundle = (new URLSearchParams(location.search).get("bundle") || "").trim();
const sessionOf = r => r.root_session || r.session_id || "";
let selected = null, pane = "output", nowId = null, seen = false;

async function api(path) {
  const response = await fetch(path, {cache: "no-store", headers: apiKey ? {Authorization: `Bearer ${apiKey}`} : {}});
  const value = await response.json();
  if (!response.ok) {
    if (response.status === 401) $("auth").hidden = false;
    throw new Error(value.error?.message || `HTTP ${response.status}`);
  }
  return value;
}
function notice(message) { $("error").hidden = !message; $("error").textContent = message || ""; }

// The output pane stays pinned to the bottom while the stream grows, unless the user
// scrolled up to read.
function stick(el) {
  const pinned = el.scrollTop + el.clientHeight >= el.scrollHeight - 48;
  return () => { if (pinned) el.scrollTop = el.scrollHeight; };
}

const tok = n => typeof n === "number" ? n.toLocaleString() : "—";
function facts(r, running) {
  const u = r.usage || {};
  const wall = typeof r.wallclock_s === "number" ? `${r.wallclock_s.toFixed(2)} s` : running ? `${(Date.now() / 1000 - r.started_at).toFixed(2)} s` : "—";
  const parts = [`wall <b>${wall}</b>`, `queue <b>${seconds(r.queue_s)}</b>`, `first token <b>${seconds(r.first_token_s)}</b>`,
                 `prompt <b>${tok(u.prompt_tokens ?? u.input_tokens)}</b>`,
                 `${running ? "completion" : "generated"} <b>${tok(u.completion_tokens ?? u.output_tokens)}</b>`];
  if (r.timings?.predicted_per_second) parts.push(`<b>${speed(r.timings.predicted_per_second)}</b>`);
  return parts.join(" · ");
}

async function renderNow(r) {
  if (!r) {
    if (nowId !== null) { nowId = null; $("now").hidden = true; $("now-empty").hidden = false; }
    return;
  }
  $("now").hidden = false; $("now-empty").hidden = true;
  $("now-state").textContent = r.state; $("now-state").className = `tag ${r.state}`;
  $("now-snippet").textContent = r.snippet || r.id; $("now-snippet").title = r.snippet || r.id;
  $("now-path").textContent = r.path.replace(/^\/v1/, "");
  // the full record carries the growing output; the light list entry does not
  const full = await api(`/api/requests?id=${encodeURIComponent(r.id)}`);
  if (nowId !== r.id) { $("now-facts").innerHTML = ""; nowId = r.id; }
  $("now-facts").innerHTML = facts({...full, ...r}, !TERMINAL.includes(r.state));
  const out = $("now-output");
  const keep = stick(out);
  out.textContent = full.output || (full.stream ? "Waiting for the first token…" : "No output retained.");
  out.hidden = false;
  keep();
  // stream=false answers arrive in the response body, not as deltas
  if (!full.output && full.response && !full.stream) {
    try { out.textContent = JSON.parse(full.response).choices?.[0]?.message?.content || full.response; }
    catch (_) { out.textContent = full.response; }
  }
}

function renderFinished(done) {
  $("finished-count").textContent = done.length ? `${done.length} retained` : "none yet";
  const rows = $("finished-rows");
  const stamp = JSON.stringify(done.map(r => [r.id, r.state, r.wallclock_s]));
  if (rows.dataset.stamp === stamp) return;
  rows.dataset.stamp = stamp;
  rows.replaceChildren();
  if (!done.length) { rows.innerHTML = '<div class="empty">No finished requests retained (the API monitor keeps the last 100).</div>'; return; }
  for (const r of done) {
    const row = document.createElement("div"); row.className = "srow"; row.dataset.request = r.id;
    const time = document.createElement("span"); time.className = "stime"; time.textContent = new Date(r.started_at * 1000).toLocaleTimeString();
    const tag = document.createElement("span"); tag.className = `tag ${r.state}`; tag.textContent = r.state;
    const snip = document.createElement("span"); snip.className = "ssnip"; snip.title = r.snippet || r.id; snip.textContent = r.snippet || r.id;
    const tok = document.createElement("span"); tok.className = "stok";
    const inp = r.usage?.prompt_tokens ?? r.usage?.input_tokens, out = r.usage?.completion_tokens ?? r.usage?.output_tokens;
    tok.textContent = `${typeof inp === "number" ? inp.toLocaleString() : "—"} → ${typeof out === "number" ? out.toLocaleString() : "—"} tok`;
    const wall = document.createElement("span"); wall.className = "swall"; wall.textContent = seconds(r.wallclock_s);
    row.append(time, tag, snip, tok, wall);
    row.onclick = async () => { selected = r.id; pane = "output"; tabs(); await showDetail(); };
    rows.append(row);
  }
}

function tabs() { document.querySelectorAll("[data-pane]").forEach(b => b.setAttribute("aria-selected", String(b.dataset.pane === pane))); }

async function showDetail() {
  if (!selected) return;
  const id = selected;
  const value = await api(`/api/requests?id=${encodeURIComponent(id)}`);
  if (selected !== id) return;
  $("detail-panel").hidden = false;
  $("detail-title").textContent = `${value.id} · ${value.path.replace(/^\/v1/, "")}`;
  $("detail-state").textContent = value.state; $("detail-state").className = `tag ${value.state}`;
  $("detail-facts").innerHTML = facts(value, !TERMINAL.includes(value.state));
  let text = value[pane] || "No content.";
  if (pane === "input") { try { text = JSON.stringify(JSON.parse(text), null, 2); } catch (_) {} }
  $("detail-content").textContent = text + (value[`${pane}_truncated`] ? "\n\n[monitor capture truncated at 256K characters]" : "");
}

async function poll() {
  try {
    if (!bundle) throw new Error("No session id in the link (?bundle=… is missing).");
    const history = await api("/api/requests");
    const mine = (history.requests || []).filter(r => sessionOf(r) === bundle);
    if (mine.length) seen = true;
    if (!mine.length && !seen) {
      $("s-title").textContent = "Session (nothing retained yet)";
    } else {
      const newest = mine[0];
      $("s-title").textContent = newest.session_name || newest.snippet || bundle;
      const agents = [...new Set(mine.map(r => r.agent).filter(Boolean))];
      $("s-meta").textContent = `${agents.length ? agents.join(", ") + " · " : ""}bundle ${bundle}`;
    }
    const active = mine.filter(r => !TERMINAL.includes(r.state));
    await renderNow(active[0]);
    renderFinished(mine.filter(r => TERMINAL.includes(r.state)));
    if (selected && !mine.some(r => r.id === selected)) { selected = null; $("detail-panel").hidden = true; }
    if (selected) await showDetail();
    notice("");
  } catch (error) { notice(error.message); }
}

$("detail-close").onclick = () => { selected = null; $("detail-panel").hidden = true; };
document.querySelectorAll("[data-pane]").forEach(b => b.onclick = () => { pane = b.dataset.pane; tabs(); if (selected) showDetail(); });
$("key").onclick = () => { $("auth").hidden = !$("auth").hidden; if (!$("auth").hidden) $("api-key").focus(); };
$("auth").onsubmit = async event => { event.preventDefault(); apiKey = $("api-key").value.trim(); sessionStorage.setItem("strata.monitor.key", apiKey); $("auth").hidden = true; seen = true; poll(); };
$("theme").onclick = () => { const theme = document.documentElement.dataset.theme === "dark" ? "light" : "dark"; document.documentElement.dataset.theme = theme; localStorage.setItem("strata.theme", theme); };

(async () => { await poll(); setTimeout(function loop() { poll(); setTimeout(loop, 2000); }, 2000); })();
