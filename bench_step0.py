#!/usr/bin/env python3
"""bench_step0.py — cold prefill / decode benchmark harness.

Generates exact-token-count prompts, drives `strata serve` over HTTP,
and prints a summary table of prefill and decode throughput.

This is a benchmark harness only; it does not touch the engine source.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import re
import sys
import time
import urllib.error
import urllib.request

# --------------------------------------------------------------------------- helpers
# The PolyStrata repo root is two levels up from this script's directory
# (inkling-core-resident/../../PolyStrata), then the tools/ sub-dir.
# Both the root and tools/ must be on sys.path:
#   - PolyStrata root  → `import serve.frontend` finds the serve package
#   - tools/            → `import strata_tokenizer` finds the module
_POLYSTRATA_ROOT = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "PolyStrata",
)
_POLYSTRATA_TOOLS = os.path.join(_POLYSTRATA_ROOT, "tools")
if _POLYSTRATA_ROOT not in sys.path:
    sys.path.insert(0, _POLYSTRATA_ROOT)
if _POLYSTRATA_TOOLS not in sys.path:
    sys.path.insert(0, _POLYSTRATA_TOOLS)

from strata_tokenizer import Tokenizer  # noqa: E402
from serve.frontend import ChatTemplate  # noqa: E402

# --------------------------------------------------------------------------- log regex
# Copied verbatim from serve/server.py ENGINE_REQUEST (lines 432-437).
ENGINE_REQUEST = re.compile(
    r"prompt (?P<prompt>\d+) tokens = (?P<reused>\d+) reused "
    r"\+ \d+(?: of \d+)? read in (?P<read>[\d.]+) ms "
    r"\((?P<pp>[\d.]+) tok/s\), "
    r"(?P<gen>\d+) generated in (?P<gen_ms>[\d.]+) ms "
    r"\((?P<tg>[\d.]+) tok/s\)"
    r"(?:, drafts accepted (?P<drafts_accepted>\d+) of (?P<drafts_offered>\d+))?"
)

# --------------------------------------------------------------------------- filler
# Synthetic Python code, reusing the community benchmark's style (benchmark.py
# line 96). Each line is a compact function definition that varies with i.
def _make_filler():
    """Return a deterministic ~12 000-line synthetic Python module."""
    return "\n".join(
        f"def task_{i:05d}(value: int) -> int: return (value * {(i % 97) + 1} + {i}) % 100003"
        for i in range(12000)
    )

# --------------------------------------------------------------------------- helpers
def _repo_root():
    """Directory that contains this script's parent (PolyStrata-wt/)."""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _build_request(config, model_name, messages, max_new):
    """Build the JSON body for POST /v1/chat/completions."""
    sampling = config.get("sampling", {})
    body = {
        "model": model_name,
        "messages": messages,
        "max_tokens": max_new,
        "temperature": sampling.get("temperature", 1.0),
        "top_k": sampling.get("top_k", 50),
        "top_p": sampling.get("top_p", 1.0),
        "stream": True,
        "stream_options": {"include_usage": True},
    }
    return body


def _count(tokenizer, template, messages, tools=None, **kwargs):
    """Offline token count: render the chat template and encode it.

    This mirrors the community benchmark's count() at benchmark.py line 48-50,
    so the offline count matches what the server's encode_prompt() will produce.
    """
    rendered = template.render(messages, tools, **kwargs)
    return len(tokenizer.encode(rendered, parse_special=True))


# --------------------------------------------------------------------------- main logic
def main():
    parser = argparse.ArgumentParser(
        description="Cold prefill / decode benchmark harness"
    )
    parser.add_argument(
        "--config", required=True,
        help="Config JSON filename or absolute path (resolved against repo root if relative)"
    )
    parser.add_argument(
        "--targets", default="745,32000,64000,128000",
        help="Comma-separated target token counts (default: 745,32000,64000,128000)"
    )
    parser.add_argument(
        "--max-new", type=int, default=16,
        help="Maximum new tokens to generate per request (default: 16)"
    )
    parser.add_argument(
        "--port", type=int, default=8095,
        help="Server port (default: 8095)"
    )
    parser.add_argument(
        "--tag", default="",
        help="Unique nonce to avoid server conversation cache hits"
    )
    args = parser.parse_args()

    # ---- resolve config path ------------------------------------------------
    root = _repo_root()
    config_path = os.path.join(root, args.config) if not os.path.isabs(args.config) else args.config
    if not os.path.isfile(config_path):
        print(f"ERROR: config file not found: {config_path}", file=sys.stderr)
        sys.exit(1)

    config = json.load(open(config_path, encoding="utf-8"))
    model_name = config["model_name"]
    log_path = config["log"]
    tokenizer_dir = config["tokenizer"]
    port = args.port
    max_new = args.max_new
    tag = args.tag
    targets = [int(t) for t in args.targets.split(",")]

    # ---- verify prerequisites -------------------------------------------------
    # Check server is reachable (quick probe).
    try:
        probe = urllib.request.Request(
            f"http://127.0.0.1:{port}/v1/models",
            method="GET",
        )
        urllib.request.urlopen(probe, timeout=10)
    except urllib.error.URLError as exc:
        print(f"ERROR: server unreachable at http://127.0.0.1:{port}: {exc.reason}", file=sys.stderr)
        sys.exit(1)

    if not os.path.isfile(log_path):
        print(f"ERROR: log file does not exist: {log_path}", file=sys.stderr)
        sys.exit(1)

    # ---- load tokenizer and template ------------------------------------------
    # ChatTemplate needs the Jinja template file; it lives at serve/chat_template.jinja
    # relative to the PolyStrata repo root.
    chat_template_path = os.path.join(root, "..", "PolyStrata", "serve", "chat_template.jinja")
    tokenizer = Tokenizer.from_dir(tokenizer_dir)
    template = ChatTemplate(chat_template_path)

    # ---- build filler text ----------------------------------------------------
    filler = _make_filler()
    ending = (
        "\n\nWrite a detailed explanation of the code above. Discuss deterministic "
        "integer transforms, modulo arithmetic, testing, naming, complexity, and "
        "maintainability. Write at least 600 words."
    )

    # ---- run binary search for each target ------------------------------------
    # Per-target unique prefix containing --tag and the target and a random-ish
    # run id so conversation caching never reuses tokens.
    run_id = int(hashlib.sha256(os.urandom(8)).hexdigest()[:8], 16)

    summary_rows = []

    for target in targets:
        prefix = f"Tag {tag} target {target} run {run_id}.\nReview this synthetic Python module:\n"

        # Binary search the filler length so the rendered prompt lands within
        # +/-1% of the target token count.
        lo, hi = 0, len(filler)
        while lo < hi:
            middle = (lo + hi + 1) // 2
            prompt_text = prefix + filler[:middle] + ending
            messages = [{"role": "user", "content": prompt_text}]
            if _count(tokenizer, template, messages) <= target:
                lo = middle
            else:
                hi = middle - 1

        prompt_text = prefix + filler[:lo] + ending
        messages = [{"role": "user", "content": prompt_text}]
        actual = _count(tokenizer, template, messages)

        if actual < target * 0.99 or actual > target:
            print(
                f"WARNING: target {target} got {actual} tokens ({'+' if actual > target else '-'}"
                f"{actual - target}), continuing anyway",
                file=sys.stderr,
            )

        # ---- record pre-request log byte offset ---------------------------------
        log_byte_offset = os.path.getsize(log_path)

        # ---- send HTTP request --------------------------------------------------
        body = _build_request(config, model_name, messages, max_new)
        body_bytes = json.dumps(body).encode("utf-8")

        url = f"http://127.0.0.1:{port}/v1/chat/completions"
        wire = urllib.request.Request(
            url,
            data=body_bytes,
            headers={"Content-Type": "application/json"},
            method="POST",
        )

        print(
            f"[{os.path.basename(config_path)}] target={target} "
            f"prompt_tokens={actual} sending ...",
            flush=True,
        )

        start = time.perf_counter()
        usage = {}
        try:
            with urllib.request.urlopen(wire, timeout=3600) as response:
                # Drain SSE stream; capture usage from the last chunk that has it.
                for line in response:
                    if not line.startswith(b"data: "):
                        continue
                    payload = line[6:].strip()
                    if payload == b"[DONE]":
                        break
                    chunk = json.loads(payload)
                    if "usage" in chunk:
                        usage = chunk["usage"]
        except urllib.error.URLError as exc:
            print(f"ERROR: HTTP request failed: {exc.reason}", file=sys.stderr)
            sys.exit(1)

        elapsed = time.perf_counter() - start

        # ---- read newly appended log lines and print ----------------------------
        with open(log_path, "r", encoding="utf-8") as f:
            f.seek(log_byte_offset)
            for line in f:
                m = ENGINE_REQUEST.search(line)
                if m:
                    print(f"[{os.path.basename(config_path)}] target={target} {line.strip()}")
                    # Parse into summary row
                    summary_rows.append({
                        "target": target,
                        "prompt_tokens": int(m["prompt"]),
                        "reused": int(m["reused"]),
                        "generated": int(m["gen"]),
                        "prefill_tok_s": float(m["pp"]),
                        "decode_tok_s": float(m["tg"]),
                        "prefill_ms": float(m["read"]),
                        "decode_ms": float(m["gen_ms"]),
                    })

        # Also print the HTTP response's usage field if present
        if usage:
            print(
                f"[{os.path.basename(config_path)}] target={target} "
                f"usage: {json.dumps(usage)}",
                flush=True,
            )

    # ---- print summary table --------------------------------------------------
    if summary_rows:
        print()
        print("=" * 90)
        print(f"{'Target':>10}  {'Prompt Toks':>12}  {'Prefill Tok/s':>15}  "
              f"{'Decode Tok/s':>14}  {'Prefill ms':>12}  {'Decode ms':>11}")
        print("-" * 90)
        for row in summary_rows:
            print(
                f"{row['target']:>10}  {row['prompt_tokens']:>12}  "
                f"{row['prefill_tok_s']:>15.1f}  {row['decode_tok_s']:>14.1f}  "
                f"{row['prefill_ms']:>12.1f}  {row['decode_ms']:>11.1f}"
            )
        print("=" * 90)
    else:
        print("No log lines matching the engine request pattern were found.")


if __name__ == "__main__":
    main()
