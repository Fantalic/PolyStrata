import { fmt } from "./format.js";

// Pure helpers for the Sessions panel: no DOM, so they can be unit-tested on their own.

export const STATUS_LABELS = {
  idle: "Idle", reading: "Reading", generating: "Generating", queued: "Queued", error: "Error",
};

/** Map a session source to one of the five Monitor badges. */
export function statusOf(kind, item = {}, live = {}) {
  if (kind === "active") {
    if (live.state === "error") return "error";
    if (live.state === "reading") return "reading";
    if (live.state === "generating") return "generating";
    if (live.queued > 0) return "queued";
    return "idle";
  }
  if (kind === "queued") return "queued";
  if (kind === "history") return item.finish === "error" ? "error" : "idle";
  return "idle";
}

/** Sessions are identified by the client's header; the queue's id is the fallback. */
export function idOf(entry = {}) {
  return entry.session_id || entry.root_session || entry.id || "";
}

export function shortId(id = "") {
  return id ? String(id).slice(0, 8) : "session";
}

/** Waiting rows offer Pause; a paused request or conversation offers Play instead. */
export function queueActionOf(w = {}, q = {}) {
  const id = w.id || "";
  const session = w.root_session || w.session_id || w.bundle || "";
  if (!id && !session) return null;
  const paused = (!!id && (q.paused_requests || []).includes(id))
    || (!!session && (q.paused_sessions || []).includes(session));
  return paused ? "play" : "pause";
}

/** The Active row has no queued id: its Pause/Play follows the session-level pause alone. */
export function sessionActionOf(sessionId = "", bundle = "", q = {}) {
  if (!sessionId || sessionId === "parked") return null;
  const paused = (q.paused_sessions || []).includes(sessionId)
    || (!!bundle && (q.paused_sessions || []).includes(bundle));
  return paused ? "play" : "pause";
}

/** A session row is pinned when the server's focus points at it or its root bundle. */
export function pinStateOf(focused, sessionId = "", bundle = "") {
  if (!focused || !sessionId || sessionId === "parked") return false;
  return focused.bundle === sessionId || focused.id === sessionId
      || (!!bundle && (focused.bundle === bundle || focused.id === bundle));
}

/** The active session's status line and progress bar, computed from the latest /metrics state. */
export function activityOf(live = {}, last = null) {
  const on = live.queued > 0 ? "queued" : live.state;
  const cls = live.state === "error" ? "error" : live.state === "reading" ? "reading"
    : live.state === "generating" ? "generating" : on === "queued" ? "queued" : "idle";
  let label = "Waiting for a request", detail = "", pct = 0, tone = "";
  if (live.state === "reading") {
    label = "Reading prompt";
    tone = "info";
    if (live.prompt_total) {
      pct = (100 * live.prompt_read) / live.prompt_total;
      detail = `${fmt(live.prompt_read)} / ${fmt(live.prompt_total)} tokens · ${fmt(pct)}%`;
    } else {
      detail = `${fmt(live.prompt_tokens)} tokens`;
    }
  } else if (live.state === "generating") {
    label = live.phase ? live.phase[0].toUpperCase() + live.phase.slice(1) : "Generating";
    pct = live.max_tokens ? Math.min(100, (100 * live.generated) / live.max_tokens) : 0;
    detail = `${fmt(live.generated)} tokens · ${fmt(live.tok_s, 1)} tok/s`;
  } else if (last) {
    detail = `last: ${fmt(last.output_tokens)} tokens${last.decode_tok_s ? ` at ${fmt(last.decode_tok_s, 1)} tok/s` : ""}`;
  }
  const width = Number.isFinite(pct) ? Math.max(0, Math.min(100, pct)) : 0;
  return {cls, label, detail, pct: width, tone};
}
