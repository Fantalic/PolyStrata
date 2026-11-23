# Inkling 2 Mini — prefill measurements and tuning

Inkling 2 Mini is a 3.6 B MoE model from the `inkling2` family, supported by the
`strata-poly` engine. Three quant variants were found on disk during investigation:

| Variant | Size | GGUF type | Architecture | Notes |
| --- | --- | --- | --- | --- |
| IQ1_S | ~7.2 GiB | UD-IQ1_S | `inkling2` | Hidden from the UI until the BACKENDS fix |
| Q2_K_XL | ~8.5 GiB | UD-Q2_K_XL | `inkling2` | Hidden from the UI until the BACKENDS fix |
| IQ4_NL | ~16.4 GiB | UD-IQ4_NL | `inkling2_loop` | Requires the `inkling2_loop` engine key |

The first two were hidden from the UI because their architecture keys were
missing from `serve/models.py` BACKENDS. The `inkling2_loop` variant needs a
separate key. All three run on `strata-poly` without engine code changes.

## Core geometry

- **24 physical layers**, `loop.start=6`, `loop.end=18`, `loop.count=3` →
  **48 visits per token**. Visits 0-5 and 42-47 are one-pass I/O layers
  (12 visits, 25% of the work). Physical layers 6-17 are applied 3× as visits
  6-41 (**36 visits**, 75% of compute) with **shared weights**
  (`inkling.cpp:120-142`).
- Each visit has its **own attention cache and convolution state** — the three
  core passes share weights only, not KV (`inkling.hpp:13-16`). This blocks
  cross-pass KV reuse.
- **3072 routed experts**, 128 per physical layer, ~2 MiB each in IQ1_S. The
  1536 core experts are ~3.1 GiB. Expert placement is per-expert, per-call,
  inside `Engine::moe_rows()` (GPU slot / CPU host / staged PCIe).
- **Attention: 43 of 48 visits are full attention** (`inkling.cpp:456`:
  `window = B.swa ? window_ : 0`). Only 5 visits use sliding-window rings.
  Long prefill therefore carries an O(n²) attention term that expert residency
  cannot fix.
- **KV cache:** Inkling accepts only fp16 and q8_0 (`inkling.cpp:188-189`).
  `--kv q4_0` parses but is rejected at load. q8_0 row = 2176 B/position/visit
  (the load log prints `1088` — half the real value, `inkling.cpp:321-322`).
  At 48 visits: 128K KV ≈ 13.7 GiB (q8_0), 25.6 GiB (fp16). Most of the
  backing store streams from host RAM, not VRAM.

## Hardware

Measurements on a Windows PC with:

- **GPU 1:** RTX 3060 Ti, sm_86, 8 GiB (runs the engine).
- **GPU 0:** GTX 1050 Ti, sm_61 (desktop only; cannot run the engine — build
  targets sm_86). No 2-GPU split is possible.
- **RAM:** 32 GB system memory. It holds the KV cache at 64K/128K — the 128K KV
  is ~13.7 GiB (q8_0), exceeding the 8 GiB VRAM.

Windows Task Manager shows 0% GPU for CUDA work on this system (no Compute/CUDA
engine counter exposed — only 3D/Copy/Video). Use `nvidia-smi` for GPU evidence.

## Step-0 prefill measurements

All measurements use the `strata-poly` engine and the IQ1_S model with the
`strata-inkling2-core-*.json` configs from the experiment worktree. The benchmark
script (`bench_step0.py`) binary-searches a prompt to an exact token count,
POSTs to `/v1/chat/completions`, and reads the server log line
`prompt N tokens = R reused + C read in X ms (Y tok/s), M generated in Z ms
(W tok/s)`. Y = prefill tok/s, W = decode tok/s.

### Baseline (`--prompt-batch 256`, no `--stage-ahead`)

| Prompt tokens | Prefill tok/s | Decode tok/s (16 new) | Expert-cache hit |
| --- | --- | --- | --- |
| 745 | ~390 | 46.6 | 92.0% |
| 31,929 | 242 (~132 s) | 46.4 | 95.7% |
| 63,929 | 159 (~402 s) | 38.9 | 97.8% |
| 127,929 | ~120 (~749 s) | — | 96.8% |

The 128K cold prefill at ~120 tok/s completes without OOM but was aborted by
timeout at ~90K tokens after 749 s (~18 min for full 128K). No decode was
measured because prefill did not finish.

### With `--stage-ahead 256`

| Prompt tokens | Prefill tok/s | Decode tok/s (16 new) |
| --- | --- | --- |
| 745 | 418.7 | 44.1 |
| 31,929 | 242.5 | 39.6 |
| 63,929 | 158.5 | 37.4 |

Stage-ahead 256 gives ~28 tok/s at 745 (noise level) but **no measurable
improvement at 32K or 64K**. The compact-miss path works but is not the
bottleneck at long context. The expert cache hit rate rises with context
(92% → 98%), isolating the attention/KV pass as the dominant cost.

### Prompt-batch sweep — the step-0 win

Measured 2026-11-23 on GPU 1, cold prompts, `--stage-ahead 256`, core expert
profile. Prefill tokens/s (higher is better):

| Prompt tokens | `--prompt-batch 256` | `--prompt-batch 512` | `--prompt-batch 1024` |
| --- | --- | --- | --- |
| 745 | ~390 | 397.6 | 437.5 |
| 31,929 | 242 | 260.8 | 277.9 |
| 63,929 | 159 | 166.7 | 176.0 |
| 127,929 | ~120 (aborted at ~90K / 749 s) | — | 101.5 (~21 min, no OOM) |

`--prompt-batch 1024` is the winner: **+15% at 32K** and +11% at 64K over the
pb256 baseline, and it completed a 128K cold prefill without OOM. A 16-token
decode sample after that 128K prefill measured 16.1 tok/s. `--prompt-batch 512`
was an intermediate point. This is why the surviving IQ1_S UI config uses pb1024.

## Best settings

The recommended config for running Inkling 2 Mini at long context:

```
--prompt-batch 1024
--stage-ahead 256
--stage-mib 128 --stage-read-mib 128
--kv q8_0
--window 1
expert-profile-core.txt
```

- `--prompt-batch 1024` is the best batch size (the compact-miss path was
  originally gated on this default).
- `--stage-ahead 256` engages the existing compact-miss PCIe staging path.
- `--stage-mib/--stage-read-mib 128` are the default staging buffer sizes.
- **q8_0 KV** is required. 4-bit KV is ruled out by the quality constraint
  (+8-12% perplexity on long documents, `docs/DETAILS.md:88-90`).

## Rejected experiments

- **Stage-ahead 256 alone** — flat at 32K and 64K. The compact-miss path works
  but is not the bottleneck.
- **Boosted expert profile + `STRATA_FREEZE_EXPERTS=1`** — decode regressed to
  36-42 tok/s, prefill to 237 tok/s. Freeze early-returns in `follow()`,
  `shrink_cache()`, `grow_cache()`, `mind_card_vram()`, and
  `resize_card_cache()`; non-core experts pushed out by the boosted profile
  can never come back and routing hits them on the slow CPU path.
- **Graph replay for fixed prompt batches** — gated by
  `(windowed_ || block_pass_ || n==1)` at `engine.cpp:4889` plus missing
  recurrent-state snapshots for prompt chunks. Would need a dedicated flag and
  a gate fix.
- **kuraneko1/Strata fork** (commit 75f4bc9) — built successfully but could not
  load our models: IQ1_S / Q2_K_XL → `unsupported expert format in layer 0`;
  IQ4_NL → `architecture is 'inkling2_loop'`. Its ideas are being ported into
  our engine instead.

## What is needed to run the models

**No engine code change is required.** The installed `engine-poly` executable
accepts all the flags above (verified with `--help`). The one source change
needed for UI visibility is the `inkling2` key in `serve/models.py` BACKENDS.

Commit 315cd2948 (device-side graph replay) is decode-only and does not affect
prefill. It is in this branch's source; no deployed binary has been built from
it yet. Build from main to pick it up.

## Open item

**Tiered core-expert residency** (step 1 from the approved plan) was approved in
principle but **not started**. No profile split has been made. The plan is a
tier1 pre-pass in `fill_expert_cache()` (`engine.cpp:1938-1961`) to claim all
1536 core experts before the value sort. Tier1 residency alone does NOT make
`all_cached()` true — that requires all 3072 experts, which cannot fit on
8 GiB.

## What was not tested

- Graph replay for fixed prompt batches (unmeasured, gated).
- Tiered core-expert residency (not implemented).
- A clean isolation of the cold/warm gap: warm runs reuse conversation-cache
  KV checkpoints as well as expert placement, so the measured 13x gap at 43K
  does not separate those two effects.
