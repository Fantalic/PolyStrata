"""serve/affinity.py - opt-in admission affinity for the single-request turn (the solo path).

With parallel agent sessions the requests interleave A,B,A,B and every switch makes the engine
re-prefill the other conversation (measured: 21 s of re-reading against 1.8 s when a turn follows
its predecessor back-to-back).  This module reorders WHO takes the turn next; it never changes
what runs.  The rules, in order:

  1. A small prompt (<= small_toks tokens) always runs at once - the helper calls (titles,
     summaries) cost almost nothing even cold, so they never wait behind a big turn.
      A quick call never queues; what it does to an armed hold is STRATA_AFFINITY_HOLD_SMALL -
      pause (default: the countdown freezes while it runs and resumes after), reset (a fresh
      window after it), or run (it burns the window).
  2. A prompt that continues a recently served conversation runs before a foreign big one.
     "Same conversation" is the engine's own turn checkpoint: the prompt's tokens up to its last
     turn-start token (<|im_start|>, the engine's TURN_ID) - the next turn of a chat re-renders
     exactly that prefix, while another session differs before it.
  3. A foreign big request is held while the hold window is open: after a big turn with no
     continuation queued, the server waits hold_s for the same session's follow-up before it
     serves a foreign one.  hold_s is one knob for both the wait and the foreign request's max
     deferral (a hold expires, the waiting foreign request goes).
  4. Focus mode (set from the monitor page): one conversation is pinned; its continuations run
     first and the server waits for the follow-up without a timeout, until Skip lets one foreign
     request through or Unfocus clears it.

The HTTP endpoints live in serve/server.py (GET /api/queue and POST /api/queue/...); the monitor
page (serve/web/monitor.html) shows the waiting requests and the controls.
"""
from __future__ import annotations

import threading
import time
import uuid
from collections import deque

TURN_ID = 248045        # <|im_start|>: the engine's default --turn-token (its checkpoint sits at the last one)
HOLD_S_MAX = 600.0      # a hold longer than this is surely a typo
SMALL_TOKS_MAX = 1 << 17


def checkpoint(ids: list, turn_id: int = TURN_ID) -> list:
    """The conversation identity of a prompt: its tokens up to the last turn-start token (the
    next turn of the same chat re-renders exactly this; another session's prompt differs before
    it).  A prompt without the turn token is its own identity."""
    k = 0
    for i in range(len(ids) - 1, -1, -1):          # backwards: no reversed copy of a 35K list
        if ids[i] == turn_id:
            k = i
            break
    return ids[:k] if k else list(ids)


def _matches(prefix: list, ids: list) -> bool:
    """`ids` continues `prefix`: longer, and equal over the prefix (list slice compare, C speed)."""
    return len(ids) > len(prefix) and ids[:len(prefix)] == prefix


def _continues(prefix: list, bundle: str, e: "Entry") -> bool:
    """Does the waiting entry `e` continue the served conversation (`prefix`, `bundle`)?
    With a session bundle (the plugin's headers) the match is by the root session id - a
    sub-agent's first request shares its main session's bundle without sharing a token prefix.
    Without bundles, by the engine's token checkpoint."""
    if bundle and e.bundle:
        return bundle == e.bundle
    return _matches(prefix, e.ids)


def last_user_snippet(req: dict, chars: int = 100) -> str:
    """The last user message's text on one line, cut to `chars`: the queue view's column that says
    which session a call belongs to.  Works for OpenAI and Anthropic bodies (both use `messages`
    with roles); a body without a user turn names an empty snippet."""
    for m in reversed(req.get("messages") or []):
        if not isinstance(m, dict) or m.get("role") != "user":
            continue
        c = m.get("content")
        if isinstance(c, str):
            text = c
        elif isinstance(c, list):                    # content parts: the text ones joined
            text = " ".join(p.get("text") for p in c if isinstance(p, dict) and isinstance(p.get("text"), str))
        else:
            continue
        return " ".join(text.split())[:chars]        # collapsed whitespace, one line
    return ""


def first_user_snippet(req: dict, chars: int = 60) -> str:
    """The first user message's text on one line, cut to `chars`: a session's name for the queue
    view - a conversation's opening question is stable across its turns, where the last user
    message changes every turn."""
    for m in req.get("messages") or []:
        if not isinstance(m, dict) or m.get("role") != "user":
            continue
        c = m.get("content")
        if isinstance(c, str):
            text = c
        elif isinstance(c, list):                    # content parts: the text ones joined
            text = " ".join(p.get("text") for p in c if isinstance(p, dict) and isinstance(p.get("text"), str))
        else:
            continue
        return " ".join(text.split())[:chars]        # collapsed whitespace, one line
    return ""


class Entry:
    """One waiting request, as the admission sees it (also what GET /api/queue shows)."""
    __slots__ = ("id", "path", "snippet", "arrived", "arrived_mono", "ids", "plen", "deferred", "reason",
                 "session_name", "bundle", "session_id", "root_session", "agent")

    def __init__(self, id: str, ids: list, path: str, snippet: str, session_name: str = "",
                 session_id: str = "", root_session: str = "", agent: str = ""):
        self.id, self.ids, self.path, self.snippet = id, ids, path, snippet
        self.session_name = session_name
        # the session bundle (plugin headers): every request of one root session - the main agent's
        # session and its sub-agent spawns - shares the root id and counts as one conversation
        self.session_id, self.root_session, self.agent = session_id, root_session, agent
        self.bundle = root_session or session_id
        self.arrived, self.arrived_mono = time.time(), time.monotonic()
        self.plen = len(ids)
        self.deferred = False                       # was held back at least once (for the view)
        self.reason = ""                            # why it is waiting: hold / focus / affinity

    def small(self, small_toks: int) -> bool:
        return self.plen <= small_toks

    def public(self, affinity) -> dict:
        if affinity.hold and not _continues(affinity.hold["prefix"], affinity.hold["bundle"], self):
            state = "hold"
        elif affinity.focused and not _continues(affinity.focused["prefix"], affinity.focused["bundle"], self):
            state = "focus"
        elif self.deferred:
            state = "affinity"
        else:
            state = "queued"
        return {"id": self.id, "path": self.path, "snippet": self.snippet, "arrived": self.arrived,
                "session_name": self.session_name,
                "session_id": self.session_id, "root_session": self.root_session, "agent": self.agent,
                "prompt_tokens": self.plen, "small": self.small(affinity.settings["small_toks"]),
                "state": state, "waited_s": round(time.monotonic() - self.arrived_mono, 1)}


class Affinity:
    """The waiting requests, the recently served conversations, the hold and the focus."""

    def __init__(self, enabled: bool = False, hold_s: float = 20.0, small_toks: int = 4096, hold_small: str = "pause",
                 recent_n: int = 4, turn_id: int = TURN_ID):
        self.lock = threading.Lock()
        self.settings = {"enabled": bool(enabled), "hold_s": float(hold_s),
                         "small_toks": int(small_toks), "hold_small": str(hold_small).strip().lower(),
                         "recent_n": int(recent_n)}
        self.turn_id = turn_id
        self.waiting: dict[str, Entry] = {}         # insertion order = arrival order
        self.recent: deque = deque(maxlen=max(1, int(recent_n)))   # served conversations' checkpoints
        self.hold: dict | None = None               # {"until": mono, "prefix": [...], "at": wall}
        self.focused: dict | None = None            # {"prefix": [...], "id": ..., "snippet": ...}
        self.skip_once = False                      # Skip: let exactly one foreign request through
        self.counters = {"deferred": 0, "runs": 0, "holds": 0, "skips": 0}
        self.running = True                   # queue accepts requests (True) or paused (False)
        self.paused = False                   # True while paused (for symmetry with running)
        self.paused_requests: set[str] = set()  # individual requests paused by user

    def _hold_left(self, now=None) -> float:
        """Seconds left on the armed hold, with a pause frozen: while a quick unrelated request runs
        (paused_at set), the countdown stands still, so the busy time does not eat the window."""
        if not self.hold:
            return 0.0
        now = time.monotonic() if now is None else now
        left = self.hold["until"] - now
        if self.hold.get("paused_at") is not None:
            left += now - self.hold["paused_at"]
        return max(left, 0.0)

    # ---- registration -------------------------------------------------
    def register(self, id: str | None, ids: list, path: str = "", snippet: str = "", session_name: str = "",
                 session_id: str = "", root_session: str = "", agent: str = "") -> Entry | None:
        """A request starts waiting for the turn (None for a request without tokens: nothing to
        match, it runs as before)."""
        if not ids:
            return None
        entry = Entry(id or uuid.uuid4().hex[:12], ids, path or "", snippet or "", session_name=session_name,
                      session_id=session_id, root_session=root_session, agent=agent)
        with self.lock:
            self.waiting[entry.id] = entry
        return entry

    def unregister(self, id: str | None) -> None:
        if not id:
            return
        with self.lock:
            self.waiting.pop(id, None)

    # ---- admission ----------------------------------------------------
    def decide(self, entry: Entry) -> bool:
        """May `entry` take the turn now (the caller has the FIFO and gives it back on False)?"""
        s = self.settings
        if not s["enabled"]:
            return True
        if not self.running:                # queue paused: hold all big requests
            return False
        with self.lock:
            if entry.small(s["small_toks"]):        # rule 1: small always runs
                if (s["hold_small"] == "pause" and self.hold is not None
                        and self.hold.get("paused_at") is None
                        and self._hold_left() > 0
                        and not _continues(self.hold["prefix"], self.hold["bundle"], entry)):
                    self.hold["paused_at"] = time.monotonic()
                return True
            if self.skip_once:                      # rule 4's Skip: one foreign request passes
                self.skip_once = False
                self.counters["skips"] += 1
                return True
            if entry.id in self.paused_requests:      # user paused this request: it never runs
                return False
            if self.focused:                        # rule 4: focus waits without a timeout
                ok = _continues(self.focused["prefix"], self.focused["bundle"], entry)
            else:
                ok = any(_continues(p["prefix"], p["bundle"], entry)
                         for p in self.recent)      # rule 2: a continuation
                if not ok:
                    # another waiting request continues a recent conversation: it goes first
                    others = any(e.id != entry.id and any(_continues(p["prefix"], p["bundle"], e)
                                                          for p in self.recent)
                                 for e in self.waiting.values())
                    if others:
                        ok = False
                    elif self.hold:                 # rule 3: the hold window
                        ok = _continues(self.hold["prefix"], self.hold["bundle"], entry) \
                            or self._hold_left() <= 0
                    else:
                        ok = True                   # nothing defers it: it runs
            if ok:
                self.counters["runs"] += 1
            elif not entry.deferred:                # count a wait episode once, not every check
                entry.deferred = True
                entry.reason = ("focus" if self.focused else
                                "hold" if self.hold and self._hold_left() > 0 else
                                "affinity")
                self.counters["deferred"] += 1
            return ok

    def served(self, ids: list, entry: "Entry | None" = None) -> None:
        """The turn is over (the caller still holds the FIFO): remember the conversation, then arm
        the hold when it was a big turn or a continuation of the held conversation with no other
        continuation already waiting; a quick unrelated call never clears the window - the turn's
        end follows hold_small (pause, reset, or run).  `entry` is the served request's own
        registration: it is still in `waiting` (the release runs after this) and must not count
        as its own follower."""
        if not ids or not self.settings["enabled"]:
            return
        prefix = checkpoint(ids, self.turn_id)
        bundle = entry.bundle if entry is not None else ""
        with self.lock:
            self.recent.appendleft({"prefix": prefix, "bundle": bundle})
            big = len(ids) > self.settings["small_toks"]
            follower = any(_continues(prefix, bundle, e)
                           for e in self.waiting.values() if e is not entry)
            held = (self.hold is not None and entry is not None
                    and _continues(self.hold["prefix"], self.hold["bundle"], entry))
            # A turn re-arms the hold when it was big, or when it continues the conversation the armed hold
            # already protects (a quick follow-up of the protected session is activity: its window refreshes).
            # A quick call from an unrelated session never clears the window: what the turn's end does to the
            # countdown is hold_small - pause (frozen while it ran, the rest resumes), reset (a fresh window),
            # or run (it burned the time).  An expired hold clears here.
            if (big or held) and not follower and not self.focused:
                self.hold = {"until": time.monotonic() + self.settings["hold_s"], "prefix": prefix,
                             "bundle": bundle, "at": time.time(), "paused_at": None}
                self.counters["holds"] += 1
            elif self.hold is not None and self._hold_left() > 0:
                now = time.monotonic()
                mode = self.settings["hold_small"]
                if mode == "pause":
                    if self.hold.get("paused_at") is not None:
                        self.hold["until"] += now - self.hold["paused_at"]
                        self.hold["paused_at"] = None
                elif mode == "reset":
                    self.hold["until"] = now + self.settings["hold_s"]
                    self.hold["paused_at"] = None
                else:                                       # "run": the busy time burned the window
                    self.hold["paused_at"] = None
            else:
                self.hold = None                    # expired (or nothing armed): no gap to bridge

    # ---- the monitor page's controls ----------------------------------
    def focus(self, id: str) -> dict:
        with self.lock:
            entry = self.waiting.get(id)
            if entry is None:
                raise KeyError(id)
            self.focused = {"prefix": checkpoint(entry.ids, self.turn_id), "bundle": entry.bundle,
                            "id": entry.id, "snippet": entry.snippet}
            self.hold = None
            return {"focused": entry.id, "snippet": entry.snippet}

    def unfocus(self) -> None:
        with self.lock:
            self.focused = None

    def skip(self) -> None:
        with self.lock:
            self.skip_once = True

    def pause(self) -> None:
        """Pause the queue: no new big requests are admitted until play()."""
        with self.lock:
            self.running = False
            self.paused = True

    def play(self) -> None:
        """Resume the queue: re-evaluate waiting requests."""
        with self.lock:
            self.running = True
            self.paused = False

    def set_settings(self, enabled: bool = None, hold_s: float = None, small_toks: int = None, hold_small: str = None,
                     recent_n: int = None) -> dict:
        with self.lock:
            if enabled is not None:
                if not isinstance(enabled, bool):
                    raise ValueError("enabled: true or false")
                self.settings["enabled"] = enabled
            if hold_s is not None:
                if not isinstance(hold_s, (int, float)) or isinstance(hold_s, bool) \
                        or not 0 <= float(hold_s) <= HOLD_S_MAX:
                    raise ValueError(f"hold_s: seconds from 0 to {HOLD_S_MAX:.0f}")
                self.settings["hold_s"] = float(hold_s)
                if self.hold is not None:           # the running hold obeys the new value at once
                    self.hold["until"] = time.monotonic() + self.settings["hold_s"]
            if small_toks is not None:
                if not isinstance(small_toks, int) or isinstance(small_toks, bool) \
                        or not 0 <= small_toks <= SMALL_TOKS_MAX:
                    raise ValueError(f"small_toks: tokens from 0 to {SMALL_TOKS_MAX}")
                self.settings["small_toks"] = small_toks
            if hold_small is not None:
                if not isinstance(hold_small, str) or hold_small.strip().lower() not in ("pause", "reset", "run"):
                    raise ValueError('hold_small: "pause", "reset" or "run"')
                self.settings["hold_small"] = hold_small.strip().lower()
            if recent_n is not None:
                if not isinstance(recent_n, int) or isinstance(recent_n, bool) \
                        or not 1 <= recent_n <= 16:
                    raise ValueError("recent_n: 1 to 16")
                self.settings["recent_n"] = recent_n
                self.recent = deque(self.recent, maxlen=recent_n)
            return dict(self.settings)

    # ---- the queue view ------------------------------------------------
    def view(self) -> dict:
        with self.lock:
            waiting = [e.public(self) for e in self.waiting.values()]
            hold = None if self.hold is None else {
                "remaining_s": round(max(0.0, self.hold["until"] - time.monotonic()), 1),
                "snippet": "", "at": self.hold["at"]}
            focused = None if self.focused is None else dict(self.focused)
            return {"settings": dict(self.settings), "waiting": waiting, "hold": hold,
                    "focused": focused, "counters": dict(self.counters),
                    "paused": self.paused, "running": self.running,
                    "paused_requests": list(self.paused_requests)}

    # ---- pause individual requests ------------------------------------
    def pause_request(self, id: str) -> None:
        """Pause a specific request: it will not take the turn until unpause_request is called."""
        with self.lock:
            if id in self.waiting:
                self.paused_requests.add(id)

    def unpause_request(self, id: str) -> None:
        """Unpause a specific request: it can now take the turn like normal."""
        with self.lock:
            self.paused_requests.discard(id)

    # ---- manual reordering --------------------------------------------
    def reorder(self, order: list[str]) -> None:
        """Reorder the waiting list by ID. IDs not in the current waiting set are ignored.
        The dict preserves insertion order, so rebuilding it with the given ID order
        reorders the queue."""
        with self.lock:
            if not all(id in self.waiting for id in order):
                return
            self.waiting = {id: self.waiting[id] for id in order}
