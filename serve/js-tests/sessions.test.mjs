import test from "node:test";
import assert from "node:assert/strict";
import {statusOf, idOf, shortId, queueActionOf, pinStateOf} from "../web/modules/session-status.js";

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
