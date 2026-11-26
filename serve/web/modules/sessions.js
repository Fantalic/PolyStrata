// The Sessions panel: a pinned Active row, Waiting, and Finished behind Show history.
import { $, esc, fmt, headers, toast, state, icon } from "./runtime.js";
import { statusOf, idOf, shortId, queueActionOf, sessionActionOf, pinStateOf, activityOf } from "./session-status.js";

let showHistory = false;
const expanded = new Set();                       // row keys whose latest input is open
const rows = new Map();                           // row key -> row data, for the detail overlay
const historyLimit = 25;

function label(cls) {
  return {idle: "Idle", reading: "Reading", generating: "Generating", queued: "Queued", error: "Error"}[cls] || "Idle";
}

/** The loading bar's own row: status text and progress, with no second badge. */
function activity(a) {
  return `<div class="session-row__activity">
    <strong>${esc(a.label)}</strong>
    <span class="muted small">${esc(a.detail)}</span>
    <div class="st-progress"${a.tone ? ` data-tone="${a.tone}"` : ""}><div class="st-progress__bar" style="width:${a.pct}%"></div></div>
  </div>`;
}

function row(r) {
  const open = expanded.has(r.key);
  const pinTitle = r.pinned ? "Unpin this session" : "Pin this session's next request";
  const pin = r.bundle
    ? `<button class="st-btn st-btn--icon session-row__btn session-row__pin" data-session-pin="${r.pinned ? "unpin" : "pin"}" data-session-key="${esc(r.key)}" type="button" aria-pressed="${r.pinned}" title="${pinTitle}" aria-label="${pinTitle}">${icon(r.pinned ? "lock-fill" : "lock")}</button>`
    : "";
  const actionTitle = r.action === "pause" ? "Pause this session" : "Play this session";
  const action = r.action
    ? `<button class="st-btn st-btn--icon session-row__btn session-row__action" data-session-action="${r.action}" data-session-key="${esc(r.key)}" type="button" title="${actionTitle}" aria-label="${actionTitle}">${icon(r.action === "pause" ? "pause" : "play")}</button>`
    : "";
  const toggle = r.input
    ? `<button class="st-btn session-row__btn session-row__toggle" data-session-expand="${esc(r.key)}" type="button" aria-expanded="${open}" aria-label="Show latest input">${open ? "▴" : "▾"}</button>`
    : "";
  const detail = r.input
    ? `<div class="session-row__expanded" data-expanded-for="${esc(r.key)}"${open ? "" : " hidden"}><span class="muted small">Latest input</span><p class="session-row__input">${esc(r.input)}</p><button class="st-btn session-row__btn session-row__more" data-session-detail="${esc(r.key)}" type="button" title="Show the full input and all session facts">...</button></div>`
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

/** Every fact we have for a row, for the full-input overlay. */
function factsOf(r) {
  const e = r.raw || {};
  const pairs = [
    ["Session id", r.sessionId],
    ["Root session", e.root_session || r.bundle],
    ["Session name", e.session_name || r.name],
    ["Agent", e.agent],
    ["Status", label(r.cls)],
    ["Prompt tokens", e.prompt_tokens],
    ["Output tokens", e.output_tokens],
    ["Waited", e.waited_s != null ? `${fmt(e.waited_s, 1)} s` : ""],
    ["Endpoint", e.path],
    ["Finish", e.finish],
    ["Duration", e.duration_s != null ? `${fmt(e.duration_s, 1)} s` : ""],
    ["Arrived", e.arrived ? new Date(e.arrived * 1000).toLocaleString() : ""],
    ["Pinned", r.pinned ? "yes" : "no"],
    ["Input length", r.input ? `${r.input.length} characters` : ""],
  ];
  return pairs.filter(([, v]) => v != null && v !== "");
}

function openDetail(r) {
  const ov = $("session-detail-overlay");
  if (!ov) return;
  $("sd-input").textContent = r.input || "(no user input was recorded)";
  $("sd-facts").innerHTML = factsOf(r).map(([k, v]) =>
    `<div><dt>${esc(k)}</dt><dd>${esc(v)}</dd></div>`).join("");
  ov.hidden = false;
  $("sd-close")?.focus();
}

function closeDetail() {
  const ov = $("session-detail-overlay");
  if (ov) ov.hidden = true;
}

/** Render the pinned Active row plus Waiting, and Finished only when history is open. */
export function renderSessions(q, m) {
  const activeBox = $("session-active");
  const list = $("session-list");
  if (!list || !q) return;
  rows.clear();
  const live = (m && m.live) || {};
  const requests = (m && m.requests) || [];
  const activeId = live.session_id || "";
  const a = activityOf(live, requests[0] || null);
  const isPaused = (id, bundle) =>
    (q.paused_sessions || []).includes(id) || (!!bundle && (q.paused_sessions || []).includes(bundle));

  // The Active session appears only in the pinned area, never also in Waiting.
  let active = null;
  if (activeId) {
    const recent = requests.find((r) => idOf(r) === activeId) || {};
    const queued = (q.waiting || []).find((w) => idOf(w) === activeId);
    const base = queued || live;
    const bundle = base.root_session || base.session_id || base.bundle || "";
    const paused = isPaused(activeId, bundle);
    active = {
      key: `active:${activeId}`,
      queueId: queued ? queued.id : "",
      sessionId: activeId,
      bundle,
      pinned: pinStateOf(q.focused, activeId, bundle),
      name: live.session_name || base.session_name || shortId(activeId),
      cls: statusOf("active", live, live),
      detail: paused ? "Paused · waits until Play" : a.detail,
      input: live.last_input || live.snippet || base.last_input || base.snippet
             || recent.last_input || recent.snippet || "",
      snippet: live.snippet || base.snippet || recent.snippet || "",
      action: sessionActionOf(activeId, bundle, q),
      raw: queued || recent,
    };
    rows.set(active.key, active);
  }

  const waiting = [], finished = [];
  const seen = new Set(activeId ? [activeId] : []);
  const add = (entry, detail, extra = {}) => {
    const id = idOf(entry);
    if (!id || seen.has(id)) return;
    seen.add(id);
    const bundle = entry.root_session || entry.session_id || entry.bundle || "";
    const r = {
      key: extra.key || id,
      queueId: extra.queueId || entry.id || "",
      sessionId: id,
      bundle,
      pinned: pinStateOf(q.focused, id, bundle),
      name: entry.session_name || entry.snippet || shortId(id),
      cls: statusOf("queued", entry, live),
      detail,
      input: entry.last_input || entry.snippet || "",
      snippet: entry.snippet || "",
      action: extra.action || queueActionOf(entry, q),
      raw: entry,
    };
    rows.set(r.key, r);
    waiting.push(r);
  };

  for (const w of q.waiting || []) {
    const paused = (q.paused_requests || []).includes(w.id) || isPaused(idOf(w), w.root_session || w.session_id || "");
    add(w, paused ? "Paused · waits until Play"
                  : `Queued · ${fmt(w.prompt_tokens)} tokens · waited ${fmt(w.waited_s, 1)} s`);
  }
  if (q.hold) add(q.hold, `Held ${fmt(q.hold.remaining_s, 1)} s for a follow-up`, {action: null});
  if (q.focused) add(q.focused, "Focused · waits without timeout", {action: null});

  if (showHistory) {
    for (const r of requests.filter(idOf)) {
      const id = idOf(r);
      if (seen.has(id)) continue;
      seen.add(id);
      const bundle = r.root_session || r.session_id || "";
      const rowData = {
        key: `${id}:${r.time || ""}`,
        queueId: "",
        sessionId: id,
        bundle,
        pinned: pinStateOf(q.focused, id, bundle),
        name: r.session_name || r.snippet || shortId(id),
        cls: statusOf("history", r, live),
        detail: `${r.finish === "error" ? "Error" : "Done"} · ${fmt(r.output_tokens)} tokens · ${fmt(r.duration_s, 1)} s`,
        input: r.last_input || r.snippet || "",
        snippet: r.snippet || "",
        action: null,
        raw: r,
      };
      rows.set(rowData.key, rowData);
      finished.push(rowData);
      if (finished.length >= historyLimit) break;
    }
  }

  const parked = (m && m.conversation_cache && m.conversation_cache.parked) || 0;
  if (parked) {
    const r = {key: "parked", queueId: "", sessionId: "parked", bundle: "", pinned: false,
               name: "Parked conversations", cls: "idle",
               detail: `${fmt(parked)} parked · identities are not exposed by the engine`,
               input: "", snippet: "", action: null, raw: {}};
    rows.set(r.key, r);
    waiting.push(r);
  }

  if (activeBox) activeBox.innerHTML = active ? row(active) + activity(a) : "";
  const sections = [];
  if (waiting.length) sections.push(`<div class="session-group-title">Waiting</div>${waiting.map(row).join("")}`);
  if (showHistory && finished.length) sections.push(`<div class="session-group-title">Finished</div>${finished.map(row).join("")}`);
  list.innerHTML = sections.join("");
}

async function queueAction(action, key) {
  const r = rows.get(key) || {};
  const payload = {};
  if (r.bundle || r.sessionId) payload.session = r.bundle || r.sessionId;
  if (r.queueId) payload.id = r.queueId;
  try {
    if (action === "pause") {
      const res = await fetch("api/queue/pause-request", {method: "POST", headers: headers(true),
                                                          body: JSON.stringify(payload)});
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      toast("info", "Session paused", "Its next request waits until you press Play.");
    } else {
      const res = await fetch("api/queue/unpause-request", {method: "POST", headers: headers(true),
                                                            body: JSON.stringify(payload)});
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const view = await res.json();
      if (r.queueId) {
        const order = [r.queueId, ...((view.waiting || []).map((w) => w.id).filter((x) => x && x !== r.queueId))];
        const moved = await fetch("api/queue/reorder", {method: "POST", headers: headers(true),
                                                        body: JSON.stringify({order})});
        if (!moved.ok) throw new Error(`HTTP ${moved.status}`);
      }
      toast("success", "Session resumed", "It is picked up at the next chance.");
    }
    const q = await fetch("api/queue", {headers: headers()}).then((res) => res.json());
    state.lastQueue = q;
    renderSessions(q, state.lastMetrics);
  } catch (e) {
    toast("error", "Not applied", e.message || String(e));
  }
}

async function pinAction(action, bundle, snippet, lastInput = "") {
  try {
    if (action === "unpin") {
      const r = await fetch("api/queue/unfocus", {method: "POST", headers: headers(true), body: "{}"});
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      toast("info", "Session unpinned", "It no longer has priority.");
    } else {
      const r = await fetch("api/queue/focus", {method: "POST", headers: headers(true),
                                                body: JSON.stringify({session: bundle, snippet,
                                                                      last_input: lastInput})});
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

  // One listener for both containers: the Active row is outside the scrollable list.
  $("queue-card")?.addEventListener("click", (e) => {
    const toggle = e.target.closest("[data-session-expand]");
    if (toggle) {
      const key = toggle.dataset.sessionExpand;
      if (expanded.has(key)) expanded.delete(key); else expanded.add(key);
      const open = expanded.has(key);
      toggle.setAttribute("aria-expanded", String(open));
      toggle.textContent = open ? "▴" : "▾";
      for (const el of $("queue-card").querySelectorAll("[data-expanded-for]")) {
        if (el.dataset.expandedFor === key) el.hidden = !open;
      }
      return;
    }
    const detailBtn = e.target.closest("[data-session-detail]");
    if (detailBtn) {
      const r = rows.get(detailBtn.dataset.sessionDetail);
      if (r) openDetail(r);
      return;
    }
    const pinBtn = e.target.closest("[data-session-pin]");
    if (pinBtn) {
      const r = rows.get(pinBtn.dataset.sessionKey);
      pinAction(pinBtn.dataset.sessionPin, (r && r.bundle) || "",
                (r && r.snippet) || "", (r && r.input) || "");
      return;
    }
    const btn = e.target.closest("[data-session-action]");
    if (btn) queueAction(btn.dataset.sessionAction, btn.dataset.sessionKey);
  });

  $("session-detail-overlay")?.addEventListener("click", (e) => {
    if (e.target.closest("[data-session-detail-close]")) closeDetail();
  });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") closeDetail(); });
}
