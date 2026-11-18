# Affinity queue

A focused session keeps the engine for 20 s after a turn (`STRATA_AFFINITY_HOLD_S`, default 20). A big request
from another session waits out the window.

## Hold window

Twenty seconds of engine time after a turn (`STRATA_AFFINITY_HOLD_S`, default 20). A foreign request that
lands during the window queues: it reads its prompt fresh when the window ends. Measured on this PC (RTX 3060
Ti, Qwen3.6-35B-A3B, 12,288 MiB conversation-cache budget) a foreign 20K request waited the full 20 s
(queue counter `deferred=1`), then read its 20,019 fresh tokens at 2,049 tok/s.

## Quick calls never queue

Requests at most `STRATA_AFFINITY_SMALL_TOKS` tokens (default 8,192, covers the ~7.8K title/summary helpers)
run at once and never queue. They do not end the focus window: what they do to the armed countdown is
`STRATA_AFFINITY_HOLD_SMALL`. Measured on this PC helpers ran in 0.6 to 0.7 s with zero deferrals.

## Hold-small: pause, reset, run

`STRATA_AFFINITY_HOLD_SMALL` controls what a quick call does to the 20 s countdown. Three modes:

- **pause** (default): the countdown freezes while the quick call runs. A 4.6 s helper consumed 0.7 s of the
  20 s window (19.9 s remaining before it, 19.2 s after).
- **reset**: the quick call starts a fresh 20 s window.
- **run**: the quick call burns into the countdown.

Measured on this PC a pause-mode helper consumed 0.7 s of the 20 s window (19.9 s remaining before it,
19.2 s after).
