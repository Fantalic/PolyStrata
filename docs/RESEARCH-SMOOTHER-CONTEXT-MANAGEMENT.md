# Research: Smoother Context Management During Idle Periods

**Date:** 2026-11-14  
**Purpose:** Investigate ways to make context management smoother when agents are idle (e.g., calling scripts, waiting for processes)

## Current State (from HANDOFF-CACHE-EVICTION.md)

### The Problem
- Poly Strata's conversation cache evicts conversations when VRAM fills up
- Pattern: `parked` (fast, ~200-900ms) → `evictions` (VRAM freed) → `restored` (slow, ~700-900ms)
- When evictions happen, the next request takes **3-5x longer** because the conversation must be reloaded
- Agent workflow interrupted by cache misses
- Processing speed drops from ~40 tok/s to ~10 tok/s on cache misses
- No way for the plugin to predict or prevent evictions

### Current Mechanism
- **Conversation cache**: Opt-in via `--conversation-cache-mib` (VRAM budget) and `--conversation-cache-slots` (max conversations)
- **Slot picking** (`pick_slot`): Chooses the slot whose held tokens are the longest prefix match to the new prompt
- **TURN_ID checkpoint** (248045): The engine takes one at the last `<|im_start|>` of a prompt, so a prompt that agrees with the held tokens up to there is that chat's next turn
- **Affinity queue**: Reorders requests so continuations run before foreign ones, keeping the same conversation cached

### What Happens During Idle Periods
When an agent calls a script or waits for a process:
1. The agent's conversation is parked in the cache
2. Other requests (sub-agents, other sessions) may come in
3. If VRAM fills up, the idle conversation may be evicted
4. When the agent returns, the conversation must be restored (slow)

## Research Findings

### 1. NVIDIA TensorRT-LLM: Priority-Based Eviction
**Source:** [NVIDIA Technical Blog, Jan 2025](https://developer.nvidia.com/blog/introducing-new-kv-cache-reuse-optimizations-in-nvidia-tensorrt-llm)

- **Priority-based eviction**: Users can specify priority and duration for each request
- Higher priority = more likely to stay cached
- **Duration**: Controls how long a block should persist
- **Key insight**: Knowledge of workload patterns can optimize cache management

### 2. KVFlow: Workflow-Aware Eviction
**Source:** [arXiv:2507.07400, Jul 2025](https://arxiv.org/abs/2507.07400)

- **Agent Step Graph**: Abstracts agent execution schedule
- **Steps-to-execution**: Estimates temporal proximity to future activation
- **Fine-grained eviction**: Preserves entries likely to be reused
- **Prefetching**: Proactively loads required tensors from CPU to GPU in background threads
- **Result**: Up to 1.83x speedup for single workflows

### 3. SmartCache: Context-Aware Semantic Cache
**Source:** [NeurIPS 2025](https://proceedings.neurips.cc/paper_files/paper/2025/file/fb74b63d225f846e6032bf3e3ab0f4ec-Paper-Conference.pdf)

- **STEP policy** (Shortest Time to Execution and Priority): Better than LRU/LFU
- **Hierarchical semantic indexing**: Combines exact prefix matching with semantic similarity
- **Resource reuse**: Reuses KV cache, not just text response
- **Result**: Significantly reduces redundant computations and GPU memory usage

### 4. LLMs Know What to Drop: Self-Attention Guided KV Cache Eviction
**Source:** [arXiv:2503.08879, Mar 2025](https://arxiv.org/abs/2503.08879)

- **Self-attention guided eviction**: Uses the model's own attention weights to decide what to keep
- **Token-level importance**: Tokens with higher attention weights are more important
- **Result**: Efficient long-context inference with minimal quality loss

### 5. Hybrid Context Compaction (JetBrains Research)
**Source:** [JetBrains Blog, Dec 2025](https://blog.jetbrains.com/research/2025/12/efficient-context-management/)

- **Observation masking** (primary): Replace older tool outputs with placeholders
- **LLM summarization** (fallback): Compress older interactions via separate LLM call
- **Key finding**: Observation masking outperforms LLM summarization in cost and reliability
- **Latency**: Masking is fast (no API call), summarization is slow (extra LLM call)

### 6. Acon: Optimizing Context Compression for Long-horizon LLM Agents
**Source:** [arXiv:2510.00615v3, Oct 2025](https://arxiv.org/html/2510.00615v3)

- **Universal optimization framework**: Works with any LLM agent
- **Reinforcement learning**: Optimizes compression and agent action policy
- **Context compression as agent action**: Model learns when and how to compress

## Recommendations for PolyStrata

### 1. **Idle Detection and Cache Protection**
**Implementation:** Track when a conversation is idle (no requests for X seconds) and protect it from eviction.

- **How**: Add idle detection to the affinity queue. When a conversation is idle for >30s, mark it as "protected" (won't be evicted).
- **Benefit**: Prevents eviction of conversations that are likely to be reused soon.
- **Cost**: Low (just tracking idle time).

### 2. **Prefetching During Idle Periods**
**Implementation:** When an agent is idle (e.g., waiting for a script), proactively load the conversation cache into GPU memory.

- **How**: Add a background thread that monitors idle periods and prefetches conversations.
- **Benefit**: Reduces restore time when the agent returns.
- **Cost**: Moderate (requires background thread and GPU memory management).

### 3. **Context Compaction During Idle Periods**
**Implementation:** When an agent is idle, compact the conversation (summarize old messages) to reduce VRAM footprint.

- **How**: Add a context compaction endpoint that can be triggered during idle periods.
- **Benefit**: Reduces VRAM usage, allowing more conversations to fit in cache.
- **Cost**: High (requires LLM call for summarization).

### 4. **Priority-Based Eviction**
**Implementation:** Assign priorities to conversations based on activity level.

- **How**: Track conversation activity (frequency of requests, recency) and assign priorities.
- **Benefit**: Active conversations stay cached, idle ones are evicted first.
- **Cost**: Low (just tracking activity metrics).

### 5. **Plugin Integration for Context Awareness**
**Implementation:** The opencode plugin (`local-context.ts`) can provide context awareness to the server.

- **How**: The plugin can send metadata about the agent's state (idle, working, waiting) to the server.
- **Benefit**: Server can make smarter cache decisions based on agent state.
- **Cost**: Low (plugin can send lightweight metadata).

## Proposed Architecture

### Phase 1: Idle Detection and Cache Protection (Low Cost)
1. Add idle detection to the affinity queue
2. Mark conversations as "protected" when idle for >30s
3. Prevent eviction of protected conversations

### Phase 2: Priority-Based Eviction (Low Cost)
1. Track conversation activity metrics (frequency, recency)
2. Assign priorities to conversations
3. Evict lowest priority conversations first

### Phase 3: Plugin Integration (Low Cost)
1. Add metadata endpoint for plugin to send agent state
2. Server uses agent state to make cache decisions
3. Plugin can request cache protection for active conversations

### Phase 4: Prefetching (Moderate Cost)
1. Add background thread for cache prefetching
2. Monitor idle periods and prefetch conversations
3. Reduce restore time when agent returns

### Phase 5: Context Compaction (High Cost)
1. Add context compaction endpoint
2. Trigger compaction during idle periods
3. Summarize old messages to reduce VRAM footprint

## Next Steps

1. **Implement Phase 1-3** (idle detection, priority-based eviction, plugin integration)
2. **Test with real workloads** to validate improvements
3. **Consider Phase 4-5** if more optimization is needed

## Files to Modify

- `serve/affinity.py`: Add idle detection, priority tracking
- `serve/server.py`: Add cache protection, prefetching, compaction
- `serve/web/app.js`: Update UI to show cache status, idle state
- `serve/web/app.css`: Update styles for new UI elements
- `local-context.ts`: Add metadata endpoint, cache protection requests
