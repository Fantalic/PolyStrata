# Affinity admission: one session at a time, opt-in

Parallel agent sessions (opencode sub-agents and friends) interleave their requests A,B,A,B after
every tool call. Every switch makes the engine re-prefill the other conversation: measured on the
poly box, a turn that follows its predecessor back-to-back reads ~200 new tokens (1.8 s), while a
switch whose conversation was parked re-reads up to ~31K tokens (15-28 s at ~1,430 tok/s) - and
when the last decode left the VRAM tight, the engine restores only the last 4096 tokens of a
parked conversation, so the switch costs nearly the full prefill.

Affinity admission reorders WHO takes the single-request turn next. It never changes what runs,
only when. **Off by default**: start the server with `$env:STRATA_AFFINITY = "1"`, or switch it on
from the monitor page's Request queue panel (`POST /api/queue/settings`).

## The rules, in order

1. **Small prompts run at once** (<= `STRATA_AFFINITY_SMALL_TOKS`, default 4096 tokens): helper
   calls (titles, summaries) cost almost nothing even cold, so they never wait behind a big turn -
   they jump in during a hold or a focus too. A small turn in the middle costs the hot session its
   park/restore round-trip (~0.3-0.6 s).
2. **A continuation runs before a foreign request.** "Same conversation" is the engine's own turn
   checkpoint: the prompt's tokens up to its last turn-start token (`<|im_start|>`), exactly what
   `pick_slot` matches on for batch slots. A session's next turn re-renders that prefix; another
   session's prompt differs before it.
3. **The hold**: after a big turn with no continuation queued, the server holds the turn for
   `hold_s` (default 20 s) - the same session's follow-up usually arrives within seconds (a
   tool-call round-trip) and then runs at delta cost. `hold_s` is one knob for both the wait and
   the foreign request's max deferral: when it expires, the waiting foreign request goes.
4. **Focus mode** (monitor page): pin one conversation; its continuations run first and the server
   waits for the follow-up **without a timeout**, until **Skip** lets exactly one foreign request
   through or **Unfocus** clears it. Small requests keep jumping in throughout.

## The monitor page's Request queue panel

Below the status cards, collapsed until you press Show. One row per waiting request: arrival
time, prompt tokens (small/big), state (queued / affinity / hold / focus), the last user
message's first ~100 characters - that is what identifies the opencode session - and the
endpoint. The panel bar has the Affinity switch, the Hold seconds input (applied to the running
hold at once), **Skip**, and per-row Focus / Unfocus. Backed by `GET /api/queue` and
`POST /api/queue/settings|focus|skip|unfocus` (own page + auth, like /load and /unload).

## Knobs (environment; the panel overrides at run time)

| variable | default | meaning |
|---|---|---|
| `STRATA_AFFINITY` | off | `1` enables the admission |
| `STRATA_AFFINITY_HOLD_S` | 20 | the hold window in seconds (0 = never wait for a follow-up) |
| `STRATA_AFFINITY_SMALL_TOKS` | 4096 | prompts at most this long never wait |
| `STRATA_AFFINITY_RECENT` | 4 | how many served conversations stay matchable |

Notes: `STRATA_ADMIT_SHORT_FIRST` composes with this (affinity orders whole requests, short-first
orders the engine's prompt admissions). Batch mode (`--batch`) is unchanged - its slots already
pick the slot that holds the conversation (pick_slot). The counters (deferrals, holds, skips) are
in `GET /api/queue`; each hold/deferral decision prints a `[strata] affinity:` log line.

## Measuring the effect

Before/after, compare in `C:\.strata\logs\poly\<model>.log` the `prompt N tokens = R reused + M
read` lines of the same session's consecutive turns: the goal is `R` near the previous turn's
prompt size and `M` near the delta only. The queue panel's wait badge shows what the other
sessions paid for it.
