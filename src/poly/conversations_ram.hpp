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

}  // namespace strata::poly
