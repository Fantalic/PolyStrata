import test from "node:test";
import assert from "node:assert/strict";
import {statusOf, idOf, shortId, queueActionOf, sessionActionOf, pinStateOf, activityOf} from "../web/modules/session-status.js";

test("statusOf maps the live state to a badge", () => {
  assert.equal(statusOf("active", {}, {state: "reading"}), "reading");
  assert.equal(statusOf("active", {}, {state: "generating"}), "generating");
  assert.equal(statusOf("active", {}, {state: "idle", queued: 2}), "queued");
  assert.equal(statusOf("active", {}, {state: "error"}), "error");
  assert.equal(statusOf("active", {}, {state: "idle"}), "idle");
});

test("statusOf maps queued and finished sessions", () => {
  assert.equal(statusOf("queued", {}, {}), "queued");
  assert.equal(statusOf("history", {finish: "error"}, {}), "error");
  assert.equal(statusOf("history", {finish: "stop"}, {}), "idle");
  assert.equal(statusOf("parked", {}, {}), "idle");
});

test("session ids prefer the client's header", () => {
  assert.equal(idOf({session_id: "s-1", id: "internal"}), "s-1");
  assert.equal(idOf({root_session: "r-1", id: "internal"}), "r-1");
  assert.equal(idOf({id: "internal"}), "internal");
  assert.equal(idOf(), "");
  assert.equal(shortId("abcdefghijk"), "abcdefgh");
  assert.equal(shortId(""), "session");
});

test("queueActionOf offers play only for a paused waiting request", () => {
  assert.equal(queueActionOf({}, {}), null);
  assert.equal(queueActionOf({id: "r1"}, {paused_requests: []}), "pause");
  assert.equal(queueActionOf({id: "r1"}, {paused_requests: ["r1"]}), "play");
});

test("pinStateOf matches the focused session or its root bundle", () => {
  assert.equal(pinStateOf(null, "ses-1"), false);
  assert.equal(pinStateOf({bundle: "ses-1", id: "ses-1"}, "ses-1"), true);
  assert.equal(pinStateOf({bundle: "root-7", id: "r1"}, "ses-1", "root-7"), true);
  assert.equal(pinStateOf({bundle: "other", id: "other"}, "ses-1"), false);
  assert.equal(pinStateOf({bundle: "parked", id: "parked"}, "parked"), false);
});

test("activityOf builds the active row's status and progress", () => {
  const reading = activityOf({state: "reading", prompt_read: 50, prompt_total: 200, queued: 0});
  assert.equal(reading.cls, "reading");
  assert.equal(reading.label, "Reading prompt");
  assert.equal(reading.pct, 25);
  assert.equal(reading.tone, "info");
  assert.match(reading.detail, /50 \/ 200 tokens/);
  const generating = activityOf({state: "generating", generated: 30, max_tokens: 100, phase: "thinking"});
  assert.equal(generating.label, "Thinking");
  assert.equal(generating.pct, 30);
  assert.equal(generating.tone, "");
  const idle = activityOf({state: "idle", queued: 0}, {output_tokens: 12});
  assert.equal(idle.cls, "idle");
  assert.equal(idle.label, "Waiting for a request");
  assert.match(idle.detail, /last: 12 tokens/);
  assert.equal(idle.pct, 0);
  assert.equal(activityOf({state: "idle", queued: 2}).cls, "queued");
});

test("queueActionOf also follows a paused session", () => {
  assert.equal(queueActionOf({id: "r1", session_id: "s1"}, {paused_sessions: ["s1"]}), "play");
  assert.equal(queueActionOf({id: "r1", root_session: "root-1"}, {paused_sessions: ["root-1"]}), "play");
  assert.equal(queueActionOf({id: "r1"}, {paused_sessions: ["other"]}), "pause");
  assert.equal(queueActionOf({}, {paused_sessions: ["s1"]}), null);
});

test("sessionActionOf follows the session-level pause without a queued id", () => {
  assert.equal(sessionActionOf("s1", "root-1", {paused_sessions: ["s1"]}), "play");
  assert.equal(sessionActionOf("s1", "root-1", {paused_sessions: ["root-1"]}), "play");
  assert.equal(sessionActionOf("s1", "root-1", {paused_sessions: []}), "pause");
  assert.equal(sessionActionOf("", "", {}), null);
  assert.equal(sessionActionOf("parked", "", {paused_sessions: ["parked"]}), null);
});
