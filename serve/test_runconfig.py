"""serve/test_runconfig.py - #564: the web page's Settings view of the run config (GET / POST /config), against the
mock engine: only the listed keys change, every other key of the file stays, a bad value changes nothing, and only
PolyStrata's own page may write it (JSON, no foreign Origin, the API key when one is set).

    python -m unittest serve.test_runconfig
"""
from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
import urllib.error
import urllib.request
from pathlib import Path

from serve import runconfig
from serve.frontend import ChatTemplate
from serve.server import ByteTokenizer, MockEngine, Service, serve

ROOT = Path(__file__).resolve().parents[1]
CFG = {"exe": "engine/strata.exe", "args": ["--pack", "data/packs/q2_0", "--kv", "int8", "--spec-min-p", "0.5"],
       "model_name": "qwen3.8-flash-next-q2_0", "port": 8080, "host": "0.0.0.0", "api_key": "",
       "sampling": {"temperature": 0.7, "presence_penalty": 1.5},
       "mcp_servers": {"files": {"command": "npx", "args": ["-y", "server-filesystem", "."]}},
       "cors_origins": ["http://localhost:3000"]}


class Apply(unittest.TestCase):
    def test_only_the_named_keys_change(self):
        new, changed = runconfig.apply(CFG, {"sampling.top_p": 0.95, "reasoning_budget_tokens": 4096,
                                             "vram_reserve_mib": 2048, "aliases": "qwen, local", "lazy_load": True})
        self.assertEqual(changed, ["sampling.top_p", "reasoning_budget_tokens", "vram_reserve_mib", "aliases",
                                   "lazy_load"])
        self.assertEqual(new["sampling"], {"temperature": 0.7, "presence_penalty": 1.5, "top_p": 0.95})
        self.assertEqual(new["args"][-2:], ["--vram-reserve-mib", "2048"])
        self.assertEqual(new["aliases"], ["qwen", "local"])
        for k in ("exe", "model_name", "port", "host", "api_key", "mcp_servers", "cors_origins"):
            self.assertEqual(new[k], CFG[k], k)
        self.assertEqual(CFG["sampling"], {"temperature": 0.7, "presence_penalty": 1.5})   # the input is left alone

    def test_null_is_the_default_again(self):
        cfg, _ = runconfig.apply(CFG, {"vram_reserve_mib": 2048, "fit_max_tokens": True})
        new, changed = runconfig.apply(cfg, {"vram_reserve_mib": None, "fit_max_tokens": None,
                                             "sampling.temperature": None, "idle_unload_s": None})
        self.assertEqual(changed, ["vram_reserve_mib", "fit_max_tokens", "sampling.temperature"])
        self.assertEqual(new["args"], CFG["args"])
        self.assertNotIn("fit_max_tokens", new)
        self.assertEqual(new["sampling"], {"presence_penalty": 1.5})
        new, _ = runconfig.apply({"sampling": {"top_k": 20}}, {"sampling.top_k": None})
        self.assertNotIn("sampling", new)                                 # an empty block goes

    def test_truncate_middle_is_an_opt_in_boolean_setting(self):
        cfg, changed = runconfig.apply(CFG, {"truncate_middle": True})
        self.assertTrue(cfg["truncate_middle"])
        self.assertEqual(changed, ["truncate_middle"])
        cfg, _ = runconfig.apply(cfg, {"truncate_middle": None})
        self.assertNotIn("truncate_middle", cfg)
        with self.assertRaisesRegex(ValueError, "truncate_middle"):
            runconfig.apply(CFG, {"truncate_middle": "yes"})

    def test_refused(self):
        for changes in ({"api_key": "x"}, {"host": "0.0.0.0"}, {"mcp_servers": {}}, {"before_load": "calc.exe"},
                        {"args": []}, {"exe": "evil.exe"}, {"trusted_origins": ["*"]}, {"allowed_hosts": ["*"]},
                        {"sampling.temperature": -1}, {"sampling.top_p": 0}, {"sampling.top_k": 65},
                        {"sampling.min_p": 2}, {"reasoning_budget_tokens": 1.5}, {"fit_max_tokens": "yes"},
                        {"effort_position": "middle"}, {"vram_reserve_mib": -5}, {"idle_unload_s": True},
                        {"aliases": [1]}, {}, None, ["x"]):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                runconfig.apply(CFG, changes)
        with self.assertRaises(ValueError):                              # one bad value: nothing is applied
            runconfig.apply(CFG, {"fit_max_tokens": True, "sampling.top_k": 0})
        with self.assertRaises(ValueError):
            runconfig.apply({**CFG, "vision": {"gpu": True}}, {"lazy_load": True})

    def test_view(self):
        v = runconfig.view({**CFG, "args": CFG["args"] + ["--vram-reserve-mib", "1500"]}, "x/strata-q2_0.json")
        self.assertEqual(v["file"], "strata-q2_0.json")
        got = {k["key"]: k for k in v["keys"]}
        self.assertEqual(got["sampling.temperature"]["value"], 0.7)
        self.assertEqual(got["vram_reserve_mib"]["value"], 1500)
        self.assertIsNone(got["fit_max_tokens"]["value"])
        self.assertIsNone(got["truncate_middle"]["value"])
        self.assertEqual(got["anthropic_thinking"]["choices"], ["model", "on_request"])
        self.assertNotIn("api_key", got)
        self.assertNotIn("mcp_servers", got)

    def test_kv_resident_toggle(self):
        # kv_resident=16384, kv="q4_0" adds --kv-resident 16384 --kv q4_0; null removes both
        cfg, changed = runconfig.apply(CFG, {"kv_resident": 16384, "kv": "q4_0"})
        self.assertIn("--kv-resident", cfg["args"])
        self.assertIn("16384", cfg["args"])
        self.assertIn("--kv", cfg["args"])
        self.assertIn("q4_0", cfg["args"])
        self.assertEqual(changed, ["kv_resident", "kv"])
        # toggle off: both null removes both flags cleanly
        cfg2, _ = runconfig.apply(cfg, {"kv_resident": None, "kv": None})
        self.assertNotIn("--kv-resident", cfg2["args"])
        self.assertNotIn("16384", cfg2["args"])
        self.assertNotIn("--kv", cfg2["args"])
        self.assertNotIn("q4_0", cfg2["args"])
        # no duplicates: original --kv int8 is also gone (was overwritten, now removed)
        self.assertNotIn("int8", cfg2["args"])

    def test_kv_string_must_be_one_of_the_five_values(self):
        for bad in ("banana", "q5_0", "f16", ""):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError, msg=f"kv={bad!r} should be rejected"):
                    runconfig.check("kv", bad, CFG)

    def test_non_kv_arg_rejects_strings(self):
        # context, kv_resident, vram_reserve_mib must be whole numbers; a string is not accepted
        for key in ("context", "kv_resident", "vram_reserve_mib"):
            with self.subTest(key=key):
                with self.assertRaises(ValueError, msg=f"{key} with string should be rejected"):
                    runconfig.check(key, "banana", CFG)

    def test_defaults_are_reported(self):
        # every SPEC key has a DEFAULTS entry
        self.assertEqual(set(runconfig.DEFAULTS), set(runconfig.SPEC))
        v = runconfig.view(CFG, "strata-q2_0.json")
        d = {k["key"]: k["default"] for k in v["keys"]}
        self.assertEqual(d["cache_disk_mib"], "auto")
        self.assertEqual(d["cache_mib"], "auto")
        self.assertEqual(d["cache_focus"], "12")
        self.assertEqual(d["cache_ttl_s"], "0 (never)")
        self.assertIsNone(d["aliases"])
        self.assertEqual(d["kv"], "fp16")
        # in the base CFG (cache keys unset) the matching value fields are None
        got = {k["key"]: k for k in v["keys"]}
        self.assertIsNone(got["cache_mib"]["value"])
        self.assertIsNone(got["cache_disk_mib"]["value"])
        self.assertIsNone(got["cache_slots"]["value"])
        self.assertIsNone(got["cache_focus"]["value"])
        self.assertIsNone(got["cache_ttl_s"]["value"])
        self.assertIsNone(got["cache_checkpoints"]["value"])

    def test_memory_policy_keys(self):
        # the five runtime knobs and the start-only checkpoint count land in args as whole numbers
        mem = {"cache_mib": 12288, "cache_disk_mib": 4096, "cache_slots": 6, "cache_focus": 12,
               "cache_ttl_s": 900, "cache_checkpoints": 10}
        cfg, changed = runconfig.apply(CFG, mem)
        self.assertEqual(changed, list(mem))
        args = cfg["args"]
        for flag, val in (("--conversation-cache-mib", 12288), ("--conversation-cache-disk-mib", 4096),
                          ("--conversation-cache-slots", 6), ("--conversation-cache-focus", 12),
                          ("--conversation-cache-ttl-s", 900), ("--prompt-cache", 10)):
            self.assertIn(flag, args)
            self.assertEqual(args[args.index(flag) + 1], str(val), flag)

    def test_disk_size_accepts_auto_but_other_sizes_do_not(self):
        cfg, changed = runconfig.apply(CFG, {"cache_mib": "auto", "cache_disk_mib": "auto"})
        self.assertEqual(changed, ["cache_mib", "cache_disk_mib"])
        self.assertEqual(runconfig.value_of(cfg, "cache_mib"), "auto")
        self.assertEqual(runconfig.value_of(cfg, "cache_disk_mib"), "auto")
        for key in ("cache_slots", "cache_focus", "cache_ttl_s", "cache_checkpoints"):
            with self.subTest(key=key), self.assertRaises(ValueError):
                runconfig.apply(CFG, {key: "auto"})
        for key in ("cache_mib", "cache_disk_mib"):
            with self.subTest(key=key), self.assertRaises(ValueError):
                runconfig.apply(CFG, {key: -1})


class Http(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "strata-q2_0.json"
        self.path.write_text(json.dumps(CFG, indent=1), encoding="utf-8")
        tok = ByteTokenizer()
        self.svc = Service(MockEngine(tok, "ok", max_context=4096), tok, ChatTemplate(ROOT / "serve/chat_template.jinja"))
        self.svc.config_path = str(self.path)
        self.httpd = serve(self.svc, port=0)
        self.host = f"127.0.0.1:{self.httpd.server_address[1]}"

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.dir.cleanup()

    def call(self, method, body=None, headers=None, path="/config"):
        h = {"Content-Type": "application/json", **(headers or {})}
        req = urllib.request.Request(f"http://{self.host}{path}", method=method, headers=h,
                                     data=None if body is None else (body if isinstance(body, bytes)
                                                                     else json.dumps(body).encode()))
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            with e:
                return e.code, json.loads(e.read())

    def saved(self):
        return json.loads(self.path.read_text(encoding="utf-8"))

    def test_read_and_write_from_the_page(self):
        status, v = self.call("GET")
        self.assertEqual(status, 200, v)
        self.assertEqual({k["key"]: k["value"] for k in v["keys"]}["sampling.temperature"], 0.7)
        with contextlib.redirect_stdout(io.StringIO()):
            status, v = self.call("POST", {"set": {"sampling.temperature": 1.0, "open_browser": False}},
                                  {"Origin": f"http://{self.host}"})
        self.assertEqual(status, 200, v)
        self.assertEqual(v["changed"], ["sampling.temperature", "open_browser"])
        cfg = self.saved()
        self.assertEqual(cfg["sampling"]["temperature"], 1.0)
        self.assertIs(cfg["open_browser"], False)
        for k in ("mcp_servers", "cors_origins", "host", "args", "exe"):
            self.assertEqual(cfg[k], CFG[k], k)
        self.assertEqual(json.loads((self.path.parent / "strata-q2_0.json.bak").read_text(encoding="utf-8")), CFG)
        status, v = self.call("POST", {"set": {"open_browser": False}})   # no Origin (a script on this PC): allowed
        self.assertEqual((status, v["changed"]), (200, []))

    def test_only_from_strata_s_own_page(self):
        before = self.path.read_bytes()
        status, _ = self.call("POST", {"set": {"fit_max_tokens": True}}, {"Origin": "https://evil.example"})
        self.assertEqual(status, 403)
        status, _ = self.call("POST", b'{"set": {"fit_max_tokens": true}}', {"Content-Type": "text/plain"})
        self.assertEqual(status, 415)
        for body in ({"set": {"api_key": ""}}, {"set": {"mcp_servers": {"x": {"command": "calc"}}}},
                     {"set": {"sampling.top_k": 0}}, {"nothing": 1}, b"not json"):
            status, b = self.call("POST", body, {"Origin": f"http://{self.host}"})
            self.assertEqual(status, 400, (body, b))
        self.assertEqual(self.path.read_bytes(), before)                 # nothing was written
        self.assertFalse((self.path.parent / "strata-q2_0.json.bak").exists())

    def test_the_api_key(self):
        self.svc.api_key = "secret"
        self.assertEqual(self.call("GET")[0], 401)
        self.assertEqual(self.call("POST", {"set": {"fit_max_tokens": True}})[0], 401)
        with contextlib.redirect_stdout(io.StringIO()):
            status, _ = self.call("POST", {"set": {"fit_max_tokens": True}}, {"Authorization": "Bearer secret"})
        self.assertEqual(status, 200)
        self.assertIs(self.saved()["fit_max_tokens"], True)

    def test_without_a_run_config(self):
        self.svc.config_path = None
        self.assertEqual(self.call("GET")[0], 404)
        self.assertEqual(self.call("POST", {"set": {"fit_max_tokens": True}})[0], 404)

    def test_memory_keys_reach_a_live_engine_now(self):
        # a live engine that understands CACHE gets the changed knobs at once; the file is saved either way
        calls = []

        class LiveEngine:
            batch = 1

            def alive(self):
                return True

            def cache_settings(self, values):
                calls.append(values)
                return {"park": 12288, "disk": "auto", "cap": 65536, "target": 6, "slots": 8,
                        "focus": 20, "ttl": 900, "checkpoints": 10}

        self.svc.engine = LiveEngine()
        with contextlib.redirect_stdout(io.StringIO()):
            status, v = self.call("POST", {"set": {"cache_slots": 8, "cache_focus": 20}},
                                  {"Origin": f"http://{self.host}"})
        self.assertEqual(status, 200, v)
        self.assertTrue(v["runtime"]["applied"], v)
        self.assertEqual(calls, [{"slots": 8, "focus": 20}])
        self.assertEqual(v["runtime"]["policy"]["slots"], 8)
        self.assertEqual(self.saved()["args"][-4:], ["--conversation-cache-slots", "8",
                                                      "--conversation-cache-focus", "20"])
        # the engine's policy line now feeds the Monitor's live readout
        self.assertEqual(self.svc.conv_log.state["settings"]["slots"], 8)

    def test_memory_keys_without_a_live_engine_just_save(self):
        # the MockEngine has no cache_settings: the keys are saved to the file, no runtime reply, no error
        with contextlib.redirect_stdout(io.StringIO()):
            status, v = self.call("POST", {"set": {"cache_mib": 8192}}, {"Origin": f"http://{self.host}"})
        self.assertEqual(status, 200, v)
        self.assertNotIn("runtime", v)
        self.assertIn("--conversation-cache-mib", self.saved()["args"])

    def test_runtime_settings_endpoint_applies_without_saving(self):
        # POST /v1/cache/settings is the panel's Apply-now: the knobs reach a live engine, the config file is
        # untouched (unlike POST /config), and only the five known keys are passed through
        calls = []

        class LiveEngine:
            batch = 1

            def alive(self):
                return True

            def cache_settings(self, values):
                calls.append(values)
                return {"park": 12288, "disk": "auto", "cap": 65536, "target": 6, "slots": 8,
                        "focus": 20, "ttl": 900, "checkpoints": 10}

        self.svc.engine = LiveEngine()
        before = self.saved()
        status, v = self.call("POST", {"mib": 4096, "slots": 8, "focus": 20, "junk": "ignored"},
                              {"Origin": f"http://{self.host}"}, path="/v1/cache/settings")
        self.assertEqual(status, 200, v)
        self.assertEqual(v["status"], "ok")
        self.assertEqual(calls, [{"mib": 4096, "slots": 8, "focus": 20}])
        self.assertEqual(self.saved(), before)   # nothing persisted: Apply is runtime only


if __name__ == "__main__":
    unittest.main()
