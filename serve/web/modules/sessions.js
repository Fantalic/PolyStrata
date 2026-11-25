// The Sessions panel: one row per session we can identify, newest first.
import { $, esc, fmt, state } from "./runtime.js";
import { statusOf, idOf, shortId } from "./session-status.js";

let showHistory = false;
const historyLimit = () => (showHistory ? 25 : 4);

function label(cls) {
  return {idle: "Idle", reading: "Reading", generating: "Generating", queued: "Queued", error: "Error"}[cls] || "Idle";
}

/** Render active identity plus queued, held, focused and finished sessions. */
export function renderSessions(q, m) {
  const list = $("session-list");
  if (!list || !q) return;
  const live = (m && m.live) || {};
  const activeId = live.session_id || "";
  const activeEl = $("session-active-id");
  if (activeEl) {
    activeEl.textContent = activeId
      ? `${live.session_name || "Active session"} · ${shortId(activeId)}${live.agent ? ` · ${live.agent}` : ""}`
      : "";
  }
  const rows = [];
  const seen = new Set();
  const add = (entry, kind, detail) => {
    const id = idOf(entry);
    if (!id || seen.has(id)) return;
    seen.add(id);
    rows.push({id, name: entry.session_name || entry.snippet || shortId(id), cls: statusOf(kind, entry, live), detail});
  };
  for (const w of q.waiting || []) {
    add(w, "queued", `Queued · ${fmt(w.prompt_tokens)} tokens · waited ${fmt(w.waited_s, 1)} s`);
  }
  if (q.hold) add(q.hold, "queued", `Held ${fmt(q.hold.remaining_s, 1)} s for a follow-up`);
  if (q.focused) add(q.focused, "queued", "Focused · waits without timeout");
  const finished = ((m && m.requests) || []).filter(idOf).slice(0, historyLimit());
  for (const r of finished) {
    add(r, "history", `${r.finish === "error" ? "Error" : "Done"} · ${fmt(r.output_tokens)} tokens · ${fmt(r.duration_s, 1)} s`);
  }
  const parked = (m && m.conversation_cache && m.conversation_cache.parked) || 0;
  if (parked) rows.push({id: "parked", name: "Parked conversations", cls: "idle",
                         detail: `${fmt(parked)} parked · identities are not exposed by the engine`});
  if (!rows.length) {
    list.innerHTML = "";                            // no placeholder: the table below is always visible
    return;
  }
  list.innerHTML = rows.map((r) => `<div class="session-row">
    <span class="session-badge session-badge--${r.cls}">${label(r.cls)}</span>
    <span class="session-row__name" title="${esc(r.id)}">${esc(r.name)}</span>
    <span class="muted small session-row__detail">${esc(r.detail)}</span>
    <span class="muted small session-row__id">${esc(shortId(r.id))}</span>
  </div>`).join("");
}

if (typeof document !== "undefined") {
  $("sessions-history-toggle")?.addEventListener("click", () => {
    showHistory = !showHistory;
    const btn = $("sessions-history-toggle");
    btn.setAttribute("aria-expanded", String(showHistory));
    btn.textContent = showHistory ? "Hide history" : "Show history";
    if (state.lastQueue) renderSessions(state.lastQueue, state.lastMetrics);
  });
}
