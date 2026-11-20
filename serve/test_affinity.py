"""The affinity admission (serve/affinity.py): the unit tests of the ordering rules, plus the
Service-level behavior a waiting request sees (keep-alives while held, cancel while held, a small
request that jumps the hold).

    python -m unittest serve.test_affinity -v
"""
import threading
import time
import unittest
from unittest import mock

import serve.server as server
from serve.affinity import Affinity, checkpoint, last_user_snippet, TURN_ID
from serve.frontend import ChatTemplate
from serve.server import ByteTokenizer, MockEngine, Service
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def conv(first: int, *more: int) -> list:
    """A prompt of one conversation: `first` tokens, a turn start, then `more`."""
    return [first] + ([TURN_ID] if more else []) + list(more)


class AffinityRules(unittest.TestCase):
    def affinity(self, **kw) -> Affinity:
        a = Affinity(**kw)
        a.set_settings(enabled=True)
        return a

    def test_disabled_every_request_runs(self):
        a = Affinity(hold_s=0.05)
        big = a.register("b", conv(1, 2))
        self.assertTrue(a.decide(big))

    def test_small_request_never_waits(self):
        a = self.affinity(small_toks=100)
        a.served(conv(1, 2))                       # arms the hold (a big turn, nobody follows)
        small = a.register("s", conv(9))           # 1 token < 100: a helper call
        self.assertTrue(a.decide(small))

    def test_continuation_runs_before_a_foreign_request(self):
        a = self.affinity(small_toks=0)
        a.served(conv(1, 2))
        next_turn = a.register("a2", conv(1, 2, 3))
        foreign = a.register("b1", conv(7, 8))
        self.assertTrue(a.decide(next_turn))
        self.assertFalse(a.decide(foreign))
        self.assertEqual(a.view()["waiting"][1]["state"], "hold")

    def test_a_waiting_continuation_holds_the_foreign_request_even_without_a_hold(self):
        a = self.affinity(small_toks=0, hold_s=0.0)
        a.served(conv(1, 2))
        follower = a.register("a2", conv(1, 2, 3))
        foreign = a.register("b1", conv(7, 8))
        a.decide(follower)                          # the continuation takes the turn
        self.assertFalse(a.decide(foreign))         # the follower's twin (a registration race) first
        a.unregister(follower.id)
        self.assertTrue(a.decide(foreign))          # nobody follows the conversation any more

    def test_the_hold_expires_and_the_foreign_request_goes(self):
        a = self.affinity(small_toks=0, hold_s=0.05)
        a.served(conv(1, 2))
        foreign = a.register("b1", conv(7, 8))
        self.assertFalse(a.decide(foreign))
        time.sleep(0.08)
        self.assertTrue(a.decide(foreign))
        self.assertEqual(a.counters["deferred"], 1)  # counted once for the whole wait

    def test_focus_waits_without_timeout_until_skip(self):
        a = self.affinity(small_toks=0)
        a1 = a.register("a1", conv(1, 2))
        a.focus(a1.id)
        a.served(conv(1, 2))
        a2 = a.register("a2", conv(1, 2, 3))
        foreign = a.register("b1", conv(7, 8))
        self.assertTrue(a.decide(a2))               # the focused conversation goes first
        self.assertFalse(a.decide(foreign))         # and a foreign request waits, with no timeout
        time.sleep(0.05)                            # (longer than any hold would be in this test)
        self.assertFalse(a.decide(foreign))
        a.skip()                                    # the monitor page's Skip: exactly one passes
        self.assertTrue(a.decide(foreign))
        foreign2 = a.register("b2", conv(9, 9))
        self.assertFalse(a.decide(foreign2))        # focus still holds the rest
        a.unregister(a2.id)                         # a1, a2 and foreign1 never took the turn in
        a.unregister(foreign.id)                    # this unit test: take them out of the way
        a.unregister(a1.id)
        a.unfocus()
        self.assertTrue(a.decide(foreign2))

    def test_focus_survives_a_growing_conversation(self):
        a = self.affinity(small_toks=0)
        a1 = a.register("a1", conv(1, 2))
        a.focus(a1.id)
        grown = a.register("a2", conv(1, 2, 3, 4, TURN_ID, 5))
        self.assertTrue(a.decide(grown))            # the turn checkpoint is a prefix of the next turn

    # ---- session bundles (the plugin's headers) ------------------------
    def test_a_subagent_request_continues_its_main_session_bundle(self):
        """A sub-agent's first request shares the root session, not a token prefix: it counts as the
        main conversation's continuation even though its prompt starts fresh."""
        a = self.affinity(small_toks=0)
        main = a.register("m1", conv(1, 2), session_id="ses_main", root_session="ses_main")
        a.served(conv(1, 2), main)                  # the main session's turn ends, the hold arms
        sub = a.register("s1", conv(70, 71, 72), session_id="ses_sub1", root_session="ses_main")
        self.assertTrue(a.decide(sub))              # its bundle continues the served one
        foreign = a.register("b1", conv(80, 81), session_id="ses_other", root_session="ses_other")
        self.assertFalse(a.decide(foreign))         # a different root waits out the hold

    def test_a_bundle_beats_the_token_checkpoint(self):
        """With roots on both sides the bundle decides - even when the tokens would match: a
        different root must not ride in on a shared prompt prefix."""
        a = self.affinity(small_toks=0)
        a1 = a.register("a1", conv(1, 2), session_id="ses_a", root_session="ses_a")
        a.served(conv(1, 2), a1)
        stranger = a.register("s1", conv(1, 2, 3), session_id="ses_b", root_session="ses_b")
        self.assertFalse(a.decide(stranger))        # same tokens, different session: waits

    def test_an_untagged_served_conversation_still_matches_by_checkpoint(self):
        """No headers (an older plugin, no plugin): the token checkpoint is the only identity - the
        mixed case (tagged waiting request, untagged served conversation) falls back to tokens."""
        a = self.affinity(small_toks=0)
        a.served(conv(1, 2))                        # no entry: bundle ""
        next_turn = a.register("a2", conv(1, 2, 3), session_id="ses_x", root_session="ses_x")
        self.assertTrue(a.decide(next_turn))        # the tokens still match

    def test_the_view_carries_the_session_fields(self):
        a = self.affinity(small_toks=0)
        e = a.register("m1", conv(1, 2), session_id="ses_main", root_session="ses_main", agent="architect")
        v = a.view()["waiting"][0]
        self.assertEqual(v["session_id"], "ses_main")
        self.assertEqual(v["root_session"], "ses_main")
        self.assertEqual(v["agent"], "architect")

    def test_a_small_request_jumps_in_during_a_focus(self):
        a = self.affinity(small_toks=100)
        a1 = a.register("a1", conv(1, 2))
        a.focus(a1.id)
        helper = a.register("h", conv(5))           # 1 token: small, runs at once
        self.assertTrue(a.decide(helper))

    def test_the_recent_conversations_are_bounded(self):
        a = self.affinity(small_toks=0, recent_n=2)
        for c in (1, 2, 3):
            a.served(conv(c))
        self.assertEqual(len(a.recent), 2)
        self.assertEqual([p["prefix"][0] for p in a.recent], [3, 2])   # newest first

    def test_settings_validation(self):
        a = self.affinity()
        with self.assertRaises(ValueError):
            a.set_settings(hold_s=100000)
        with self.assertRaises(ValueError):
            a.set_settings(enabled="yes")
        with self.assertRaises(ValueError):
            a.set_settings(small_toks=-1)
        a.set_settings(hold_s=30, enabled=False, small_toks=8192)
        self.assertEqual(a.settings, {"enabled": False, "hold_s": 30.0, "small_toks": 8192, "hold_small": "pause", "recent_n": 4})

    def test_checkpoint_is_the_last_turn_start(self):
        ids = conv(1, 2, 3)
        self.assertEqual(checkpoint(ids), [1])         # the tokens before the last turn start
        self.assertEqual(checkpoint([5, 6]), [5, 6])   # no turn token: the whole prompt

    def test_last_user_snippet(self):
        req = {"messages": [{"role": "user", "content": "  hello\nworld  "}]}
        self.assertEqual(last_user_snippet(req), "hello world")
        parts = {"messages": [{"role": "user", "content": [{"type": "text", "text": "hi"},
                                                           {"type": "image_url", "url": "x"}]}]}
        self.assertEqual(last_user_snippet(parts), "hi")


class AffinityOnService(unittest.TestCase):
    """A request the admission holds keeps its keep-alives, can be cancelled, and a small one runs."""
    HOLD_S = 5.0

    def setUp(self):
        tok = ByteTokenizer()
        self.svc = Service(MockEngine(tok, "ok"), tok, ChatTemplate(ROOT / "serve/chat_template.jinja"))
        self.svc.affinity.set_settings(enabled=True, small_toks=0, hold_s=self.HOLD_S)
        self.svc.affinity.served(self.svc.tok.encode("warm"))   # a big turn just served: hold armed
        p1 = mock.patch.object(server, "QUEUE_BEAT_S", 0.1)
        p2 = mock.patch.object(server, "AFFINITY_RECHECK_S", 0.05)
        for p in (p1, p2):
            p.start()
            self.addCleanup(p.stop)

    def test_a_held_request_pings_then_runs_when_the_hold_goes(self):
        foreign = self.svc.run(self.svc.tok.encode("hi"), False, None, 8, {}, threading.Event())
        first = next(foreign)
        self.assertEqual(first, ("ping", None))     # held: no generation yet
        self.assertEqual(self.svc.affinity.view()["waiting"][0]["state"], "hold")
        self.svc.affinity.set_settings(hold_s=0.0)  # the hold is over
        events, done = [], None
        for _ in range(40):                         # the answer ran once the admission let it
            events.append(next(foreign))
            if events[-1][0] == "done":
                done = True
                break
        self.assertTrue(done, events[-4:])
        self.assertEqual(self.svc.affinity.view()["waiting"], [])

    def test_cancelled_while_held_never_takes_the_turn(self):
        cancel = threading.Event()
        foreign = self.svc.run(self.svc.tok.encode("hi"), False, None, 8, {}, cancel)
        self.assertEqual(next(foreign), ("ping", None))
        cancel.set()
        rest = list(foreign)
        self.assertEqual([k for k, _ in rest], ["done"])
        self.assertEqual(rest[0][1]["finish"], "cancel")   # it ended queued: no generation happened
        self.assertEqual(self.svc.affinity.view()["waiting"], [])

    def test_a_small_request_runs_straight_through_the_hold(self):
        self.svc.affinity.set_settings(small_toks=1000)
        small = self.svc.run(self.svc.tok.encode("hi"), False, None, 8, {}, threading.Event())
        events, done = [], None
        for _ in range(40):
            events.append(next(small))
            if events[-1][0] == "done":
                done = True
                break
        self.assertTrue(done, events[-4:])          # no ping first: the hold did not stop it
        self.assertEqual(self.svc.affinity.view()["waiting"], [])


class TestViewFreeze(unittest.TestCase):
    """view() and resume_freeze() — the freeze must not drain the countdown, and the
    resume must restore it.  These are pure-Affinity tests (no Service, no engine)."""

    def test_view_remaining_s_includes_the_frozen_pause(self):
        """While a hold is frozen (paused_at set), view().hold.remaining_s should still
        show the full window (~5 s), not the drained raw until-now (~4.7 after 0.3 s)."""
        aff = Affinity(hold_s=5.0, small_toks=0)
        aff.hold = {"until": time.monotonic() + 5.0, "prefix": [1], "bundle": "",
                     "at": time.time(), "paused_at": None}
        aff.hold["paused_at"] = time.monotonic()   # freeze it
        time.sleep(0.3)
        raw_remaining = max(0.0, aff.hold["until"] - time.monotonic())
        view_remaining = aff.view()["hold"]["remaining_s"]
        # raw is ~4.7, view should still be ~5.0 (frozen countdown)
        self.assertGreater(view_remaining, raw_remaining + 0.1)
        self.assertAlmostEqual(view_remaining, 5.0, delta=1.0)

    def test_resume_freeze_resumes_the_countdown(self):
        """After resume_freeze(), the paused_at is cleared and remaining_s starts
        draining again.  A short until (0.5 s) should reach 0 within ~1 s."""
        aff = Affinity(hold_s=5.0, small_toks=0)
        aff.hold = {"until": time.monotonic() + 0.5, "prefix": [1], "bundle": "",
                     "at": time.time(), "paused_at": None}
        aff.hold["paused_at"] = time.monotonic()   # freeze
        aff.resume_freeze()
        self.assertIsNone(aff.hold["paused_at"])
        time.sleep(1.0)                           # past the 0.5 s window
        remaining = aff.view()["hold"]["remaining_s"]
        self.assertEqual(remaining, 0.0)


if __name__ == "__main__":
    unittest.main()
