// src/poly/conversations_ram_test.cpp - Tests for fork feature: drop_for_ram()
#include "conversations.hpp"
#include "conversations_ram.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

using strata::poly::CacheOptions;
using strata::poly::Conversations;

namespace {

int checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}

uint64_t mix(uint64_t h, int32_t t) {
    return (h ^ (uint64_t)(uint32_t)t) * 0x9E3779B97F4A7C15ull + 0x7F4A7C15ull;
}

struct Fake {
    static constexpr int64_t ctx = 1 << 16;
    uint64_t h = 1;
    int64_t at = 0, read = 0;
    size_t state = 8;
    std::vector<uint64_t> a = std::vector<uint64_t>(ctx), b = std::vector<uint64_t>(ctx);
    mutable int save_rows_calls = 0;
    mutable size_t save_rows_bytes = 0;
    void reset() { h = 1, at = 0; }
    void eval(const int32_t* t, int64_t n) {
        for (int64_t i = 0; i < n; ++i, ++at, ++read) {
            a[(size_t)at] = h = mix(h, t[i]);
            b[(size_t)at] = (uint64_t)t[i];
        }
    }
    size_t state_bytes() const { return state; }
    void save_state(uint8_t* dst) const { std::memcpy(dst, &h, 8); }
    void load_state(const uint8_t* src, int64_t pos) { std::memcpy(&h, src, 8), at = pos; }
    size_t row_bytes() const { return 16; }
    void save_rows(int64_t first, int64_t n, uint8_t* dst) const {
        ++save_rows_calls;
        save_rows_bytes += (size_t)n * row_bytes();
        std::memcpy(dst, &a[(size_t)first], (size_t)n * 8);
        std::memcpy(dst + n * 8, &b[(size_t)first], (size_t)n * 8);
    }
    void load_rows(const uint8_t* src, int64_t from, int64_t count, int64_t first, int64_t n) {
        std::memcpy(&a[(size_t)first], src + (first - from) * 8, (size_t)n * 8);
        std::memcpy(&b[(size_t)first], src + count * 8 + (first - from) * 8, (size_t)n * 8);
    }
    bool holds(const std::vector<int32_t>& ids) const {
        uint64_t x = 1;
        for (size_t i = 0; i < ids.size(); ++i)
            if (a[i] != (x = mix(x, ids[i])) || b[i] != (uint64_t)ids[i]) return false;
        return at == (int64_t)ids.size() && h == x;
    }
};

int64_t far = 0;
unsigned quits = 0;

struct Chat {
    Fake e;
    Conversations<Fake> kept;
    std::mt19937 rng{7};
    std::vector<std::pair<int64_t, uint8_t*>> due;
    explicit Chat(const CacheOptions& o) : kept(e, o) {}
    int64_t ask(std::vector<int32_t>& ids, int reply = 3, int64_t turn = -1) {
        const int64_t n = (int64_t)ids.size(), from = kept.resume(ids);
        kept.turn_at(turn);
        check(from >= 0 && from < n && e.at == from, "a prompt is taken up before its last token");
        for (int64_t i = from; i < n;) {
            const bool last = i == n - 1;
            const int64_t room = std::min<int64_t>(n - 1 - i, far > 0 ? far : 100);
            const int64_t m = last ? 1 : far > 0 ? room : kept.until_checkpoint(i, room);
            kept.before(i, last);
            if (far > 0 && !last && quits > 0 && rng() % quits == 0) {
                kept.inside(i, m, due);
                e.eval(ids.data() + i, m / 2);
                if (!due.empty() && due[0].second && due[0].first <= m / 2) e.save_state(due[0].second);
                e.h ^= 0x5EEDull;
                const int64_t back = kept.rewind();
                check(back <= i && e.at == back && e.holds(kept.seq()), "a read given up leaves the last checkpoint");
                return ask(ids, reply, turn);
            }
            if (far > 0 && !last) {
                kept.inside(i, m, due);
                int64_t done = 0;
                for (const auto& d : due) {
                    e.eval(ids.data() + i + done, d.first - done);
                    done = d.first;
                    if (d.second) e.save_state(d.second);
                }
                e.eval(ids.data() + i + done, m - done);
            } else {
                e.eval(ids.data() + i, m);
            }
            kept.read(ids.data() + i, m);
            i += m;
        }
        kept.prompt_was(n);
        for (int k = 0; k < reply; ++k) {
            const int32_t t = 1000000 + (int32_t)(rng() % 1000);
            e.eval(&t, 1);
            kept.read(&t, 1);
            ids.push_back(t);
        }
        check(e.holds(ids) && kept.seq() == ids, "the engine holds the prompt and the reply");
        kept.relieve();
        return from;
    }
};

std::vector<int32_t> text(int seed, int n) {
    std::vector<int32_t> v((size_t)n);
    std::mt19937 r((unsigned)seed);
    for (int32_t& t : v) t = (int32_t)(r() % 50000);
    return v;
}

std::vector<int32_t> operator+(std::vector<int32_t> a, const std::vector<int32_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

CacheOptions options(int64_t park_mib, int slots = 4) {
    CacheOptions o;
    o.every = 2048;
    o.park_mib = park_mib;
    o.park_slots = slots;
    o.focus = 0;
    o.shelf_ram_mib = 1;
    return o;
}

// Helper state: host_available models freed RAM from dropped banks.
// Each 3000-token bank frees 3000 * sizeof(int32_t) bytes when dropped.
// CacheOptions is copied into Conversations at construction, so the closure
// must be assigned BEFORE the Chat is built; the kept pointer is filled in
// afterwards through this shared, by-reference-captured state.
struct RamModel {
    const Conversations<Fake>* kept = nullptr;
    int initial_parked = 0;
    uint64_t free0 = 0;
    std::optional<uint64_t> operator()() const {
        if (!kept) return free0;
        long long delta = (long long)initial_parked - (long long)kept->parked();
        if (delta < 0) delta = 0;
        return free0 + uint64_t(delta) * (3000 * sizeof(int32_t));
    }
};

// drop_for_ram(): with ram_target_mib set and host_available modelling freed RAM,
// the smallest bank is dropped until free RAM >= target.
void drop_for_ram_basic() {
    uint64_t free0 = (uint64_t(2) << 20) - 4096;  // just under the 2 MiB target
    CacheOptions o = options(64, 3);               // 4 slots total (park_slots=3)
    o.park_min = 0;
    o.ram_target_mib = 2;                          // target: 2 MiB free
    RamModel ram; ram.free0 = free0; ram.initial_parked = 3;
    o.host_available = [&ram] { return ram(); };
    Chat c(o);
    ram.kept = &c.kept;

    // Park three conversations: A (slot 0), B (slot 1), C (slot 2)
    std::vector<int32_t> a = text(1, 3000), b = text(2, 3000), c2 = text(3, 3000);
    c.ask(a);  // a current (slot 0)
    c.ask(b);  // a parked (slot 0), b current (slot 1)
    c.ask(c2); // b parked (slot 1), c2 current (slot 2)
    check(c.kept.parked() == 2, "two conversations are parked");

    // D parks (slot 3): now all 4 slots used
    std::vector<int32_t> d = text(4, 3000);
    c.ask(d);  // c2 parks (slot 2), d current (slot 3)
    check(c.kept.parked() == 3, "three conversations are parked, all slots full");

    // Free RAM < 2 MiB target: drop_for_ram drops the smallest (A, oldest)
    check(c.kept.drop_for_ram() == 1, "one bank was dropped for the RAM target");
    check(c.kept.parked() == 2, "two banks remain parked");
    check(c.ask(b) > 0, "b resumes from its checkpoint (not evicted)");
    check(c.ask(c2) > 0, "c2 resumes from its checkpoint (not evicted)");
    check(c.ask(d) > 0, "d is current and resumes from its checkpoint");
    check(c.ask(a) == 0, "a was evicted: re-read from 0");
}

// drop_for_ram(): under pressure, oldest bank dropped first; current slot, newest parked survive.
void drop_for_ram_pressure() {
    uint64_t free0 = (uint64_t(2) << 20) - 4096;
    CacheOptions o = options(64, 3);
    o.park_min = 0;
    o.ram_target_mib = 2;
    RamModel ram; ram.free0 = free0; ram.initial_parked = 3;
    o.host_available = [&ram] { return ram(); };
    Chat c(o);
    ram.kept = &c.kept;

    // Park four conversations: A (oldest, slot 0), B (slot 1), C (newest, slot 2), D (current, slot 3)
    std::vector<int32_t> a = text(1, 3000), b = text(2, 3000), c2 = text(3, 3000), d = text(4, 3000);
    c.ask(a); c.ask(b); c.ask(c2); c.ask(d);
    check(c.kept.parked() == 3, "three conversations parked, all slots full");

    check(c.kept.drop_for_ram() == 1, "one bank dropped: A (oldest)");
    check(c.kept.parked() == 2, "two banks remain");
    check(c.ask(b) > 0, "b resumes from checkpoint (survived)");
    check(c.ask(c2) > 0, "c (newest parked) spared");
    check(c.ask(d) > 0, "d (current) spared");
    check(c.ask(a) == 0, "a was evicted (re-read from 0)");
}

// drop_for_ram(): ram_target_mib = 0 (default) is a no-op.
void drop_for_ram_default_off() {
    CacheOptions o = options(64, 4);  // 5 slots (park_slots=4)
    o.park_min = 0;
    Chat c(o);

    std::vector<int32_t> a = text(1, 3000), b = text(2, 3000), c2 = text(3, 3000);
    c.ask(a); c.ask(b); c.ask(c2);
    check(c.kept.parked() == 2, "two conversations parked");

    check(c.kept.drop_for_ram() == 0, "drop_for_ram returns 0 when ram_target_mib is 0");
    check(c.kept.parked() == 2, "no banks were dropped");
    check(c.ask(a) > 0, "a still resumes from checkpoint");
    check(c.ask(b) > 0, "b still resumes from checkpoint");
}

// drop_for_ram(): host_available returns nullopt (unknown) — no drops.
void drop_for_ram_unknown_host() {
    CacheOptions o = options(64, 4);
    o.park_min = 0;
    o.host_available = [] { return std::optional<uint64_t>{}; };  // unknown
    o.ram_target_mib = 2;
    Chat c(o);

    std::vector<int32_t> a = text(1, 3000), b = text(2, 3000), c2 = text(3, 3000);
    c.ask(a); c.ask(b); c.ask(c2);
    check(c.kept.parked() == 2, "two conversations parked");

    check(c.kept.drop_for_ram() == 0, "drop_for_ram returns 0 when host_available is unknown");
    check(c.kept.parked() == 2, "no banks were dropped");
}

// adopt(): stamps used so an adopted bank ages like a resumed one — drop_stale skips banks
// without a used stamp forever, so adopt() must leave one behind. drop_stale never drops the
// current slot, so the bank is droppable only once it is parked, which needs turn checkpoints
// (an adopted bank without them is forgotten by the next transition, not parked).
void adopt_sets_used_for_ttl() {
    CacheOptions o = options(64, 4);
    o.park_min = 0;
    Chat c(o);

    std::vector<int32_t> a = text(1, 2000), b = text(2, 2000);
    c.ask(a); c.ask(b);
    check(c.kept.parked() == 1, "a is parked");

    c.kept.adopt(text(3, 1000));
    check(c.kept.parked() == 1, "adopted conversation is in the current slot, a still parked");
    check(c.kept.used_at(1) != std::chrono::steady_clock::time_point{}, "adopt() stamped used");
    check(c.kept.drop_stale(60) == 0, "adopted bank is current and survives drop_stale");
}

// drop_for_ram(): wanted hook protects the best victim; next-best is dropped instead.
void drop_for_ram_wanted_skips() {
    uint64_t free0 = (uint64_t(2) << 20) - 4096;
    CacheOptions o = options(64, 3);
    o.park_min = 0;
    o.ram_target_mib = 2;
    o.wanted = [](size_t i) { return i == 0; };
    RamModel ram; ram.free0 = free0; ram.initial_parked = 3;
    o.host_available = [&ram] { return ram(); };
    Chat c(o);
    ram.kept = &c.kept;

    // Park three conversations: A (oldest/smallest, slot 0), B (slot 1), C (newest, slot 2)
    std::vector<int32_t> a = text(1, 3000), b = text(2, 3000), c2 = text(3, 3000);
    c.ask(a); c.ask(b); c.ask(c2);
    check(c.kept.parked() == 2, "two conversations parked");

    // D parks (slot 3): now all 4 slots full
    std::vector<int32_t> d = text(4, 3000);
    c.ask(d);
    check(c.kept.parked() == 3, "three conversations parked, all slots full");

    check(c.kept.drop_for_ram() == 1, "wanted hook skipped the best victim; next-best was dropped");
    check(c.kept.parked() == 2, "two banks remain parked");
    check(c.ask(b) == 0, "b was evicted (re-read from 0)");
    check(c.ask(a) > 0, "a (wanted) survived drop_for_ram");
    check(c.ask(c2) > 0, "c (newest) survived");
    check(c.ask(d) > 0, "d (current) survived");
}

}  // namespace

int main() {
    drop_for_ram_basic();
    drop_for_ram_pressure();
    drop_for_ram_default_off();
    drop_for_ram_unknown_host();
    adopt_sets_used_for_ttl();
    drop_for_ram_wanted_skips();
    std::printf("conversations_ram_test: %d checks passed\n", checks);
    return 0;
}
