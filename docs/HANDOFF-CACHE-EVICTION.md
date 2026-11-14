# HANDOFF: Poly Strata conversation cache — smooth context handling

**Written for:** Poly Strata engineer  
**Date:** 2026-11-14  
**From:** opencode plugin team  
**Server:** `E:\_tools\strata\_build\PolyStrata`, port 8095

## Problem

The opencode plugin `local-context.ts` does context folding (archives old messages into chunks, keeps a protected tail). The poly server's **conversation cache** evicts cached conversations when VRAM fills up, causing the model to see less context on subsequent requests. This interrupts the agent's workflow and slows down processing.

## Evidence

Poly Strata server logs (`C:\.strata\logs\poly\strata-qwen36-ud-q3_k_m.log`):

```
strata-poly: conversation cache: parked 113574 tokens in 282.1 ms; parked=2 bytes=4035147776 evictions=34
strata-poly: the attention caches gave back 0.09 GiB that this request and the one before do not reach
strata-poly: conversation cache: restored 77824 tokens in 748.2 ms; parked=2 bytes=3574337536 evictions=34
```

Pattern:
- **Parked**: Server stores conversation in cache for reuse (fast, ~200-900ms)
- **Evictions**: Cache fills up, old entries freed (VRAM freed)
- **Restored**: Cache miss, conversation reloaded (slow, ~700-900ms)

When evictions happen, the next request takes **3-5x longer** because the conversation must be reloaded instead of reused from cache.

## Impact

- Agent workflow interrupted by cache misses
- Processing speed drops from ~40 tok/s to ~10 tok/s on cache misses
- Model loses recent context when cache entries are evicted
- No way for the plugin to predict or prevent evictions

## Request

Can the poly server be improved to:

1. **Smarter cache management**: Keep conversations in cache longer, or evict less aggressively (e.g., based on recency + frequency, not just size)
2. **Graceful degradation**: When cache fills, don't just evict — keep the most active conversations and evict idle ones
3. **Warning system**: Log when a conversation is about to be evicted, so the plugin can take action (e.g., fold/compact before eviction)
4. **VRAM optimization**: If possible, reduce the VRAM footprint of cached conversations so more can fit simultaneously

## Environment

- Server runs in `tall` mode (196K ctx, vision OFF)
- 128K model: `qwen3.6-35b-a3b-ud-q3_k_m`
- Affinity queue enabled (`STRATA_AFFINITY=1`)
- Context folding plugin active (caps tool output at 9K chars)

## Files

- Plugin: `C:\Users\FakuPara\.config\opencode\plugins\local-context.ts`
- Server logs: `C:\.strata\logs\poly\strata-qwen36-ud-q3_k_m.log`
- Server source: `E:\_tools\strata\_build\PolyStrata\serve\affinity.py`
