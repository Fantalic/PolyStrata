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
        self.assertEqual([list(p)[0] for p in a.recent], [3, 2])   # newest first

    def test_settings_validation(self):
        a = self.affinity()
        with self.assertRaises(ValueError):
            a.set_settings(hold_s=100000)
        with self.assertRaises(ValueError):
            a.set_settings(enabled="yes")
        with self.assertRaises(ValueError):
            a.set_settings(small_toks=-1)
        a.set_settings(hold_s=30, enabled=False, small_toks=8192)
        self.assertEqual(a.settings, {"enabled": False, "hold_s": 30.0, "small_toks": 8192, "recent_n": 4})

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


if __name__ == "__main__":
    unittest.main()
