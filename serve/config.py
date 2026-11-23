"""serve/config.py - run-config reading, model settings and the small request-accounting helpers.

Split out of serve/server.py (the 7,900-line module) so the Service class can move into its own
file without a circular import: these helpers only touch the standard library and the config dict,
never the engine, the HTTP handlers or Service itself.  server.py imports them back by name, so
`from serve.server import run_config, start_model, ...` keeps working for the tests and the bench
scripts.
"""
from __future__ import annotations

import json
import os
import re
from pathlib import Path

# #606: a reply that repeats one token this many times in a row is ended there ("length"): a model in a loop, or a
# broken state that answers one token forever (an issue saw 36,689 tokens of "!"). The config's "repeat_stop_tokens"
# sets it; 0 turns it off.
REPEAT_STOP_TOKENS = 256
BEFORE_LOAD_TIMEOUT_S = 120.0  # #1997: how long the before_load command may run (the config's before_load_timeout_s)


# ------------------------------------------------------------------------------------------------ shared defaults
def clean_shared_defaults(d) -> dict:
    """The Chat settings other apps get (POST /settings): only known keys, each checked; ValueError names a bad one."""
    if d is None:
        return {}
    if not isinstance(d, dict):
        raise ValueError("defaults must be an object")
    out = {}
    for key, value in d.items():
        if value is None or value == "":
            continue
        number = isinstance(value, (int, float)) and not isinstance(value, bool)
        if key == "reasoning_effort":
            if value not in ("none", "low", "medium", "high"):
                raise ValueError("reasoning_effort: none, low, medium or high")
        elif key == "temperature":
            if not number or not 0 <= value <= 2:
                raise ValueError("temperature: 0..2")
        elif key == "top_p":
            if not number or not 0 < value <= 1:
                raise ValueError("top_p: 0 < top_p <= 1")
        elif key == "top_k":
            if not number or value != int(value) or not 1 <= value <= 64:
                raise ValueError("top_k: an integer 1..64")
            value = int(value)
        elif key in ("seed", "max_tokens"):
            if not number or value != int(value) or value <= 0:
                raise ValueError(f"{key}: a positive integer")
            value = int(value)
        elif key == "reasoning_budget_tokens":                 # #123: 0 means no budget, so 0 is allowed here
            if not number or value != int(value) or value < 0:
                raise ValueError("reasoning_budget_tokens: a whole number of tokens (0: no budget)")
            value = int(value)
        elif key == "experimental_speed_projection":
            if not isinstance(value, bool):
                raise ValueError("experimental_speed_projection: true or false")
        else:
            raise ValueError(f"unknown setting {key!r}")
        out[key] = float(value) if key in ("temperature", "top_p") else value
    return out


# ------------------------------------------------------------------------------------------------ run config
def split_joined_args(cfg: dict) -> None:
    """#2267: an engine option written as ONE element of the config's "args" - "--ple-io mmap" or "--ple-io=mmap" - reaches
    the engine as a single argument ("unknown argument: --ple-io mmap"; the usual form is two elements, "--ple-io",
    "mmap").  Recommend, never force: such an element is split into the flag and its value, and said at startup.  Only an
    element that starts with a flag name (--letters-digits-dashes) followed by a space or "=" is touched, and never one
    that sits in the value place of the flag before it."""
    args = cfg.get("args")
    if not isinstance(args, list):
        return
    out, changed = [], []
    for a in args:
        m = re.match(r"^(--[A-Za-z0-9][A-Za-z0-9-]*)(?:\s+|=)(\S.*)$", a) if isinstance(a, str) else None
        prev = out[-1] if out else None
        in_value_place = isinstance(prev, str) and re.match(r"^--[A-Za-z0-9][A-Za-z0-9-]*$", prev) is not None
        if m is None or in_value_place:
            out.append(a)
            continue
        out += [m.group(1), m.group(2).strip()]
        changed.append(a)
    if changed:
        cfg["args"] = out
        print("[strata] the config's args hold " + ", ".join(repr(c) for c in changed) + " as one element each: split into the "
              'flag and its value (write them as two elements, e.g. "--ple-io", "mmap")', flush=True)


def read_config(path) -> dict:
    """The run config's JSON.  A mistake in it (a missing comma between two args, a trailing comma) is said with its
    place in the file instead of a traceback (#2267: editing "args" by hand is where it happens)."""
    text = Path(path).read_text(encoding="utf-8-sig")   # Notepad adds a BOM
    try:
        return json.loads(text)
    except json.JSONDecodeError as e:
        raise SystemExit(f"[strata] {path} is not valid JSON: {e.msg} at line {e.lineno}, column {e.colno}. Check the commas "
                         'between the entries of "args" (each option and each value is its own quoted entry, e.g. '
                         '"--ple-io", "mmap",) and that no entry is followed by a comma at the end of a list.') from None


def lift_reasoning_effort_arg(cfg: dict) -> None:
    """#1641: "--reasoning-effort high" in a config's "args" is meant for the thinking level, which is not an engine
    option (the engine stopped at the unknown flag).  It is the config's "reasoning_effort" (none, low, medium or high:
    the level for requests that name none), so the flag is taken out of the engine's arguments and becomes that key
    unless the config already has one; said at startup."""
    args = cfg.get("args")
    if not isinstance(args, list):
        return
    out, value, i = [], None, 0
    while i < len(args):
        a = args[i]
        if a == "--reasoning-effort" and i + 1 < len(args):
            value, i = args[i + 1], i + 2
        elif isinstance(a, str) and a.startswith("--reasoning-effort="):
            value, i = a.partition("=")[2], i + 1
        else:
            out.append(a)
            i += 1
    if value is None:
        return
    cfg["args"] = out
    if cfg.get("reasoning_effort") is None:
        cfg["reasoning_effort"] = value
    print("[strata] '--reasoning-effort' in the config's args is not an engine option: taken out of them. The thinking "
          "level is the config's 'reasoning_effort' key (outside 'args'); it is used as " +
          f"{cfg['reasoning_effort']!r}", flush=True)


def run_config(path, overrides: dict | None = None) -> dict:
    """A run config as the server uses it, at the start and at every switch to it (#37): read_config(), an option
    written as one entry of "args" split (#2267), "--reasoning-effort" lifted out of them (#1641), and the command
    line's --gpu / --replicas (`overrides`) written in.  SystemExit for JSON that cannot be read."""
    cfg = read_config(path)
    split_joined_args(cfg)
    lift_reasoning_effort_arg(cfg)
    cfg.update(overrides or {})
    return cfg


# ------------------------------------------------------------------------------------------------ sampling
# #1819: llama.cpp clients call the repetition penalty and its window repeat_penalty / repeat_last_n; they are the
# same settings as repetition_penalty / penalty_last_n (the /props answer already reports them under those names)
SAMPLING_ALIASES = {"repeat_penalty": "repetition_penalty", "repeat_last_n": "penalty_last_n"}


def sampling_defaults_from_config(cfg: dict) -> dict:
    """The run config's optional `sampling` block: defaults for the sampling fields a request leaves out, so
    a plain client gets configured sampling instead of greedy.  Supported: temperature, top_p, top_k, min_p,
    presence_penalty, repetition_penalty (also written repeat_penalty), frequency_penalty, penalty_last_n (also
    repeat_last_n), seed.  The request's own fields
    always win - an explicit temperature=0 still means greedy, a field set to null falls back to the default.
    A bad value refuses to start the server (a typo'd config should not quietly change sampling); unknown keys
    are named at startup and ignored."""
    out = {}
    block = dict(cfg.get("sampling") or {})
    for alias, canon in SAMPLING_ALIASES.items():      # #1819: llama.cpp's names for the same two fields
        if block.get(alias) is not None:
            if block.get(canon) is not None:
                raise SystemExit(f"[strata] config sampling: both {alias!r} and {canon!r} are set; they are the same "
                                 f"setting, keep one")
            block[canon] = block.pop(alias)
    for key, value in block.items():
        if value is None:
            continue
        number = isinstance(value, (int, float)) and not isinstance(value, bool)
        if key == "temperature":
            if not number or value < 0:
                raise SystemExit(f"[strata] config sampling.temperature={value!r}: expected a number >= 0 (0 = greedy)")
            out[key] = float(value)
        elif key == "top_p":
            if not number or not 0 < value <= 1:
                raise SystemExit(f"[strata] config sampling.top_p={value!r}: expected 0 < top_p <= 1")
            out[key] = float(value)
        elif key == "min_p":
            if not number or not 0 <= value <= 1:
                raise SystemExit(f"[strata] config sampling.min_p={value!r}: expected 0 <= min_p <= 1")
            out[key] = float(value)
        elif key == "top_k":
            if not number or value != int(value) or not 1 <= value <= 64:
                raise SystemExit(f"[strata] config sampling.top_k={value!r}: the sampled path takes an integer 1..64")
            out[key] = int(value)
        elif key == "presence_penalty":
            if not number or value < 0:
                raise SystemExit(f"[strata] config sampling.presence_penalty={value!r}: expected a number >= 0")
            out[key] = float(value)
        elif key == "frequency_penalty":
            if not number or value < 0:
                raise SystemExit(f"[strata] config sampling.frequency_penalty={value!r}: expected a number >= 0")
            out[key] = float(value)
        elif key == "repetition_penalty":
            if not number or value <= 0:
                raise SystemExit(f"[strata] config sampling.repetition_penalty={value!r}: expected a number > 0 (1 = off)")
            out[key] = float(value)
        elif key == "penalty_last_n":
            if not number or value != int(value) or value < 0:
                raise SystemExit(f"[strata] config sampling.penalty_last_n={value!r}: expected a non-negative integer")
            out[key] = int(value)
        elif key == "seed":
            if not number or value != int(value) or value <= 0:
                raise SystemExit(f"[strata] config sampling.seed={value!r}: expected a positive integer")
            out[key] = int(value)
        elif key == "experimental_speed_projection":
            if not isinstance(value, bool):
                raise SystemExit(f"[strata] config sampling.experimental_speed_projection={value!r}: expected true or "
                                 "false (the default for requests that leave it out, when the engine has the vector)")
            out[key] = value
        else:
            print(f"[strata] config sampling.{key}={value!r}: unknown key, ignored", flush=True)
    return out


# the API's model name per model family (the GGUF architecture, "arch" in the config) when the config names none
FAMILY_MODEL_NAMES = {"qwen4exp": "qwen3.8-flash-next", "qwen35moe": "qwen3.6-35b-a3b"}


# The sampling each model card recommends ("Best Practices"), used when neither the request, the Chat settings nor the
# run config's `sampling` block says otherwise.  Greedy is only for a request or config that asks for temperature 0.
# min_p 0, repetition_penalty 1 and presence_penalty 0 are the engine's own "off", so they are not listed.
MODEL_CARD_SAMPLING = {
    # Qwen3.8-Flash-Next (and Gyro, the same base model)
    "qwen4exp": {"thinking": {"temperature": 1.0, "top_p": 0.95, "top_k": 20},
                 "instruct": {"temperature": 0.7, "top_p": 0.8, "top_k": 20, "presence_penalty": 1.5}},
    # Qwen3.6-35B-A3B: the general-task sets (the card's precise-coding set is temperature 0.6, presence_penalty 0)
    "qwen35moe": {"thinking": {"temperature": 1.0, "top_p": 0.95, "top_k": 20, "presence_penalty": 1.5},
                  "instruct": {"temperature": 0.7, "top_p": 0.8, "top_k": 20, "presence_penalty": 1.5}},
}


def model_card_sampling(arch, thinking) -> dict:
    """The model card's sampling for the family `arch` and the thinking state ({} for an unknown or unset family)."""
    sets = MODEL_CARD_SAMPLING.get(arch) if arch else None
    return dict(sets["thinking" if thinking else "instruct"]) if sets else {}


def default_model_name(cfg: dict) -> str:
    """The config's "model_name", else its family's name ("arch": qwen4exp, qwen35moe), else Flash-Next's."""
    return cfg.get("model_name") or FAMILY_MODEL_NAMES.get(cfg.get("arch") or "qwen4exp", "qwen3.8-flash-next")


# ------------------------------------------------------------------------------------------------ model settings
def alias_list(aliases) -> list[str]:
    """#297: the config's `aliases` - a list of names (or one comma-separated string), like llama-server's --alias -
    as a list of names.  ValueError for anything else."""
    if aliases is None:
        aliases = []
    if isinstance(aliases, str):
        aliases = aliases.split(",")
    if not isinstance(aliases, list) or not all(isinstance(x, str) for x in aliases):
        raise ValueError(f"aliases={aliases!r}: expected a list of model names")
    return [x.strip() for x in aliases if x.strip()]


def budget_tokens(value) -> int | None:
    """#123: a reasoning_budget_tokens value as the most tokens to think, or None for no budget (0 or less).
    ValueError (a 400) for anything that is not a whole number."""
    if isinstance(value, float) and value.is_integer():
        value = int(value)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"reasoning_budget_tokens={value!r}: expected a whole number of tokens (0: no budget)")
    return value if value > 0 else None


def model_settings(cfg: dict, overrides: dict | None = None) -> dict:
    """#37: what a model's own run config sets on the Service ({attribute: value}), read at the start and again at every
    switch to that model (Service.apply_settings).  `overrides` are the command line's --fit-max-tokens, --idle-unload,
    --min-free-vram-mib and --before-load, which win for every model.  ValueError names a bad value; nothing is set
    then, so a switch to a model with a bad config leaves the loaded one alone.  The keys of the server itself (host,
    api_key, allowed_hosts, cors_origins, ...) are main()'s: they come from the config the server started with."""
    over = {k: v for k, v in (overrides or {}).items() if v is not None}

    def flag(key):                                     # an opt-in that is true or false (absent: false)
        v = cfg.get(key, False)
        if not isinstance(v, bool):
            raise ValueError(f"\"{key}\" must be true or false, not {v!r}")
        return v

    def number(key, kind, words):
        try:
            return kind(cfg.get(key) or 0)
        except (TypeError, ValueError):
            raise ValueError(f"\"{key}\" must be {words}, not {cfg.get(key)!r}") from None

    effort = None
    if cfg.get("reasoning_effort") is not None:        # #1641: the thinking level of requests that name none
        effort = clean_shared_defaults({"reasoning_effort": cfg["reasoning_effort"]}).get("reasoning_effort")
    mode = str(cfg.get("anthropic_thinking") or "model")   # #278: "on_request" = only when the request asks
    if mode not in ("model", "on_request"):
        raise ValueError(f"anthropic_thinking must be \"model\" or \"on_request\", not {mode!r}")
    warm = cfg.get("warm_start", False)                # true, or the chat request whose first turn is the start
    if not isinstance(warm, (bool, str)) or warm == "":
        raise ValueError(f"\"warm_start\" must be true, false or the path of a chat request (a JSON file), not {warm!r}")
    rs = cfg.get("repeat_stop_tokens", REPEAT_STOP_TOKENS)   # #606: opt-out with 0
    if isinstance(rs, bool) or not isinstance(rs, int) or rs < 0:
        raise ValueError(f"\"repeat_stop_tokens\" must be a whole number >= 0 (0 = off), not {rs!r}")
    budget = cfg.get("reasoning_budget_tokens")        # #123: a default thinking budget for every request
    if budget is not None:
        budget_tokens(budget)
    loops = cfg.get("reasoning_loop_recovery", False)  # #728: false (default) | "stop" | "recover" (true)
    if loops is True:
        loops = "recover"
    if loops is not False and loops not in ("stop", "recover"):
        raise ValueError(f"\"reasoning_loop_recovery\" must be false, \"stop\" or \"recover\", not {loops!r}")
    try:
        timeout = float(cfg.get("before_load_timeout_s") or BEFORE_LOAD_TIMEOUT_S)
    except (TypeError, ValueError):
        timeout = BEFORE_LOAD_TIMEOUT_S
        print(f"[strata] \"before_load_timeout_s\" in the config is not a number; using {timeout:g} s", flush=True)
    return {
        "fit_max_tokens": bool(over.get("fit_max_tokens")) or cfg.get("fit_max_tokens") is True,
        "reasoning_close_retry": cfg.get("reasoning_close_retry") is True,   # #1053: opt-in, off by default
        "drop_oldest": cfg.get("drop_oldest") is True,                       # #17: opt-in, off by default
        "truncate_middle": cfg.get("truncate_middle") is True,               # opt-in, off by default
        "preserve_thinking": cfg.get("preserve_thinking") is True,           # #25: opt-in, off by default
        "config_effort": effort,
        "codex_thread_titles": cfg.get("codex_thread_titles") is True,       # #923: opt-in, off by default
        "codex_compaction_cache": cfg.get("codex_compaction_cache") is True,  # #924: opt-in, off by default
        "aliases": alias_list(cfg.get("aliases")),                           # #297: other names the model answers to
        "anthropic_think_unasked": mode == "model",
        "tool_call_recovery": flag("tool_call_recovery"),                    # opt-in: see OutputParser's recover
        "warm_start": warm,                                                  # opt-in: see Service.start_warm
        "repeat_stop_tokens": rs,
        "reasoning_budget_tokens": budget if budget is not None else 0,
        "reasoning_loop_recovery": loops,
        "idle_unload_s": over["idle_unload_s"] if "idle_unload_s" in over else
        number("idle_unload_s", float, "a number of seconds"),
        "min_free_vram_mib": over["min_free_vram_mib"] if "min_free_vram_mib" in over else
        number("min_free_vram_mib", int, "a whole number of MiB"),
        "before_load": over.get("before_load") or cfg.get("before_load") or None,
        "before_load_timeout_s": timeout,
        "before_load_ignore_failure": cfg.get("before_load_ignore_failure") is True,
        "before_load_strict": cfg.get("before_load_strict") is True,
        "memory_debug": bool(os.environ.get("STRATA_MEMORY_DEBUG")) or cfg.get("memory_debug") is True,
        # 0.1.45: the model card's sampling (MODEL_CARD_SAMPLING) for the Strata project's families; the models on
        # strata-poly have no "arch" and keep their own defaults
        "model_arch": None if poly_engine(cfg) else cfg.get("arch") or "qwen4exp",
        "answer_room": cfg.get("answer_room") is not False,                 # 0.1.45: see ANSWER_ROOM_MIN_CAP
    }


def poly_engine(cfg: dict) -> bool:
    """The run config starts strata-poly (the engine of the models PolyStrata adds), not the Strata project's."""
    return Path(str(cfg.get("exe") or "")).stem.lower() in ("strata-poly", "strata-glm")   # its name before 10-05


def engine_exe(cfg: dict) -> str:
    """The config's engine as an absolute path.  A relative "exe" is the config's cwd's: Windows' CreateProcess resolves
    "engine/strata.exe" against nothing it is told about (WinError 2)."""
    return cfg["exe"] if os.path.isabs(cfg["exe"]) else os.path.abspath(os.path.join(cfg.get("cwd") or ".", cfg["exe"]))


def load_tokenizer(tpath: Path):
    """The tokenizer of a model's tokenizer directory (tools/strata_tokenizer.py wrote it), or None when the
    directory has none."""
    if not (tpath / "vocab.json").exists():
        return None
    import strata_tokenizer as ST
    return ST.Tokenizer.from_dir(tpath)


# ------------------------------------------------------------------------------------------------ request accounting
def prompt_tokens_seen(prompt_tokens: int, last: dict) -> int:
    """#471: the prompt tokens a request got through - all of them, unless the engine's DONE line says a cancel stopped
    its prompt read part-way (then the reused ones plus those read).  /metrics' history and totals count these, so a
    cancelled read is neither recorded as the whole prompt nor given a rate from tokens it never read.  An engine
    before 0.1.36 does not say (no `prompt_read`): the whole prompt, as before."""
    read = last.get("prompt_read")
    if read is None or last.get("finish") != "cancel":
        return prompt_tokens
    return min(prompt_tokens, int(last.get("reused") or 0) + int(read))


def carry_leg(carry: dict, leg: dict) -> None:
    """#1869: a request that decoded in a batch slot and went back to the solo path ends with the solo path's DONE, which
    replaces the engine's figures: keep the slot leg's decode time, tokens and prompt figures to add to it."""
    carry["decode_ms"] = carry.get("decode_ms", 0.0) + float(leg.get("decode_ms") or 0.0)
    carry["generated"] = carry.get("generated", 0) + int(leg.get("generated") or 0)
    carry.setdefault("prompt_ms", float(leg.get("prompt_ms") or 0.0))
    if leg.get("prompt_read") is not None:
        carry.setdefault("prompt_read", int(leg["prompt_read"]))


def add_carry(carry: dict, last: dict) -> dict:
    """The request's totals: the earlier legs' decode time and tokens plus the final one's; the prompt figures of the leg
    that read the prompt (the solo continuation reads next to nothing)."""
    out = {**last, "decode_ms": float(last.get("decode_ms") or 0.0) + carry.get("decode_ms", 0.0),
           "generated": int(last.get("generated") or 0) + carry.get("generated", 0),
           "prompt_ms": max(float(last.get("prompt_ms") or 0.0), carry.get("prompt_ms", 0.0))}
    if "prompt_read" in carry:
        out["prompt_read"] = carry["prompt_read"]
    return out


def request_timings(prompt_tokens: int, generated: int, last: dict) -> dict | None:
    """One request's `timings` in llama.cpp's names (what its clients show as speed), from the engine's own clock
    (StrataEngine.last): prompt_n is what was read, cache_n what the conversation cache already held.  None when the
    engine keeps no clock (MockEngine)."""
    if last.get("prompt_ms") is None:
        return None
    cache_n = int(last.get("reused") or 0)
    if last.get("prompt_read") is not None and last.get("finish") != "cancel":
        # #1870: the engine says how many prompt tokens it read: the slot's own session may have held the rest
        # (not the conversation cache), so the rate is not made of tokens it never read
        cache_n = max(cache_n, max(0, prompt_tokens - int(last["prompt_read"])))
    prompt_n, prompt_ms, decode_ms = max(0, prompt_tokens - cache_n), float(last["prompt_ms"]), float(last.get("decode_ms") or 0)
    decoded = int(last.get("generated") or generated)          # the engine's count gives its rate, as /metrics does
    return {"cache_n": cache_n, "prompt_n": prompt_n, "prompt_ms": round(prompt_ms, 1),
            "prompt_per_token_ms": round(prompt_ms / prompt_n, 3) if prompt_n else None,
            "prompt_per_second": round(prompt_n / (prompt_ms / 1000), 1) if prompt_n and prompt_ms > 0 else None,
            "predicted_n": generated, "predicted_ms": round(decode_ms, 1),
            "predicted_per_token_ms": round(decode_ms / decoded, 3) if decoded else None,
            "predicted_per_second": round(decoded / (decode_ms / 1000), 1) if decoded and decode_ms > 0 else None,
            # the speculative drafts, as llama.cpp names them (from PR #83, @mikicvi): only when the engine reported them
            **({"draft_n": int(last["drafts_offered"]), "draft_n_accepted": int(last["drafts_accepted"])}
               if last.get("drafts_offered") is not None else {})}


def request_stats(segments: list[dict]) -> dict:
    """Keep input/cache accounting at the original API boundary and total native work across continuations."""
    if not segments:
        return {}
    result = dict(segments[-1])
    for key in ("reused", "prompt_read", "chunk"):
        if key in segments[0]:
            result[key] = segments[0][key]
        else:
            result.pop(key, None)
    for key in ("prompt_ms", "decode_ms", "generated", "drafts_offered", "drafts_accepted",
                "hits", "lookups", "offloaded", "ram_blobs", "file_blobs", "file_mb", "experts_streamed"):
        if any(segment.get(key) is not None for segment in segments):
            result[key] = sum(segment.get(key) or 0 for segment in segments)
    return result