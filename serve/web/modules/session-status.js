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

/** Waiting rows offer Pause; a user-paused row offers Play instead. */
export function queueActionOf(w = {}, q = {}) {
  if (!w.id) return null;
  return (q.paused_requests || []).includes(w.id) ? "play" : "pause";
}

/** A session row is pinned when the server's focus points at it or its root bundle. */
export function pinStateOf(focused, sessionId = "", bundle = "") {
  if (!focused || !sessionId || sessionId === "parked") return false;
  return focused.bundle === sessionId || focused.id === sessionId
      || (!!bundle && (focused.bundle === bundle || focused.id === bundle));
}
