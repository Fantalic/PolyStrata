// src/poly/conversations_ram_test.cpp - Tests for fork feature: drop_for_ram()
#include "conversations.hpp"
#include "conversations_ram.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <string>

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
    static constexpr int64_t ctx = 1 << 20;   // the row caches: big enough for the disk-tier tests' banks
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
        kept.tag_session();   // the engine tags the entered slot per request and drops the session's superseded banks
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

// drop_all(): no parked banks — returns 0, parked stays 0, no log line.
void drop_all_no_parked() {
    CacheOptions o = options(64, 4);
    Chat c(o);

    std::vector<int32_t> a = text(1, 3000);
    c.ask(a);
    check(c.kept.parked() == 0, "no parked conversations after first ask");

    check(c.kept.drop_all() == 0, "drop_all returns 0 with nothing parked");
    check(c.kept.parked() == 0, "parked stays 0");
}

// drop_all(): three parked conversations — returns 3, parked == 0, current slot survives.
void drop_all_three_parked() {
    CacheOptions o = options(64, 3);
    o.park_min = 0;
    Chat c(o);

    // Park three conversations: A (slot 0), B (slot 1), C (slot 2), D current (slot 3)
    std::vector<int32_t> a = text(1, 3000), b = text(2, 3000), c2 = text(3, 3000), d = text(4, 3000);
    c.ask(a); c.ask(b); c.ask(c2); c.ask(d);
    check(c.kept.parked() == 3, "three conversations parked, all slots full");

    check(c.kept.drop_all() == 3, "drop_all returns 3");
    check(c.kept.parked() == 0, "parked == 0 after drop_all");
    check(c.kept.parked_bytes() == 0, "parked_bytes == 0 after drop_all");

    // Current conversation's slot survives: a follow-up that continues it reuses its cache.
    std::vector<int32_t> cont = text(4, 3200);
    check(c.ask(cont) > 0, "the current conversation's cache survived drop_all (follow-up reuses)");
    check(c.kept.parked() == 0, "the continued conversation was not parked");
}

// drop_all(): log line appears when logging is enabled.
void drop_all_log_line() {
    FILE* logf = nullptr;
    fopen_s(&logf, "drop_all_test.log", "w");
    check(logf != nullptr, "log file opened");
    if (!logf) return;

    CacheOptions o = options(64, 4);
    o.park_min = 0;
    o.log = logf;
    Chat c(o);

    std::vector<int32_t> a = text(1, 3000), b = text(2, 3000), c2 = text(3, 3000);
    c.ask(a); c.ask(b); c.ask(c2);
    check(c.kept.parked() == 2, "two conversations parked before drop_all");

    c.kept.drop_all();

    fclose(logf);
    logf = nullptr;

    FILE* rf = nullptr;
    fopen_s(&rf, "drop_all_test.log", "r");
    check(rf != nullptr, "log file readable for assertion");
    if (rf) {
        char buf[1024] = "";
        while (std::fgets(buf, (int)sizeof(buf), rf)) {}   // the last line is drop_all's
        fclose(rf);
        check(std::strstr(buf, "dropped 2 parked banks") != nullptr,
              "log contains 'dropped 2 parked banks'");
        check(std::strstr(buf, "for the clear") != nullptr,
              "log contains 'for the clear'");
        std::remove("drop_all_test.log");
    }
}

// wanted_sessions_ survives TTL expiry: session S's bank stays when wanted,
// is dropped once the wanted set is cleared.
void wanted_survives_ttl() {
    CacheOptions o = options(64, 4);
    o.park_min = 0;
    Chat c(o);

    // Park a conversation from session S
    std::vector<int32_t> a = text(1, 3000), b = text(2, 3000);
    c.ask(a);
    c.kept.set_pending_session("S");
    c.kept.tag_session();
    c.ask(b);
    check(c.kept.parked() == 1, "a is parked from session S");

    // Age the parked slot past the TTL
    auto old_time = std::chrono::steady_clock::now() - std::chrono::hours(1);
    c.kept.set_slot_used(0, old_time);

    // With S wanted: the bank survives drop_stale
    c.kept.set_wanted_sessions({"S"});
    check(c.kept.drop_stale(60) == 0, "wanted session S's bank survives TTL expiry");
    check(c.kept.parked() == 1, "parked unchanged after drop_stale (wanted)");

    // Clear the wanted set: now the bank gets dropped
    c.kept.set_wanted_sessions({});
    check(c.kept.drop_stale(60) == 1, "bank dropped once wanted set cleared");
    check(c.kept.parked() == 0, "parked == 0 after drop_stale (unwanted)");
}

// wanted_sessions_ survives eviction: only the unwanted session's bank is evicted.
void wanted_survives_eviction() {
    uint64_t free0 = (uint64_t(2) << 20) - 4096;
    CacheOptions o = options(64, 3);
    o.park_min = 0;
    o.ram_target_mib = 2;
    RamModel ram; ram.free0 = free0; ram.initial_parked = 2;
    o.host_available = [&ram] { return ram(); };
    Chat c(o);
    ram.kept = &c.kept;

    // Park two conversations: a (slot 0) and b (slot 1)
    std::vector<int32_t> a = text(1, 3000), b = text(2, 3000);
    c.ask(a);
    c.ask(b);
    check(c.kept.parked() == 1, "a is parked");

    // Tag slot 0 (a) as session S, slot 1 (b) as session other
    c.kept.set_pending_session("S");
    c.kept.tag_session();
    c.kept.set_pending_session("other");
    c.kept.tag_session();

    // Set S as wanted: only S's bank should be protected
    c.kept.set_wanted_sessions({"S"});

    // Now park c2 (slot 2, current): all 3 slots used
    std::vector<int32_t> c2 = text(3, 3000);
    c.ask(c2);
    check(c.kept.parked() == 2, "a and b are parked, c2 current");

    // Force eviction: drop_for_ram should evict b (not wanted), spare a (wanted)
    check(c.kept.drop_for_ram() == 1, "one bank evicted (the unwanted one)");
    check(c.kept.parked() == 1, "only b evicted: a (S) still parked");
}

// drop_all(): only the current conversation is spared.
void drop_all_cur_spared() {
    CacheOptions o = options(64, 2);
    o.park_min = 0;
    Chat c(o);

    // Park two conversations: a2 (slot 0), b2 (slot 1), c2 current (slot 2)
    std::vector<int32_t> a2 = text(10, 2000), b2 = text(20, 2000), c2 = text(30, 2000);
    c.ask(a2); c.ask(b2); c.ask(c2);
    check(c.kept.parked() == 2, "two parked, one current");

    check(c.kept.drop_all() == 2, "drop_all returns 2 parked");
    check(c.kept.parked() == 0, "parked == 0 after drop_all");

    // b2 was parked in slot 1: drop_all spared cur_ (slot 2), b2 was cleared.
    // Resuming b2 should re-read from 0 since it was dropped.
    check(c.ask(b2) == 0, "b2 was dropped: re-read from 0");
    // c2 was current and is spared by drop_all: the switch parked it (normal shelf behavior),
    // and its follow-up reuses the parked cache.
    check(c.kept.parked() == 1, "the switch parked the spared current conversation (normal behavior)");
    std::vector<int32_t> cont2 = text(30, 2200);
    check(c.ask(cont2) > 0, "c2 (current) survived drop_all - its follow-up reuses");
}

// Fork feature: session tags and rewrite-drop.
// The server sends "SESSION <id>" before each GEN request. The slot the request lands in
// gets the tag. When a tagged session's request lands, parked banks with the same tag that
// the request did not enter (the client's context compaction rewrote the prompt) are dropped.

void rewrite_drop_basic() {
    // Session S parks a conversation, then a tagged request with a fully divergent prompt
    // arrives → the old S bank is dropped (superseded by the rewrite).
    CacheOptions o = options(64, 4);
    o.park_min = 0;
    Chat c(o);

    // Park A, B, C in slots 0, 1, 2
    std::vector<int32_t> a = text(1, 1000), b = text(2, 1000), c2 = text(3, 1000);
    c.ask(a);
    c.ask(b);
    c.ask(c2);
    check(c.kept.parked() == 2, "A and B are parked, C is current");

    // Session S: tag slot 2 (C's conversation) as belonging to session S
    c.kept.set_pending_session("S");
    c.kept.tag_session();
    check(c.kept.parked() == 2, "tagging does not change parked count");

    // Session S's next turn: a fully divergent prompt (0 common tokens)
    // This should park the old S bank (slot 2) and create a fresh slot.
    // tag_session() then drops the old S bank (parked, tagged S, not entered by this request).
    std::vector<int32_t> divergent = text(99, 1000);  // tokens 99xxx: no overlap
    c.kept.set_pending_session("S");
    c.ask(divergent);
    // ask(): C's slot parks (parked=3), then the ask's tag_session() drops the S-tagged bank (parked=2).
    check(c.kept.parked() == 2, "the old S bank (C's) was dropped (superseded)");
    // A and B's banks still exist.
    check(c.ask(a) > 0, "A resumes from its checkpoint");
    check(c.ask(b) > 0, "B resumes from its checkpoint");
}

void current_bank_survives() {
    // Session S's conversation is CURRENT (not parked). Next turn arrives tagged S.
    // Nothing is dropped: the entered slot is cur_ (protected by the drop_superseded guard).
    CacheOptions o = options(64, 4);
    o.park_min = 0;
    Chat c(o);

    // Park A in slot 0, B in slot 1
    std::vector<int32_t> a = text(1, 1000), b = text(2, 1000);
    c.ask(a);
    c.ask(b);
    check(c.kept.parked() == 1, "A is parked");

    // Session S: B is current, tag it
    c.kept.set_pending_session("S");
    c.kept.tag_session();
    check(c.kept.parked() == 1, "tagging current slot does not drop it");

    // Session S's next turn: a divergent prompt. B's slot parks in the switch and is tagged S,
    // so the ask's tag_session() drops it (the rewrite supersedes S's previous bank). What
    // survives parked is A's bank.
    std::vector<int32_t> divergent = text(99, 1000);
    c.kept.set_pending_session("S");
    c.ask(divergent);
    check(c.kept.parked() == 1, "after the rewrite-drop, A's bank survives (parked=1)");
    check(c.ask(a) > 0, "A resumes from its checkpoint");
}

void different_session_bank_survives() {
    // A's bank parked with tag A; a request tagged S (S != A) arrives -> A's bank survives.
    CacheOptions o = options(64, 4);
    o.park_min = 0;
    Chat c(o);

    std::vector<int32_t> a = text(1, 1000), a_cont = text(1, 1200), b = text(2, 1000);
    c.ask(a);
    c.kept.set_pending_session("A");   // tags A's slot while it is current
    c.ask(a_cont);
    c.ask(b);                          // parks A; the parked slot keeps its tag
    check(c.kept.parked() == 1, "A is parked, B is current");

    c.kept.set_pending_session("S");   // a different session
    std::vector<int32_t> divergent = text(99, 1000);
    c.ask(divergent);
    check(c.kept.parked() == 2, "the different-session banks survive (nothing was dropped)");
    check(c.ask(a_cont) > 0, "A's bank survived the S request (it resumes)");
}

void untagged_flow_unchanged() {
    // No SESSION line → no drops, no tags. Legacy pile-up behavior intact.
    CacheOptions o = options(64, 4);
    o.park_min = 0;
    Chat c(o);

    // Park A, B, C
    std::vector<int32_t> a = text(1, 1000), b = text(2, 1000), c2 = text(3, 1000);
    c.ask(a);
    c.ask(b);
    c.ask(c2);
    check(c.kept.parked() == 2, "A and B are parked, C is current");

    // No set_pending_session, no tag_session: legacy flow
    std::vector<int32_t> divergent = text(99, 1000);
    c.ask(divergent);
    // All four banks exist (pile-up continues):
    check(c.kept.parked() == 3, "untagged: no drops (A, B, C parked, the new one current)");
}

// wanted_is_session_scoped: no wanted set → nothing protected, the bank is dropped.
void wanted_is_session_scoped() {
    CacheOptions o = options(64, 4);
    o.park_min = 0;
    Chat c(o);

    // Park a conversation
    std::vector<int32_t> a = text(1, 3000), b = text(2, 3000);
    c.ask(a);
    c.kept.set_pending_session("S");
    c.kept.tag_session();
    c.ask(b);
    check(c.kept.parked() == 1, "a is parked from session S");

    // Age the parked slot past the TTL
    auto old_time = std::chrono::steady_clock::now() - std::chrono::hours(1);
    c.kept.set_slot_used(0, old_time);

    // No wanted set: the bank should be dropped by TTL
    c.kept.set_wanted_sessions({});
    check(c.kept.drop_stale(60) == 1, "bank dropped when no wanted set");
    check(c.kept.parked() == 0, "parked == 0 after drop_stale (control)");
}

// The checkpoint ladder: with more checkpoints than are kept, the kept marks spread over the prompt, so a
// mid-prompt rewrite still finds a rung at or below the divergence and the bank is salvaged instead of parked
// and re-read whole.

// The whole log file as one string, for the tests that watch the cache's report lines.
std::string log_text(const char* path) {
    std::string s;
    std::FILE* f = std::fopen(path, "rb");
    if (f) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

// With checkpoints=10 (the server's --prompt-cache 10), a 61000-token conversation holds rungs on the
// geometric ladder: a probe that diverges at D comes back at a rung worth at least a third of D.  With the
// old bunch-at-tail eviction every probe below the last two checkpoints came back at 0.
void ladder_rungs_probed() {
    const std::vector<int32_t> p1 = text(1, 61000);
    const int64_t ds[] = {3000, 5000, 9000, 16384, 24576, 40000, 55000};
    for (const int64_t d : ds) {
        CacheOptions o = options(64, 4);
        o.checkpoints = 10;
        Chat c(o);
        std::vector<int32_t> ids = p1;
        c.ask(ids);
        std::vector<int32_t> probe(ids.begin(), ids.begin() + d);
        probe = probe + text(20, 300);
        const int64_t from = c.ask(probe);
        check(from > 0 && from >= d / 3, "a mid-prompt divergence comes back at a ladder rung");
    }
}

// The parked shape: the bank sits parked when the rewrite arrives, so the salvage has to bring it back
// into the engine (the rows and the state, bit for bit) and go on from the rung.
void parked_bank_salvage() {
    CacheOptions o = options(64, 4);
    o.checkpoints = 10;
    Chat c(o);
    std::vector<int32_t> ids = text(1, 61000);
    c.ask(ids);
    std::vector<int32_t> other = text(9, 500);
    other[0] = ids[0] == 49999 ? 0 : ids[0] + 1;   // starts unlike: the long bank parks, nothing salvageable yet
    c.ask(other);
    check(c.kept.parked() == 1, "the long bank is parked");
    std::vector<int32_t> probe(ids.begin(), ids.begin() + 40000);
    probe = probe + text(21, 300);
    const int64_t from = c.ask(probe);
    check(from > 0 && from >= 40000 / 3, "a parked bank is salvaged from a mid-prompt divergence");
}

// The rewrite flow that emptied the cache: a session's full-context prompt, then rewrites that diverge
// mid-prompt.  Each turn must salvage the session's bank from a rung; a turn that parks and re-reads
// whole is the old behaviour - the bank was dropped as superseded and the next turn started from 0.
void rewrite_flow_salvages() {
    CacheOptions o = options(64, 4);
    o.checkpoints = 10;
    o.log = std::fopen("ladder_flow_log.txt", "w");
    Chat c(o);
    const std::vector<int32_t> ctx = text(1, 61000);
    std::vector<int32_t> t1 = ctx;
    c.kept.set_pending_session("S5");
    c.ask(t1);
    // Turn 2: the same context with a rewritten tail, shorter than the first prompt.
    std::vector<int32_t> t2(ctx.begin(), ctx.begin() + 40000);
    t2 = t2 + text(30, 300);
    c.kept.set_pending_session("S5");
    const int64_t from2 = c.ask(t2);
    check(from2 > 0 && from2 >= 40000 / 3, "the rewrite comes back at a rung of the first turn's bank");
    // Turn 3: another rewrite - the fork of turn 2 copied the rungs at or below its entry point.
    std::vector<int32_t> t3(ctx.begin(), ctx.begin() + 40000);
    t3 = t3 + text(31, 350);
    c.kept.set_pending_session("S5");
    const int64_t from3 = c.ask(t3);
    check(from3 > 0 && from3 >= 40000 / 3, "the next rewrite still comes back at a rung (the fork kept the ladder)");
    if (o.log) { std::fflush(o.log); std::fclose(o.log); }
    const std::string log = log_text("ladder_flow_log.txt");
    // Salvageable turns must not print the scratch diagnostic: that line is for turns that give up.
    check(log.find("re-reading from scratch") == std::string::npos, "salvageable turns say nothing about re-reading from scratch");
}

// A rewrite that makes the prompt shorter: the salvage point is capped by the new prompt's length, and the
// salvaged conversation goes on from its own end afterwards.
void shrink_salvages() {
    CacheOptions o = options(64, 4);
    o.checkpoints = 10;
    Chat c(o);
    std::vector<int32_t> ids = text(1, 61000);
    c.ask(ids);
    std::vector<int32_t> p2(ids.begin(), ids.begin() + 8192);
    p2 = p2 + text(40, 100);
    const int64_t from = c.ask(p2);
    check(from > 0 && from >= 8192 / 3, "a shorter rewrite still salvages the common prefix");
    const size_t len2 = p2.size();
    p2 = p2 + text(41, 50);
    const int64_t again = c.ask(p2);
    check(again == (int64_t) len2, "the salvaged conversation goes on from its own end");
}

// A divergence below the first rung cannot be salvaged: the bank is parked, re-read from scratch and dropped
// as superseded - the old silent behaviour.  The diagnostic line now says so, with the session id.
void head_divergence_says_so() {
    CacheOptions o = options(64, 4);
    o.checkpoints = 10;
    o.log = std::fopen("ladder_head_log.txt", "w");
    Chat c(o);
    std::vector<int32_t> ids = text(1, 61000);
    c.kept.set_pending_session("S9");
    c.ask(ids);
    std::vector<int32_t> p2(ids.begin(), ids.begin() + 1000);   // below every rung the ladder keeps
    p2 = p2 + text(42, 200);
    c.kept.set_pending_session("S9");
    const int64_t from = c.ask(p2);
    check(from == 0, "a divergence below the first rung cannot be salvaged");
    if (o.log) { std::fflush(o.log); std::fclose(o.log); }
    const std::string log = log_text("ladder_head_log.txt");
    check(log.find("prompt shares 1000 tokens with a kept bank") != std::string::npos, "the scratch diagnostic names the shared prefix");
    check(log.find("(session S9)") != std::string::npos, "the scratch diagnostic names the session");
    check(log.find("dropped 1 superseded banks of session S9") != std::string::npos, "the unsalvageable bank is still dropped as superseded");
}

// A bank parked with only a prefix of its rows banked: the salvage must not come back past the banked rows -
// the marks beyond them have no rows behind them, and a read resumed at one would attend over a hole (the
// banked prefix ends, the mark is beyond it, the tokens in between were never re-read).  The cap is
// reach()'s: a parked bank is reachable no further than its banked rows go.
void partial_bank_capped() {
    CacheOptions o = options(1, 4);   // 1 MiB: an 80000-token bank (1.22 MiB) parks only partially
    o.park_min = 0;
    Chat c(o);
    std::vector<int32_t> a = text(1, 80000);
    c.ask(a);
    std::vector<int32_t> b = text(9, 400);
    b[0] = a[0] == 49999 ? 0 : a[0] + 1;   // starts unlike: a parks whole, no salvage muddies the test
    c.ask(b);
    // a's bank: the RAM tier banks 65536 tokens (8 chunks of the 1 MiB budget); the marks beyond that have
    // no rows behind them.  A probe past the banked rows must come back at or below 65536, not at a mark
    // in (65536, 78000] - at such a mark the restore would leave rows 65536..mark unread by anyone.
    std::vector<int32_t> probe(a.begin(), a.begin() + 78000);
    probe = probe + text(21, 300);
    const int64_t from = c.ask(probe);
    check(from > 0 && from <= 65536, "the salvage stops at the banked rows (no hole past them)");
}

// The server's runtime settings (the CACHE line): set_limits() applies new budgets between requests and
// the next park uses them.  The values are clamped at the setter, so a malformed server message cannot
// make the budgets negative and kill parking silently.
void runtime_limits_apply() {
    CacheOptions o = options(0, 4);   // no park budget yet
    o.park_min = 0;
    Chat c(o);
    std::vector<int32_t> a = text(1, 40000);
    c.ask(a);
    std::vector<int32_t> b = text(9, 400);
    b[0] = a[0] == 49999 ? 0 : a[0] + 1;   // starts unlike: nothing to salvage, the switch parks a
    c.ask(b);
    check(c.kept.parked() == 0, "mib 0: nothing parks");
    c.kept.set_limits(-5, -1, true, -3, -2);   // the CACHE line's values, clamped at the setter
    std::vector<int32_t> d = text(5, 400);
    d[0] = a[0] == 49999 ? 0 : a[0] + 1;
    c.ask(d);
    check(c.kept.parked() == 0, "negative values clamp to 0: still no parking");
    c.kept.set_limits(2, 0, false, 4, 0);   // the CACHE line: mib=2
    std::vector<int32_t> e = text(7, 400);
    e[0] = a[0] == 49999 ? 0 : a[0] + 1;
    c.ask(e);
    check(c.kept.parked() == 1, "mib raised at runtime: the next park keeps the bank");
}

// The disk tier completes what the RAM tier leaves: a bank bigger than the RAM budget parks in RAM up to
// the budget and the rest goes to disk in the same park, so the bank is whole and the probe salvages deep.
void park_tail_goes_to_disk() {
    CacheOptions o = options(1, 4);
    o.disk_mib = 3;
    o.park_min = 0;
    o.log = std::fopen("disk_tail_log.txt", "w");
    Chat c(o);
    std::vector<int32_t> a = text(1, 80000);
    c.ask(a);
    std::vector<int32_t> b = text(9, 400);
    b[0] = a[0] == 49999 ? 0 : a[0] + 1;
    c.ask(b);   // parks a: RAM banks its budget's worth, the disk tier banks the rest of the same park
    check(c.kept.parked() == 1, "the bank is parked, not wiped");
    std::vector<int32_t> probe(a.begin(), a.begin() + 78000);
    probe = probe + text(21, 300);
    const int64_t from = c.ask(probe);
    check(from > 0, "the whole bank salvages (the tail the RAM refused is on disk)");
    check(c.kept.evictions() == 0, "no bank was evicted to make the room");
    if (o.log) { std::fflush(o.log); std::fclose(o.log); }
    const std::string log = log_text("disk_tail_log.txt");
    check(log.find(" to disk") != std::string::npos, "the disk tier's park says so in its report line");
}

// The RAM budget full of parked banks: a new bank's park goes to the disk tier whole instead of evicting
// the neighbours - the tier's promise is that banks are not wiped to make room for chunks of others.
void disk_saves_when_ram_refuses() {
    CacheOptions o = options(1, 4);
    o.disk_mib = 3;
    o.park_min = 0;
    o.log = std::fopen("disk_refuse_log.txt", "w");
    Chat c(o);
    std::vector<int32_t> a = text(1, 40000), b = text(2, 40000), d = text(3, 40000);
    c.ask(a);
    c.ask(b);   // parks a: 0.61 MiB in RAM, fits
    c.ask(d);   // parks b: the RAM has 0.39 MiB left, b parks on it plus disk for the tail
    std::vector<int32_t> e = text(77, 400);
    e[0] = a[0] == 49999 ? 0 : a[0] + 1;
    c.ask(e);   // parks d: the RAM budget is spent (a+b hold 1.22 MiB of the 1 MiB budget) - d goes to disk
    check(c.kept.parked() == 3, "three banks parked");
    check(c.kept.evictions() == 0, "none of them was evicted for another's chunk");
    check(c.ask(a) > 0, "a salvages");
    check(c.ask(b) > 0, "b salvages");
    check(c.ask(d) > 0, "d salvages (its bank is on disk)");
    if (o.log) { std::fflush(o.log); std::fclose(o.log); }
    const std::string log = log_text("disk_refuse_log.txt");
    check(log.find(" to disk") != std::string::npos, "the disk tier's parks say so");
}

// The auto disk budget: a park grows disk_mib toward the promise (park_target banks parkable in RAM plus
// disk), capped; the growth is reported once, in the log.
void auto_grows_disk_budget() {
    CacheOptions o = options(2, 4);
    o.disk_auto = true;
    o.park_min = 0;
    o.log = std::fopen("disk_grow_log.txt", "w");
    Chat c(o);
    std::vector<int32_t> a = text(1, 400000);   // 6.1 MiB bank: the RAM side banks 2 MiB of it
    c.ask(a);
    std::vector<int32_t> b = text(9, 400);
    b[0] = a[0] == 49999 ? 0 : a[0] + 1;
    c.ask(b);   // parks a: grow_disk() sets the budget to 6 banks of 7 MiB minus the 2 MiB park = 40 MiB
    if (o.log) { std::fflush(o.log); std::fclose(o.log); }
    const std::string log = log_text("disk_grow_log.txt");
    check(log.find("disk budget raised to 40 MiB") != std::string::npos, "the budget grew to the promise");
    check(log.find("the biggest bank 7 MiB") != std::string::npos, "the report names the bank that set it");

    // The cap holds the budget down.
    CacheOptions o2 = options(2, 4);
    o2.disk_auto = true;
    o2.disk_cap_mib = 8;
    o2.park_min = 0;
    o2.log = std::fopen("disk_cap_log.txt", "w");
    Chat c2(o2);
    std::vector<int32_t> a2 = text(1, 400000);
    c2.ask(a2);
    std::vector<int32_t> b2 = text(9, 400);
    b2[0] = a2[0] == 49999 ? 0 : a2[0] + 1;
    c2.ask(b2);
    if (o2.log) { std::fflush(o2.log); std::fclose(o2.log); }
    const std::string log2 = log_text("disk_cap_log.txt");
    check(log2.find("disk budget raised to 8 MiB") != std::string::npos, "the cap holds the budget down");
}

// The idle pre-park when the host sits at its floor (no headroom for even one chunk): the disk tier banks
// a budget-sized chunk anyway, so the banked copy advances instead of waiting for a squeeze that never ends.
void pre_park_uses_disk_when_host_dry() {
    CacheOptions o = options(2, 4);
    o.disk_mib = 4;
    o.park_min = 0;
    o.pre_park_floor_mib = 1;
    o.host_available = [] { return std::optional<uint64_t>(uint64_t(0)); };
    o.log = std::fopen("disk_prepark_log.txt", "w");
    Chat c(o);
    std::vector<int32_t> a = text(1, 40000);
    c.ask(a);
    check(c.kept.pre_park(), "the pre-park goes on through the disk tier");
    check(c.kept.pre_park(), "and again: the banked copy advances while the host stays dry");
    if (o.log) { std::fflush(o.log); std::fclose(o.log); }
    const std::string log = log_text("disk_prepark_log.txt");
    check(log.find("pre-parked") != std::string::npos && log.find(" to disk") != std::string::npos,
          "the dry-host pre-park reports its disk tier");
}

}  // namespace

int main() {
    drop_for_ram_basic();
    drop_for_ram_pressure();
    drop_for_ram_default_off();
    drop_for_ram_unknown_host();
    adopt_sets_used_for_ttl();
    drop_for_ram_wanted_skips();
    drop_all_no_parked();
    drop_all_three_parked();
    drop_all_log_line();
    drop_all_cur_spared();
    rewrite_drop_basic();
    current_bank_survives();
    different_session_bank_survives();
    untagged_flow_unchanged();
    wanted_survives_ttl();
    wanted_survives_eviction();
    wanted_is_session_scoped();
    ladder_rungs_probed();
    parked_bank_salvage();
    rewrite_flow_salvages();
    shrink_salvages();
    head_divergence_says_so();
    partial_bank_capped();
    park_tail_goes_to_disk();
    disk_saves_when_ram_refuses();
    auto_grows_disk_budget();
    pre_park_uses_disk_when_host_dry();
    runtime_limits_apply();
    std::printf("conversations_ram_test: %d checks passed\n", checks);
    return 0;
}
