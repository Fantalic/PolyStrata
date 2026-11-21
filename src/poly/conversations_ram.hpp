#pragma once
// Fork feature: host-RAM-pressure relief for the conversation cache.
// Defined here, out-of-class, so src/poly/conversations.hpp stays a clean
// merge surface with the original repo — that file carries only the
// declaration and the CacheOptions fields.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace strata::poly {

template<class E>
size_t Conversations<E>::drop_for_ram() {
    if (o_.ram_target_mib <= 0) return 0;
    auto avail = o_.host_available ? o_.host_available() : std::optional<uint64_t>{};
    if (!avail) return 0;
    const uint64_t target = (uint64_t) o_.ram_target_mib << 20;
    size_t n = 0;
    std::vector<size_t> skip;
    for (;;) {
        if (*avail >= target) break;
        size_t out;
        if (!pick_victim(slots_.size(), out, skip)) break;
        if (o_.wanted && o_.wanted(out)) { skip.push_back(out); continue; }
        slots_[out] = Slot{};
        ++n;
        const auto fresh = o_.host_available ? o_.host_available() : std::optional<uint64_t>{};
        if (fresh) *avail = *fresh;
    }
    if (n && o_.log) {
        const Held h = held(cur_);
        std::fprintf(o_.log,
                      "strata-poly: conversation cache: dropped %zu parked banks for the RAM target (%lld MiB free < %lld); parked=%zu bytes=%zu evictions=%zu heap=%lld MiB\n",
                      n, (long long) (*avail >> 20), o_.ram_target_mib, h.count, h.bytes, evictions_,
                      (long long) (spill_heap_bytes() >> 20));
    }
    return n;
}

/// sync_tags() — ensure session_tags_ is the same size as slots_; new entries are empty.
/// Called by tag_session() before indexing.
template<class E>
void Conversations<E>::sync_tags() {
    if (session_tags_.size() < slots_.size()) session_tags_.resize(slots_.size());
}

/// The pending session tag (the server's SESSION line) and the per-slot tags.
/// A tagged session keeps exactly one bank: when its request lands, parked banks
/// with the same tag that the request did not enter are dropped (the client's
/// context compaction rewrites the prompt - the old bank is superseded).
template<class E>
void Conversations<E>::set_pending_session(const std::string& id) { pending_session_ = id; }

template<class E>
void Conversations<E>::tag_session() {
    sync_tags();
    if (!pending_session_.empty()) {
        drop_superseded(pending_session_);   // the request's own session: superseded banks go now
        session_tags_[cur_] = pending_session_;
        pending_session_.clear();
    }
}

/// Drops every parked slot tagged `id` - called right after tag_session() for the
/// request's own session: any same-tag bank still parked was not entered by this
/// request (it entered would have made it cur_), so it is superseded.
/// Returns how many were dropped.
///
/// Note: empty slots with stale tags never match (seq.empty() guard) - harmless.
/// When drop_superseded drops, it clears the tag. Slots_[cur_] = Slot{} resets
/// leave the tag (stale on empty slot, harmless).
template<class E>
size_t Conversations<E>::drop_superseded(const std::string& id) {
    size_t n = 0;
    int64_t tokens = 0;
    for (size_t i = 0; i < slots_.size(); ++i) {
        Slot& s = slots_[i];
        if (i == cur_ || s.seq.empty() || i >= session_tags_.size() || session_tags_[i] != id) continue;
        tokens += len(s);
        s = Slot{};
        session_tags_[i].clear();
        ++n;
    }
    if (n && o_.log) {
        const Held h = held(cur_);
        std::fprintf(o_.log,
                     "strata-poly: conversation cache: dropped %zu superseded banks of session %s (%lld tokens); parked=%zu bytes=%zu evictions=%zu heap=%lld MiB\n",
                     n, id.c_str(), (long long) tokens, h.count, h.bytes, evictions_,
                     (long long) (spill_heap_bytes() >> 20));
    }
    return n;
}

}  // namespace strata::poly
