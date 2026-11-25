// The Sessions panel: every identifiable session, newest first, with expandable latest input.
import { $, esc, fmt, headers, toast, state } from "./runtime.js";
import { statusOf, idOf, shortId, queueActionOf, pinStateOf } from "./session-status.js";

let showHistory = false;
const expanded = new Set();                       // row keys whose latest input is open
const historyLimit = () => (showHistory ? 25 : 4);

function label(cls) {
  return {idle: "Idle", reading: "Reading", generating: "Generating", queued: "Queued", error: "Error"}[cls] || "Idle";
}

function row(r) {
  const open = expanded.has(r.key);
  const pin = r.bundle
    ? `<button class="st-btn session-row__btn session-row__pin" data-session-pin="${r.pinned ? "unpin" : "pin"}" data-session-bundle="${esc(r.bundle)}" data-session-snippet="${esc(r.snippet)}" type="button" aria-pressed="${r.pinned}" title="${r.pinned ? "Unpin this session" : "Pin this session's next request"}">${r.pinned ? "Unpin" : "Pin"}</button>`
    : "";
  const action = r.action
    ? `<button class="st-btn session-row__btn" data-session-action="${r.action}" data-queue-id="${esc(r.queueId)}" type="button">${r.action === "pause" ? "Pause" : "Play"}</button>`
    : "";
  const toggle = r.snippet
    ? `<button class="st-btn session-row__btn session-row__toggle" data-session-expand="${esc(r.key)}" type="button" aria-expanded="${open}" aria-label="Show latest input">${open ? "▴" : "▾"}</button>`
    : "";
  const detail = r.snippet
    ? `<div class="session-row__expanded" data-expanded-for="${esc(r.key)}"${open ? "" : " hidden"}><span class="muted small">Latest input</span><p class="session-row__input">${esc(r.snippet)}</p></div>`
    : "";
  return `<div class="session-row" data-key="${esc(r.key)}">
    <span class="session-badge session-badge--${r.cls}">${label(r.cls)}</span>
    <span class="session-row__name" title="${esc(r.sessionId || r.name)}">${esc(r.name)}</span>
    <span class="muted small session-row__detail">${esc(r.detail)}</span>
    <span class="muted small session-row__id">${esc(shortId(r.sessionId))}</span>
    <span class="session-row__actions">${pin}${action}${toggle}</span>
    ${detail}
  </div>`;
}

/** Render active identity plus waiting, held, focused, finished and parked sessions. */
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

  const seen = new Set();
  const waiting = [];
  const add = (entry, kind, detail, extra = {}) => {
    const id = idOf(entry);
    if (!id || seen.has(id)) return;
    seen.add(id);
    const isActive = kind === "queued" && !!activeId && id === activeId;
    const bundle = entry.root_session || entry.session_id || entry.bundle || "";
    waiting.push({
      key: extra.key || id,
      queueId: extra.queueId || entry.id || "",
      sessionId: id,
      bundle,
      pinned: pinStateOf(q.focused, id, bundle),
      name: entry.session_name || entry.snippet || shortId(id),
      cls: isActive ? statusOf("active", entry, live) : statusOf(kind, entry, live),
      detail,
      snippet: entry.snippet || "",
      action: extra.action || null,
    });
  };
  for (const w of q.waiting || []) {
    const paused = (q.paused_requests || []).includes(w.id);
    const isActive = !!activeId && w.session_id === activeId;
    add(w, "queued",
        paused ? "Paused · stays in the queue"
               : isActive ? `Active · ${fmt(w.prompt_tokens)} tokens · waited ${fmt(w.waited_s, 1)} s`
                          : `Queued · ${fmt(w.prompt_tokens)} tokens · waited ${fmt(w.waited_s, 1)} s`,
        {key: w.id, queueId: w.id, action: queueActionOf(w, q)});
  }
  if (q.hold) add(q.hold, "queued", `Held ${fmt(q.hold.remaining_s, 1)} s for a follow-up`);
  if (q.focused) add(q.focused, "queued", "Focused · waits without timeout");

  const finished = [];
  for (const r of ((m && m.requests) || []).filter(idOf)) {
    const id = idOf(r);
    if (seen.has(id)) continue;
    const bundle = r.root_session || r.session_id || "";
    finished.push({
      key: `${id}:${r.time || ""}`,
      queueId: "",
      sessionId: id,
      bundle,
      pinned: pinStateOf(q.focused, id, bundle),
      name: r.session_name || r.snippet || shortId(id),
      cls: statusOf("history", r, live),
      detail: `${r.finish === "error" ? "Error" : "Done"} · ${fmt(r.output_tokens)} tokens · ${fmt(r.duration_s, 1)} s`,
      snippet: r.snippet || "",
      action: null,
    });
    if (finished.length >= historyLimit()) break;
  }

  const parked = (m && m.conversation_cache && m.conversation_cache.parked) || 0;
  if (parked) waiting.push({key: "parked", queueId: "", sessionId: "parked",
                            bundle: "", pinned: false,
                            name: "Parked conversations", cls: "idle",
                            detail: `${fmt(parked)} parked · identities are not exposed by the engine`,
                            snippet: "", action: null});

  const sections = [];
  if (waiting.length) sections.push(`<div class="session-group-title">Active &amp; waiting</div>${waiting.map(row).join("")}`);
  sections.push(`<div class="session-group-title">Finished</div>${finished.map(row).join("")}`);
  list.innerHTML = sections.join("");
}

async function queueAction(action, id) {
  try {
    if (action === "pause") {
      const r = await fetch("api/queue/pause-request", {method: "POST", headers: headers(true),
                                                        body: JSON.stringify({id})});
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      toast("info", "Session paused", "It stays in the queue until you press Play.");
    } else {
      const r = await fetch("api/queue/unpause-request", {method: "POST", headers: headers(true),
                                                          body: JSON.stringify({id})});
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const view = await r.json();
      const order = [id, ...((view.waiting || []).map((w) => w.id).filter((x) => x && x !== id))];
      const moved = await fetch("api/queue/reorder", {method: "POST", headers: headers(true),
                                                      body: JSON.stringify({order})});
      if (!moved.ok) throw new Error(`HTTP ${moved.status}`);
      toast("success", "Session resumed", "It is next in the queue.");
    }
    const q = await fetch("api/queue", {headers: headers()}).then((r) => r.json());
    state.lastQueue = q;
    renderSessions(q, state.lastMetrics);
  } catch (e) {
    toast("error", "Not applied", e.message || String(e));
  }
}

async function pinAction(action, bundle, snippet) {
  try {
    if (action === "unpin") {
      const r = await fetch("api/queue/unfocus", {method: "POST", headers: headers(true), body: "{}"});
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      toast("info", "Session unpinned", "It no longer has priority.");
    } else {
      const r = await fetch("api/queue/focus", {method: "POST", headers: headers(true),
                                                body: JSON.stringify({session: bundle, snippet})});
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      toast("success", "Session pinned", "Its next request runs first.");
    }
    const q = await fetch("api/queue", {headers: headers()}).then((r) => r.json());
    state.lastQueue = q;
    renderSessions(q, state.lastMetrics);
  } catch (e) {
    toast("error", "Not applied", e.message || String(e));
  }
}

if (typeof document !== "undefined") {
  $("sessions-history-toggle")?.addEventListener("click", () => {
    showHistory = !showHistory;
    const btn = $("sessions-history-toggle");
    btn.setAttribute("aria-expanded", String(showHistory));
    btn.textContent = showHistory ? "Hide history" : "Show history";
    if (state.lastQueue) renderSessions(state.lastQueue, state.lastMetrics);
  });

  $("session-list")?.addEventListener("click", (e) => {
    const toggle = e.target.closest("[data-session-expand]");
    if (toggle) {
      const key = toggle.dataset.sessionExpand;
      if (expanded.has(key)) expanded.delete(key); else expanded.add(key);
      const open = expanded.has(key);
      toggle.setAttribute("aria-expanded", String(open));
      toggle.textContent = open ? "▴" : "▾";
      for (const el of $("session-list").querySelectorAll("[data-expanded-for]")) {
        if (el.dataset.expandedFor === key) el.hidden = !open;
      }
      return;
    }
    const pinBtn = e.target.closest("[data-session-pin]");
    if (pinBtn) {
      pinAction(pinBtn.dataset.sessionPin, pinBtn.dataset.sessionBundle, pinBtn.dataset.sessionSnippet);
      return;
    }
    const btn = e.target.closest("[data-session-action]");
    if (btn) queueAction(btn.dataset.sessionAction, btn.dataset.queueId);
  });
}
