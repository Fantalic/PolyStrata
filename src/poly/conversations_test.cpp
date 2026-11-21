// src/poly/conversations_test.cpp - Conversations against an engine whose state can be checked: after every
// request the engine must hold exactly what reading the prompt from its first token gives.
//
//   g++ -std=c++20 src/poly/conversations_test.cpp src/poly/spill.cpp -o conversations_test && ./conversations_test
#include "conversations.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

using strata::poly::CacheOptions;
using strata::poly::Conversations;

namespace {

int checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

uint64_t mix(uint64_t h, int32_t t) { return (h ^ (uint64_t) (uint32_t) t) * 0x9E3779B97F4A7C15ull + 0x7F4A7C15ull; }

// The recurrent state is a hash of the tokens read.  A token leaves two rows, as the real engine's layers do: the
// hash after it, and the token.
struct Fake {
    static constexpr int64_t ctx = 1 << 16;
    uint64_t h = 1;
    int64_t at = 0, read = 0;
    size_t state = 8;   // bytes a checkpoint takes; the hash is the first 8
    std::vector<uint64_t> a = std::vector<uint64_t>(ctx), b = std::vector<uint64_t>(ctx);
    mutable int save_rows_calls = 0;     // how many times save_rows was called
    mutable size_t save_rows_bytes = 0;  // total bytes written by save_rows

    void reset() { h = 1, at = 0; }
    void eval(const int32_t* t, int64_t n) {
        for (int64_t i = 0; i < n; ++i, ++at, ++read) {
            a[(size_t) at] = h = mix(h, t[i]);
            b[(size_t) at] = (uint64_t) t[i];
        }
    }
    size_t state_bytes() const { return state; }
    void save_state(uint8_t* dst) const { std::memcpy(dst, &h, 8); }
    void load_state(const uint8_t* src, int64_t pos) { std::memcpy(&h, src, 8), at = pos; }
    size_t row_bytes() const { return 16; }
    void save_rows(int64_t first, int64_t n, uint8_t* dst) const {
        ++save_rows_calls;
        save_rows_bytes += (size_t) n * row_bytes();
        std::memcpy(dst, &a[(size_t) first], (size_t) n * 8);
        std::memcpy(dst + n * 8, &b[(size_t) first], (size_t) n * 8);
    }
    void load_rows(const uint8_t* src, int64_t from, int64_t count, int64_t first, int64_t n) {
        std::memcpy(&a[(size_t) first], src + (first - from) * 8, (size_t) n * 8);
        std::memcpy(&b[(size_t) first], src + count * 8 + (first - from) * 8, (size_t) n * 8);
    }

    bool holds(const std::vector<int32_t>& ids) const {
        uint64_t x = 1;
        for (size_t i = 0; i < ids.size(); ++i)
            if (a[i] != (x = mix(x, ids[i])) || b[i] != (uint64_t) ids[i]) return false;
        return at == (int64_t) ids.size() && h == x;
    }
};

// Above zero: the prompts are read this many tokens at a time, past the checkpoints inside (inside()), as an
// engine does that reads several calls together.  Zero: a call at a time, up to the next checkpoint.
int64_t far = 0;
// Above zero: one such read in so many is given up part way (rewind()), and the request is made again.
unsigned quits = 0;

struct Chat {
    Fake e;
    Conversations<Fake> kept;
    std::mt19937 rng{7};
    std::vector<std::pair<int64_t, uint8_t*>> due;
    explicit Chat(const CacheOptions& o) : kept(e, o) {}

    // A request as the server makes it: the prompt, where its last turn begins, then `reply` generated tokens.
    // Returns where the prompt was taken up, and leaves the tokens the engine then holds in `ids`.
    int64_t ask(std::vector<int32_t>& ids, int reply = 3, int64_t turn = -1) {
        const int64_t n = (int64_t) ids.size(), from = kept.resume(ids);
        kept.turn_at(turn);
        check(from >= 0 && from < n && e.at == from, "a prompt is taken up before its last token");
        for (int64_t i = from; i < n;) {
            const bool last = i == n - 1;
            const int64_t room = std::min<int64_t>(n - 1 - i, far > 0 ? far : 100);
            const int64_t m = last ? 1 : far > 0 ? room : kept.until_checkpoint(i, room);
            kept.before(i, last);
            if (far > 0 && !last && quits > 0 && rng() % quits == 0) {
                // the engine got part of the way, a state among the ones due written or not, and stopped
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
                for (const auto& d : due) {   // the engine writes a state when it has read up to it
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
            const int32_t t = 1000000 + (int32_t) (rng() % 1000);
            e.eval(&t, 1);
            kept.read(&t, 1);
            ids.push_back(t);
        }
        check(e.holds(ids) && kept.seq() == ids, "the engine holds the prompt and the reply");
        kept.relieve();   // between requests, as the server does: with the shelf's RAM cap set, what comes back is read from the file cache
        return from;
    }
};

std::vector<int32_t> text(int seed, int n) {
    std::vector<int32_t> v((size_t) n);
    std::mt19937 r((unsigned) seed);
    for (int32_t& t : v) t = (int32_t) (r() % 50000);
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

void one_conversation() {
    Chat c(options(0));
    std::vector<int32_t> a = text(1, 5000);
    check(c.ask(a) == 0, "a first prompt is read from its start");
    const std::vector<int32_t> turn1 = a;
    a = a + text(2, 300);
    check(c.ask(a) == (int64_t) turn1.size(), "a prompt that goes on from the reply goes on from the engine");

    // the client sends the reply back in other tokens: back to the checkpoint before the first prompt's last token
    std::vector<int32_t> b(turn1.begin(), turn1.begin() + 5000);
    b = b + text(3, 400);
    check(c.ask(b) == 4999, "a rewritten reply costs the reply");

    // an edit far back: the last checkpoint before it
    std::vector<int32_t> d(b.begin(), b.begin() + 3000);
    d = d + text(4, 100);
    check(c.ask(d) == 2048, "an edit goes back to the checkpoint before it");
    check(c.kept.parked() == 0, "nothing is parked without a budget");

    std::vector<int32_t> other = text(5, 4000);
    check(c.ask(other) == 0, "another conversation starts from nothing");
    check(c.ask(d) == 0, "and without a budget the first one is gone");
}

void two_conversations() {
    Chat c(options(64));
    std::vector<int32_t> a = text(1, 9000), b = text(2, 3000);
    c.ask(a);
    c.ask(b);
    check(c.kept.parked() == 1, "the first conversation is parked");
    a = a + text(3, 200);
    const int64_t before = c.e.read;
    check(c.ask(a) == 8999, "it comes back at the end of its prompt");
    check(c.e.read - before == 3 + 200 + 1 + 3, "and only the reply and the new turn are read");
    b = b + text(4, 100);
    check(c.ask(b) == 2999, "so does the second");

    // a side request too short to be worth a slot
    std::vector<int32_t> title = text(6, 300);
    c.ask(title);
    a = a + text(7, 50);
    check(c.ask(a) > 9000, "a short request between two turns costs nothing");
    check(c.kept.parked() == 1, "and is not kept");
}

void shared_start() {
    Chat c(options(64));
    const std::vector<int32_t> sys = text(1, 5000);
    std::vector<int32_t> a = sys + text(2, 6000), sub = sys + text(3, 2500);
    c.ask(a);
    check(c.ask(sub) == 4096, "a prompt with the same start takes it from the checkpoint inside");
    check(c.kept.parked() == 1, "and the first conversation is kept whole");
    sub = sub + text(4, 100);
    check(c.ask(sub) == 7503, "the second goes on from where the engine is");
    a = a + text(5, 100);
    check(c.ask(a) == 10999, "and so does the first");
    std::vector<int32_t> sub2 = sys + text(6, 2500);
    check(c.ask(sub2) == 4096, "a third shares the start again");
    check(c.kept.parked() == 2, "with both others parked");
}

// The server says where a prompt's last turn begins.  The chat's next request has the reply in other tokens than
// the ones the prompt ended with after the turn's header (a template drops the reply's thinking): it goes on from
// the turn, and not from a checkpoint thousands of tokens back.
void last_turn() {
    const std::vector<int32_t> sys = text(1, 5000), head = text(2, 3), think = text(3, 2);
    Chat c(options(64));
    std::vector<int32_t> a = sys + head + think;
    check(c.ask(a, 3, 5000) == 0, "a first prompt is read from its start");
    std::vector<int32_t> b = sys + head + text(4, 300) + head + think;
    check(c.ask(b, 3, 5303) == 5000, "the next turn goes on from where the last one began");
    std::vector<int32_t> d = sys + head + text(4, 300) + head + text(5, 200) + head + think;
    check(c.ask(d, 3, 5506) == 5303, "and so does the one after it");

    Chat e(options(64));
    a = sys + head + think;
    e.ask(a);
    b = sys + head + text(4, 300) + head + think;
    check(e.ask(b) == 4096, "without the place, the checkpoint before the last token is past where they differ");
}

void limits() {
    // two slots: a third conversation pushes out the one not used for longest
    Chat c(options(64, 2));
    std::vector<int32_t> v[4] = {text(1, 3000), text(2, 3000), text(3, 3000), text(4, 3000)};
    for (auto& x : v) c.ask(x);
    check(c.kept.parked() == 2 && c.kept.evictions() == 1, "the slots bound what is parked");
    v[0] = v[0] + text(5, 10);
    check(c.ask(v[0]) == 0, "the pushed-out conversation is read again");
    v[2] = v[2] + text(6, 10);
    check(c.ask(v[2]) == 2999, "a kept one is not");

    // a budget of 1 MiB with checkpoints of 300 KiB: a parked conversation holds one checkpoint and its rows
    Chat d(options(1, 8));
    d.e.state = 300 << 10;
    std::vector<int32_t> w[4] = {text(1, 1500), text(2, 1500), text(3, 1500), text(4, 1500)};
    for (auto& x : w) d.ask(x);
    check(d.kept.parked_bytes() <= (1u << 20) && d.kept.parked() == 3, "the budget bounds what is parked");
    for (auto& x : w) d.ask(x = x + text(9, 5));

    // the host says no
    CacheOptions o = options(64);
    o.admit = [](size_t) { return false; };
    Chat e(o);
    std::vector<int32_t> p = text(1, 3000), q = text(2, 3000);
    e.ask(p);
    e.ask(q);
    check(e.kept.parked() == 0, "nothing is parked when the host has no memory to spare");
}

using Restored = Conversations<Fake>::Restored;

std::vector<uint8_t> saved(Chat& c) {
    std::vector<uint8_t> f;
    check(c.kept.save(1000, [&](const void* p, size_t n) {
              f.insert(f.end(), (const uint8_t*) p, (const uint8_t*) p + n);
              return true;
          }),
          "a conversation is saved");
    check(f.size() == c.kept.save_bytes(), "in the bytes it said it takes");
    return f;
}

// Restores f, of which only the first `have` bytes can be read.
Restored restored(Chat& c, const std::vector<uint8_t>& f, size_t have = SIZE_MAX, int64_t most = Fake::ctx) {
    size_t at = 0;
    return c.kept.restore(f.size(), most, 1000, [&](void* p, size_t n) {
        if (n > std::min(have, f.size()) - at) return false;
        std::memcpy(p, f.data() + at, n);
        at += n;
        return true;
    });
}

// A session file: a conversation leaves one engine as bytes and takes another, with its checkpoints.
void session_files() {
    Chat a(options(0));
    std::vector<int32_t> ids = text(1, 5000);
    a.ask(ids);
    const std::vector<uint8_t> f = saved(a);
    check(a.e.holds(ids) && a.kept.seq() == ids, "saving leaves the conversation as it was");

    Chat b(options(0));
    check(restored(b, f) == Restored::ok && b.e.holds(ids) && b.kept.seq() == ids, "another engine takes it whole");
    std::vector<int32_t> next = ids + text(2, 300);
    const int64_t before = b.e.read;
    check(b.ask(next) == (int64_t) ids.size(), "and the next turn goes on from its end");
    check(b.e.read - before == 300 + 3, "reading the new turn and the reply only");

    Chat c(options(0));
    restored(c, f);
    std::vector<int32_t> edit(ids.begin(), ids.begin() + 4500);
    edit = edit + text(3, 100);
    check(c.ask(edit) == 4096, "an edit goes back to a checkpoint that came with it");
    Chat c2(options(0));
    restored(c2, f);
    std::vector<int32_t> early(ids.begin(), ids.begin() + 3000);
    early = early + text(11, 100);
    check(c2.ask(early) == 0, "and one before the two checkpoints a file has is read from the start");

    Chat d(options(64));
    std::vector<int32_t> mine = text(4, 3000);
    d.ask(mine);
    check(restored(d, f) == Restored::ok && d.e.holds(ids) && d.kept.parked() == 1, "the one in the engine is parked");
    mine = mine + text(5, 10);
    check(d.ask(mine) == 2999, "and comes back");
    next = ids + text(6, 20);
    check(d.ask(next) == (int64_t) ids.size(), "and so does the restored one, parked in its turn");

    Chat e(options(64));
    std::vector<int32_t> held = text(7, 2000);
    e.ask(held);
    std::vector<uint8_t> g = f;
    g.pop_back();
    check(restored(e, g) == Restored::other, "bytes that do not add up are refused");
    check(restored(e, f, SIZE_MAX, 5000) == Restored::other, "and a conversation longer than the context");
    e.e.state = 16;
    check(restored(e, f) == Restored::other, "and a file of an engine with another state");
    check(e.e.holds(held) && e.kept.seq() == held && e.kept.parked() == 0, "a refused file changes nothing");

    Chat h(options(64));
    std::vector<int32_t> was = text(8, 3000);
    h.ask(was);
    check(restored(h, f, f.size() - 100) == Restored::cut && h.kept.seq().empty() && h.e.at == 0,
          "a file that ends inside its rows leaves the engine empty");
    was = was + text(9, 10);
    check(h.ask(was) == 2999, "and the conversation it held parked");

    CacheOptions tight = options(64);
    tight.admit = [](size_t) { return false; };
    Chat t(tight);
    std::vector<int32_t> gone = text(10, 3000);
    t.ask(gone);
    check(restored(t, f) == Restored::ok && t.e.holds(ids) && t.kept.parked() == 0,
          "a host with no memory to spare for parking still takes the file");
}

// Pins f, of which only the first `have` bytes can be read.
Restored pinned(Chat& c, const std::vector<uint8_t>& f, size_t have = SIZE_MAX) {
    size_t at = 0;
    return c.kept.pin(f.size(), Fake::ctx, 1000, [&](void* p, size_t n) {
        if (n > std::min(have, f.size()) - at) return false;
        std::memcpy(p, f.data() + at, n);
        at += n;
        return true;
    });
}

// A pinned conversation: a system prompt that every chat starts with, read once and saved.  A new chat goes on
// from its end whatever took the engine since, and neither the slots nor the budget nor a long chat push it out.
void pinned_prompt() {
    const std::vector<int32_t> sys = text(1, 3000);
    Chat maker(options(0));
    std::vector<int32_t> ids = sys;
    maker.ask(ids, 0);
    const std::vector<uint8_t> f = saved(maker);

    for (const int64_t budget : {0, 64}) {
        Chat c(options(budget, 2));
        check(pinned(c, f) == Restored::ok && c.kept.pinned_tokens() == 3000 && c.e.at == 0 && c.e.read == 0,
              "a file is pinned without touching the engine");
        std::vector<int32_t> q = sys + text(2, 50);
        check(c.ask(q) == 3000 && c.e.read == 50 + 3, "a chat that starts with it reads its own tokens only");
        q = sys + text(3, 60);
        check(c.ask(q) == 3000, "and so does the next new chat");
        for (int k = 0; k < 4; ++k) {
            std::vector<int32_t> other = text(10 + k, 9000);
            c.ask(other);
        }
        q = sys + text(4, 70);
        check(c.ask(q) == 3000, "after other conversations took the engine and every slot, a new chat still does");
        check(c.kept.parked() <= 2 && c.kept.parked_bytes() <= ((size_t) budget << 20),
              "it is outside the slots and the budget");
        std::vector<int32_t> big = sys + text(5, 30000);
        c.ask(big);
        q = sys + text(6, 10);
        check(c.ask(q) == 3000, "after a long chat that began from it, the next one starts there again");
        std::vector<int32_t> part(sys.begin(), sys.begin() + 2500);
        part = part + text(7, 10);
        check(c.ask(part) == 2048, "a prompt that leaves it inside takes its checkpoint before that place");
        q = sys + text(8, 5);
        check(c.ask(q) == 3000, "and it is whole for the chat after that");
    }

    Chat d(options(64));
    check(pinned(d, f, f.size() - 100) == Restored::cut && d.kept.pinned_tokens() == 0, "a file cut short pins nothing");
    std::vector<int32_t> q = sys + text(9, 5);
    check(d.ask(q) == 0, "and the prompt is read from its start");
}

// Any order of prompts over conversations that share starts, under any options: the engine always ends up
// holding the prompt.  Now and then the conversation is saved as a session file, and one saved earlier comes back.
// Half the rounds have a pinned conversation, which some prompts start with.
void any_order() {
    std::mt19937 rng(11);
    const int64_t every[] = {0, 64, 256, 1000}, budget[] = {0, 1, 64};
    for (int round = 0; round < 60; ++round) {
        CacheOptions o;
        o.checkpoints = (int) (rng() % 5);
        o.every = every[rng() % 4];
        o.park_mib = budget[rng() % 3];
        o.park_slots = (int) (rng() % 4);
        o.park_min = rng() % 2 ? 200 : 0;
        Chat c(o);
        c.e.state = rng() % 2 ? 100 << 10 : 8;
        c.rng.seed((unsigned) round);
        std::vector<std::vector<int32_t>> chats;
        std::vector<std::vector<uint8_t>> files;
        std::vector<int32_t> sys;
        if (rng() % 2) {
            Chat maker(o);
            maker.e.state = c.e.state;
            sys = text((int) rng(), 300 + (int) (rng() % 3000));
            std::vector<int32_t> read = sys;
            maker.ask(read, 0);
            check(pinned(c, saved(maker)) == Restored::ok, "a conversation is pinned");
            chats.push_back(sys);
        }
        for (int step = 0; step < 40; ++step) {
            if (!c.kept.seq().empty() && rng() % 6 == 0) {
                files.push_back(saved(c));
                check(restored(c, files[rng() % files.size()]) == Restored::ok && c.e.holds(c.kept.seq()),
                      "a session file comes back whole");
            }
            std::vector<int32_t> ids;
            const unsigned what = rng() % 10;
            if (chats.empty() || what == 0) {
                ids = text((int) rng(), 1 + (int) (rng() % 1500));
            } else {
                ids = chats[rng() % chats.size()];
                if (what < 4) ids.resize(1 + rng() % ids.size());   // goes on from somewhere inside
                ids = ids + text((int) rng(), (int) (rng() % 400));
            }
            const int64_t turn = rng() % 2 ? (int64_t) (rng() % ids.size()) : -1;
            c.ask(ids, (int) (rng() % 5), turn);
            chats.push_back(ids);
            check(c.kept.parked() <= (size_t) std::max(o.park_slots, 0), "no more parked than slots");
            check(c.kept.parked_bytes() <= ((size_t) o.park_mib << 20), "no more parked than the budget");
        }
    }
}

// pre_park copies rows to host memory without cutting the slot: a continuing request finds the engine at its live
// position (the full sequence length), not at a checkpoint.
void pre_park_keeps_live_position() {
    Chat c(options(64));
    std::vector<int32_t> a = text(1, 5000);
    c.ask(a);  // engine has 5003 tokens (5000 prompt + 3 reply)
    check(c.kept.pre_park(), "pre_park succeeds with budget");
    check(c.kept.seq().size() == 5003, "pre_park does not truncate the slot");
    // Continue with the same tokens + more: the engine is at its live position
    std::vector<int32_t> extended = c.kept.seq();
    extended = extended + text(2, 300);
    check(c.ask(extended) == 5003, "resume with extended same-sequence hits engine's live position");
}

// After pre_park, a switch parks the conversation (already pre-parked, so rows are in the blob).  Coming back to it
// restores the rows from the pre-parked blob.  The fake engine tracks save_rows calls/bytes to verify.
void pre_parked_rows_restore_on_switch() {
    Chat c(options(64));
    std::vector<int32_t> a = text(1, 5000);
    c.ask(a);  // engine has 5003 tokens
    c.kept.pre_park();  // save rows, no cut
    const int saved_calls = c.e.save_rows_calls, saved_bytes = (int) c.e.save_rows_bytes;
    // Switch to another conversation - parks a: the rows are already in the pre-parked blob, so the park copies
    // nothing new
    std::vector<int32_t> b = text(99, 3000);
    c.ask(b);  // switches to b, parks a
    check(c.e.save_rows_calls == saved_calls, "the switch's park called save_rows no more than pre_park did");
    check((int) c.e.save_rows_bytes == saved_bytes, "the switch's park copied no new bytes (already pre-parked)");
    // Come back to a: it was parked at its last checkpoint, so it is taken up there
    std::vector<int32_t> a2 = a;
    const int64_t from = c.ask(a2);
    check(from == 4999, "restoring a goes to its last checkpoint");
    check(c.e.holds(a2), "the engine holds the restored conversation");
    // Continue with the same conversation: it was pre-parked, never cut, so the engine still holds it whole and
    // the extension goes on from where the last ask left it
    a2 = a2 + text(2, 300);
    const int64_t live = (int64_t) c.kept.seq().size();
    check(c.ask(a2) == live, "continuing a goes on from the engine's live position");
    // Switch again - parks a again, and now the rows read since the pre-park are new
    std::vector<int32_t> d = text(88, 2000);
    c.ask(d);  // switches, parks a
    check(c.e.save_rows_bytes > (size_t) saved_bytes, "the second park copied the rows read since the pre-park");
}

// pre_park respects the budget: park_mib too small (0) -> returns false, conversation untouched.
void pre_park_respects_budget() {
    CacheOptions o = options(0);
    o.park_mib = 0;  // no budget
    Chat c(o);
    std::vector<int32_t> a = text(1, 5000);
    c.ask(a);
    check(!c.kept.pre_park(), "pre_park fails when park_mib is 0");
    check(c.kept.seq().size() == 5003, "conversation is untouched after failed pre_park");
    // Verify that the resume behavior still works (same as existing tests)
    a = a + text(2, 300);
    check(c.ask(a) == 5003, "a failed pre_park leaves the conversation live, so the extension resumes from it");
}

// pre_park with a small budget saves bounded chunks (~1/8 of park_mib per tick) so a big conversation's parked copy
// advances incrementally instead of one too-big blob that the host's free-RAM gate refuses.  With park_mib = 1 the
// cap is (1 MiB << 20) / 8 / 16 = 8192 tokens at the harness's row_bytes.  The conversation is driven past 2x the
// cap so the first pre_park saves only ~8192 tokens, the next continues from there, and once coverage reaches
// marks.back(), park() adds 0 new bytes.  A follow-up ask on the same prefix reuses the full covered prefix.
void pre_park_bounded_chunks() {
    CacheOptions o = options(1);  // park_mib = 1, cap = 8192 tokens at row_bytes 16
    Chat c(o);

    // First prompt: 5000 + 3 reply = 5003, checkpoints at 2048, 4096
    std::vector<int32_t> a = text(1, 5000);
    c.ask(a);

    // Second prompt: extend to 20106 (5003 + 15100 + 3 reply), new checkpoint at 20106
    a = a + text(2, 15100);
    c.ask(a);
    check(c.kept.seq().size() == 20106, "conversation is 20106 tokens (5000 + 3 + 15100 + 3)");

    // First pre_park: saves only up to cap (8192 tokens = 131072 bytes)
    check(c.kept.pre_park(), "first pre_park succeeds");
    check(c.kept.seq().size() == 20106, "pre_park does not truncate the slot");
    check((int) c.e.save_rows_bytes == 131072, "first pre_park saves ~8192 tokens (cap * row_bytes)");

    // Second pre_park: continues from 8192, saves another cap-sized chunk (8192 tokens)
    a = a + text(3, 10000);  // 20106 + 10000 = 30106 prompt tokens + 3 reply = 30109
    c.ask(a);
    check(c.kept.seq().size() == 30109, "extended conversation is 30109 tokens (30106 prompt + 3 reply)");
    check(c.kept.pre_park(), "second pre_park succeeds");
    const size_t after_second = c.e.save_rows_bytes;
    check(after_second - (size_t) 131072 == 131072, "second pre_park also saves cap (8192 * 16) bounded chunk");

    // Third pre_park: continues from 16384, saves another cap chunk
    check(c.kept.pre_park(), "third pre_park succeeds");
    const size_t after_third = c.e.save_rows_bytes;
    check(after_third - after_second == 131072, "third pre_park also saves cap (8192 * 16) bounded chunk");

    // Fourth pre_park: continues from 24576, saves remaining to marks.back().pos (30105)
    check(c.kept.pre_park(), "fourth pre_park succeeds");
    const size_t after_fourth = c.e.save_rows_bytes;
    check(after_fourth - after_third == 88464, "fourth pre_park saves remaining (30105-24576)*16 tokens");

    // Once coverage reaches marks.back(), park() adds 0 new bytes
    std::vector<int32_t> b = text(99, 3000);
    c.ask(b);  // switches to b, parks a
    check(c.e.save_rows_bytes == after_fourth, "park() adds 0 new bytes when coverage == marks.back().pos");

    // Come back to a: it resumes at its last checkpoint (30105) - the covered prefix is reused
    std::vector<int32_t> a2 = a;
    check(c.ask(a2) == 30105, "switch-back resumes at the last checkpoint (marks.back().pos)");
    check((int64_t) c.kept.seq().size() == 30112, "switch-back holds the prompt (30109) + 3 reply tokens");
}

// A full shelf (park_slots parked conversations) plus the live one: the idle pre-park saves into the live
// conversation's own slot, so it takes no shelf slot and must succeed even with the shelf full.  Counting the
// live one made every pre-park refuse at once in that state - which opencode always lives in - and the switch
// then did all the saving one-shot.
void pre_park_full_shelf() {
    Chat c(options(64));   // 64 MiB: plenty for five 4000-token conversations at 16 bytes a row
    std::vector<std::vector<int32_t>> conv;
    for (int i = 0; i < 5; ++i) conv.push_back(text(10 + i, 4000));
    c.ask(conv[0]);
    c.ask(conv[1]);
    c.ask(conv[2]);
    c.ask(conv[3]);
    check(c.kept.parked() == 3, "three conversations are parked while the fourth is live");
    c.ask(conv[4]);   // the switch parks the fourth: the shelf is full, the fifth is live
    check(c.kept.parked() == 4, "the shelf is full with the fifth conversation live");

    const size_t before = c.e.save_rows_bytes;
    check(c.kept.pre_park(), "pre-park succeeds with a full shelf");
    check(c.e.save_rows_bytes > before, "the live conversation's pre-park saved its rows");

    // Back to the first: the fifth parks, and the pre-park did most of its saving - the switch adds nearly nothing.
    const size_t at_switch = c.e.save_rows_bytes;
    c.ask(conv[0]);
    check(c.e.save_rows_bytes - at_switch < 128, "the switch's park adds nearly nothing after the pre-park");
    check(c.kept.parked() == 4, "the shelf stays full after the switch");
}

// When the host refuses the switch's park, the least recently used parked copy is evicted for it - the leaving
// conversation always gets to park, so its engine copy is not destroyed.  The newest parked copy is spared (a
// stale RAM reading cannot wipe the shelf), and the idle pre-park never evicts: refused, it waits for the next
// tick instead.
void park_evicts_when_host_refuses() {
    int refuses = 0;
    CacheOptions o = options(4);
    o.admit = [&refuses](size_t) {
        if (refuses) { --refuses; return false; }
        return true;
    };
    Chat c(o);

    // Four conversations, one after another: a, b, c park one by one, d holds the engine.
    std::vector<int32_t> a = text(1, 5000);
    c.ask(a);
    std::vector<int32_t> b = text(2, 5000);
    check(c.ask(b) == 0, "b starts fresh");
    std::vector<int32_t> c3 = text(3, 5000);
    check(c.ask(c3) == 0, "c starts fresh");
    std::vector<int32_t> d = text(4, 5000);
    check(c.ask(d) == 0, "d starts fresh");
    check(c.kept.evictions() == 0, "the host allowed every park so far");

    // The host refuses d's park once: a, the least recently used parked copy, is evicted for it, and d parks.
    refuses = 1;
    std::vector<int32_t> e = text(5, 5000);
    check(c.ask(e) == 0, "e starts fresh");
    check(c.kept.evictions() == 1, "the refused park evicted the least recently used parked copy");

    // The idle pre-park never evicts: refused, it waits for the next tick instead.
    refuses = 1;
    check(!c.kept.pre_park(), "a refused pre-park waits instead of evicting");
    check(c.kept.evictions() == 1, "the pre-park did not evict");
    check(c.kept.pre_park(), "the next pre-park, with the host willing, saves");

    // Coming back: a re-reads from its start (it was evicted); b and c resume at their checkpoints.
    check(c.ask(a) == 0, "the evicted conversation re-reads from its start");
    check(c.ask(b) > 0, "a parked conversation that stayed resumes at its checkpoint");
    check(c.ask(c3) > 0, "the spared newest parked conversation resumes at its checkpoint");
}

// The idle pre-park sizes each chunk to what the host can spare above the smaller pre-park floor: the budget
// eighth shrinks to the spare, and nothing is saved while the host is at or below the floor.  (host_available is
// stubbed, so the real machine's RAM plays no role; the admits stay unset, so the gates are inert.)
void pre_park_spare_sizing() {
    uint64_t free_bytes = (uint64_t(3) << 20) / 2 + (uint64_t(1) << 20);   // 1.5 MiB free
    CacheOptions o = options(4);   // the budget's eighth is 512 KiB, 32768 tokens at row_bytes 16
    o.pre_park_floor_mib = 1;
    o.host_available = [&free_bytes] { return std::optional<uint64_t>(free_bytes); };
    Chat c(o);

    std::vector<int32_t> a = text(1, 60000);   // longer than any chunk below
    c.ask(a);

    // 1.5 free MiB, floor 1 MiB: the spare is 0.5 MiB = 32768 tokens, the same as the budget eighth
    check(c.kept.pre_park(), "pre-park succeeds while the host has spare above the floor");
    check(c.e.save_rows_bytes == (size_t) 524288, "the chunk is the spare (32768 tokens), never the whole gap");

    // 1.25 free MiB: the spare is 0.25 MiB, so the next chunk is 16384 tokens
    free_bytes = (uint64_t(1) << 20) + (uint64_t(1) << 18);
    check(c.kept.pre_park(), "the next pre-park continues");
    check(c.e.save_rows_bytes == (size_t) 786432, "the chunk shrinks to the smaller spare (16384 tokens)");

    // 1 free MiB, at the floor: nothing is saved
    free_bytes = uint64_t(1) << 20;
    check(!c.kept.pre_park(), "a host at the floor gives nothing");
    check(c.e.save_rows_bytes == (size_t) 786432, "no bytes were saved at the floor");
}

}  // namespace

// A long prompt read in one go has more checkpoints due than a conversation keeps.  Only the ones it keeps get a
// state, and they are the ones that reading up to each checkpoint in turn leaves: where a prompt that goes on
// from each place is taken up says which are there.
void long_read() {
    std::vector<int64_t> left[2];
    const std::vector<int32_t> a = text(1, 31000);
    for (int way = 0; way < 2; ++way) {
        far = way ? 40000 : 0;
        for (int64_t place = 2048; place < 31000; place += 2048) {
            Chat c(options(0));
            c.e.state = 1 << 20;
            std::vector<int32_t> first = a;
            check(c.ask(first) == 0, "a first prompt is read from its start");
            std::vector<int32_t> b(a.begin(), a.begin() + place + 1);
            b = b + text(3, 5);
            left[way].push_back(c.ask(b));
        }
    }
    far = 0;
    check(left[0] == left[1], "a read past its checkpoints keeps the ones a read to each of them keeps");
    const std::vector<int64_t> six = {2048, 2048, 6144,  6144,  6144,  6144, 6144,
                                      6144, 18432, 18432, 18432, 18432, 18432, 28672, 30720};
    check(left[1] == six, "which are the ladder's five inside marks, beside the one before the prompt's last token");
}

// What is kept leaves the heap for a temporary file after a release, and reads back the same, before and after its
// pages leave the process again.  The writer is a thread: the heap goes once it is done and a release follows.
void spill() {
    const size_t n = (size_t(3) << 20) + 123;   // not a whole number of pages
    auto s = std::make_shared<const strata::poly::Spill>(n);
    uint8_t* heap = s->data();
    check(heap != nullptr && s->bytes() == n, "a spill has its bytes");
    for (size_t i = 0; i < n; ++i) heap[i] = (uint8_t) (i * 131 + (i >> 9));
    std::vector<uint8_t> want(heap, heap + n);
    s->release();   // hands it to the writer
    const uint8_t* p = heap;
    for (int k = 0; k < 500 && p == heap; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        s->release();   // frees the heap once it is written
        p = s->data();
    }
    check(p != heap, "the heap is let go once the file holds it");
    check(std::memcmp(p, want.data(), n) == 0, "the file holds the same bytes");
    s->release();   // the pages the read brought back leave the process
    check(std::memcmp(s->data(), want.data(), n) == 0, "and reads back the same after its pages left");
}

// Chunked switch-park, full success: a conversation whose unsaved delta spans several budget-eighth chunks
// (small park_mib, row_bytes 16).  Bank the delta in bounded chunks and assert all rows are banked.
void switch_park_chunked_full() {
    // park_mib = 1, cap = (1 << 20) / 8 / 16 = 8192 tokens per chunk
    CacheOptions o = options(1);
    o.admit = [](size_t) { return true; };  // always allow
    Chat c(o);

    // First prompt: 30000 tokens + 3 reply = 30003 tokens.
    // Checkpoints at 2048, 4096, ..., 28672, 29999 (last token).
    // park() calls cut(s, marks.back().pos=29999) -> seq has 29999 tokens.
    // Delta = 29999 rows * 16 bytes = 479984 bytes. cap = 8192 tokens = 131072 bytes.
    // 479984 / 131072 = 3.66 -> 4 chunks (8192+8192+8192+5425).
    std::vector<int32_t> a = text(1, 30000);
    c.ask(a);

    // Clear counters: we'll measure what the switch parks.
    c.e.save_rows_calls = 0;
    c.e.save_rows_bytes = 0;

    // Switch to a new conversation -> parks a.
    std::vector<int32_t> b = text(2, 1000);
    check(c.ask(b) == 0, "b starts fresh, a is parked");

    // All chunks banked.
    check(c.e.save_rows_calls == 4,
          "park saved in 4 chunks (29999 tokens / 8192 cap)");
    check((int) c.kept.parked_bytes() == 29999 * 16 + 48,
          "parked_bytes == row bytes + 6 checkpoints * 8 bytes");
}

// Partial park: same setup, but admit allows only the first call.  Assert park returns true,
// exactly one chunk banked, the conversation survives in its slot with its prefix.
void switch_park_partial() {
    CacheOptions o = options(1);
    int admit_calls = 0;
    o.admit = [&admit_calls](size_t) {
        if (admit_calls == 0) { ++admit_calls; return true; }
        return false;  // only the first call admits
    };
    Chat c(o);

    // 30000 tokens -> 4 chunks needed, but only the first is admitted.
    std::vector<int32_t> a = text(1, 30000);
    c.ask(a);

    c.e.save_rows_calls = 0;
    c.e.save_rows_bytes = 0;

    // Switch: park tries chunks, first succeeds, second fails -> partial park.
    std::vector<int32_t> b = text(2, 1000);
    check(c.ask(b) == 0, "b starts fresh");

    check(c.e.save_rows_calls == 1, "park saved exactly 1 chunk");
    // parked_bytes includes checkpoint blobs (6 * 8 = 48 bytes); subtract them to get row bytes
    check((int) (c.kept.parked_bytes() - c.kept.parked() * 6 * 8) == 8192 * 16,
          "parked row bytes == one chunk (8192 * 16)");

    // The original conversation survives in its slot.
    // Switch back to a: it resumes at the checkpoint position (29999), not from 0.
    std::vector<int32_t> a2 = a;
    const int64_t from_a = c.ask(a2);
    check(from_a > 0,
          "switch back to a resumes at the checkpoint (not 0)");
    check(from_a == 29999,
          "resume position == the checkpoint (29999)");
}

// Nothing banked: admit always false.  Assert park returns false and the old conversation
// is evicted (matches old skip semantics).
void switch_park_nothing_banked() {
    CacheOptions o = options(1);
    o.admit = [](size_t) { return false; };  // always refuse
    Chat c(o);

    std::vector<int32_t> a = text(1, 30000);
    c.ask(a);

    c.e.save_rows_calls = 0;
    c.e.save_rows_bytes = 0;

    // Switch: park fails, conversation is evicted.
    std::vector<int32_t> b = text(2, 1000);
    check(c.ask(b) == 0, "b starts fresh, a was evicted");
    check(c.kept.parked_bytes() == 0, "no bytes parked");
}

// With focus off (the default), the smallest bank goes first: a fresh small parked copy is
// evicted before an older bigger one. Three parked conversations are needed so that the newest
// (other) is spared, leaving big and small as candidates; small's bank is the smallest so it
// is the victim.
void evict_smallest_first() {
    CacheOptions o = options(64, 3);  // 4 slots (park_slots=3)
    o.focus = 0;
    Chat c(o);
    std::vector<int32_t> big = text(1, 5000), small = text(2, 1000);
    c.ask(big);          // big current (slot 0)
    c.ask(small);        // big parked (slot 0), small current (slot 1)
    std::vector<int32_t> other = text(3, 2000);
    c.ask(other);        // small parked (slot 1), other current (slot 2)
    std::vector<int32_t> next = text(4, 2000);
    c.ask(next);         // other parked (slot 2), next current (slot 3)
    // Now: slot 0 = big (parked, old), slot 1 = small (parked, newer), slot 2 = other (parked, newest)
    std::vector<int32_t> more = text(5, 2000);
    c.ask(more);         // needs a slot: other parks, fit() evicts - newest is other (spared),
                         // candidates are big and small; small's bank is smallest → evicted
    check(c.ask(big) > 0, "the older bigger bank survived eviction");
    check(c.ask(small) == 0, "the smallest bank was evicted: it is read again from 0");
}

// With focus on, a recently-used bank keeps its place while an older candidate exists: the old
// bigger bank is the victim even though the fresh one is smaller.
//
// clock_ increments in resume() (line 68 of conversations.hpp): constructor=1, then each ask++.
// We park big, small, filler, filler2, then ask small again so its stamp is recent (clock_=9).
// Then ask more: fit() evicts. clock_=10, focus=3:
// With focus on, a fresh small bank is spared while an unfocused candidate exists: the eviction
// takes the older medium bank even though the fresh one is smaller.  With focus off the smallest
// bank (the fresh one) would go instead - this test pins the focus behavior.
void evict_respects_focus() {
    CacheOptions o = options(64, 2);
    o.focus = 2;
    o.park_min = 0;
    Chat c(o);
    std::vector<int32_t> medium = text(1, 3000), filler = text(4, 1500), small = text(2, 1000);
    check(c.ask(medium) == 0, "medium starts fresh");
    check(c.ask(filler) == 0, "filler parks");
    check(c.ask(small) == 0, "small parks: all slots full");
    std::vector<int32_t> fresh = text(3, 1500);
    // medium: stamp 1, clock_ = 4, 4-1 = 3 > focus(2) → out of focus
    // filler: stamp 2, clock_ = 4, 4-2 = 2 <= focus(2) → in focus
    // small:  stamp 3, clock_ = 4, 4-3 = 1 <= focus(2) → in focus
    // pass-0: filler and small are in focus → skipped. medium out of focus → candidate.
    // victim = medium (only out-of-focus candidate). fresh takes medium's slot.
    check(c.ask(fresh) == 0, "fresh parks: medium evicted (out of focus, only candidate)");
    check(c.kept.evictions() == 1, "one eviction (medium)");
    check(c.ask(medium) == 0, "evicted medium re-read from 0");
    check(c.ask(small) > 0, "in-focus small spared, resumes at checkpoint");
}

// A parked conversation the client has not used for longer than the TTL is dropped by drop_stale(); a
// recently used one and the current one stay.
void drop_stale_by_ttl() {
    CacheOptions o = options(64, 4);  // 5 slots (park_slots=4)
    o.park_min = 0;
    Chat c(o);
    std::vector<int32_t> a = text(1, 1000), b = text(2, 1000), c2 = text(3, 1000);
    // Park two conversations: a parks (slot 0), b parks (slot 1)
    c.ask(a);        // a current (slot 0)
    c.ask(b);        // a parked (slot 0), b current (slot 1)
    c.ask(c2);       // b parked (slot 1), c2 current (slot 2)
    // Now: slot 0 = a (parked, used just now), slot 1 = b (parked, used just now), slot 2 = c2 (current)
    check(c.kept.drop_stale(60) == 0, "nothing is stale yet (both were just used)");
    // Age slot 0's used backwards: a is now 120 seconds old
    c.kept.set_slot_used(0, std::chrono::steady_clock::now() - std::chrono::seconds(120));
    check(c.kept.drop_stale(60) == 1, "the stale parked conversation a is dropped");
    check(c.kept.parked() == 1, "only b remains parked (c2 is current)");
    check(c.ask(a) == 0, "the dropped conversation a is read again from 0");
    check(c.ask(b) > 0, "the fresh parked conversation b survived");
    check(c.kept.drop_stale(60) == 0, "the current conversation never drops");
}

// With no RAM cap (the default), relieve() is a no-op: a parked bank stays in the heap and no file is
// written - the resume reads the heap, and what the shelf cannot hold is dropped at the park instead.
void relieve_off_by_default() {
    CacheOptions o = options(64, 2);
    o.shelf_ram_mib = 0;
    Chat c(o);
    std::vector<int32_t> a = text(1, 5000);
    check(c.ask(a) == 0, "a starts fresh");
    check(strata::poly::spill_heap_bytes() > 0, "the parked bank stays in the heap: nothing was written to a file");
    std::vector<int32_t> b = text(2, 2000);
    check(c.ask(b) == 0, "b starts fresh, a parks");
    check(c.ask(a) > 0, "a resumes from its heap bank, not from 0");
}

// Past the cap, relieve() sends the oldest unfocused parked banks to their files (their heap frees) and
// spares the focused one: it stays in the heap while the old one reaches its file.
void relieve_spares_focused() {
    CacheOptions o = options(64, 2);
    o.shelf_ram_mib = 1;     // always past the cap: everything eligible gets written
    o.focus = 4;
    Chat c(o);
    std::vector<int32_t> a = text(1, 4000), b = text(2, 4000);
    check(c.ask(a) == 0, "a starts fresh");
    check(c.ask(b) == 0, "b starts fresh, a parks");
    std::vector<int32_t> d = text(3, 2000);
    check(c.ask(d) == 0, "d starts fresh, b parks: relieve writes the old a, the fresh b is focused");
    check(c.ask(b) > 0, "the focused bank b resumes at its checkpoint (heap or file)");
    check(c.ask(a) > 0, "the written bank a resumes at its checkpoint from its file");
}

int main() {
    spill();
    for (const int64_t f : {0, 6000, 700}) {   // 700: reads that start and end between checkpoints
        far = f;
        one_conversation();
        two_conversations();
        shared_start();
        last_turn();
        limits();
        any_order();
    }
    far = 0;
    session_files();
    pinned_prompt();
    quits = 3;   // with reads given up, the places a prompt is taken up from are others: any order, any options
    for (const int64_t f : {6000, 700}) {
        far = f;
        any_order();
    }
    quits = 0;
    long_read();
    pre_park_keeps_live_position();
    pre_parked_rows_restore_on_switch();
    pre_park_respects_budget();
    pre_park_bounded_chunks();
    pre_park_full_shelf();
    pre_park_spare_sizing();
    park_evicts_when_host_refuses();
    switch_park_chunked_full();
    switch_park_partial();
    switch_park_nothing_banked();
    evict_smallest_first();
    evict_respects_focus();
    drop_stale_by_ttl();
    relieve_off_by_default();
    relieve_spares_focused();
    std::printf("conversations_test: %d checks passed\n", checks);
    return 0;
}
