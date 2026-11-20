// src/poly/conversations.hpp - the conversations a server has read, kept so that a prompt which starts like one of
// them is not read again from its first token.
//
// The engine holds one sequence at a time.  Coming back to an earlier point of a sequence takes two things: the
// recurrent state at that point, which is a checkpoint, and the rows its tokens left in the attention caches.  The
// rows of the sequence in the engine are there already.  A conversation that makes room for another is parked:
// its rows are copied to host memory, up to its last checkpoint.  What is saved is written once and never changed,
// so conversations that start alike share it, and a parked conversation comes back bit for bit.  Between requests
// it leaves the process for the file cache (release(), spill.hpp).
//
// Nothing here knows the model.  E is the engine:
//     void reset();
//     size_t state_bytes() const;  void save_state(uint8_t*);  void load_state(const uint8_t*, int64_t pos);
//     size_t row_bytes() const;    void save_rows(int64_t first, int64_t n, uint8_t*);
//     void load_rows(const uint8_t* src, int64_t from, int64_t count, int64_t first, int64_t n);
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "spill.hpp"

namespace strata::poly {

struct CacheOptions {
    int checkpoints = 6;       ///< kept per conversation; 0: a prompt is reread unless it goes on from the last token
    int64_t every = 2048;      ///< a checkpoint every so many prompt tokens, besides the one at a prompt's end and
                               ///< the one where its last turn begins (turn_at()); 0: none
    int64_t park_mib = 0;      ///< host memory for parked conversations; 0: none is parked
    int park_slots = 4;        ///< parked conversations at most
    int64_t park_min = 1024;   ///< a conversation with fewer tokens before its last checkpoint is not parked
    int focus = 12;            ///< a conversation used within this many requests keeps its parked copy while
                               ///< any other candidate exists; 0: off (the smallest bank always goes first)
    int64_t shelf_ram_mib = 0; ///< the parked banks may hold this much heap before the oldest of them go to
                               ///< their files; 0 (the default): no bank is ever written - the shelf is RAM
                               ///< only and what does not fit is dropped at the park
    std::function<bool(size_t)> admit;   ///< whether the host can spare this many bytes; unset: it can
    std::function<bool(size_t)> pre_park_admit;  ///< the same for the idle pre-park; unset: admit rules it
    std::function<std::optional<uint64_t>()> host_available;  ///< the host's free bytes now; unset: unknown
    int64_t pre_park_floor_mib = 1024;  ///< idle pre-parking leaves this much physical RAM free; the switch's
                                        ///< park is gated by admit alone, which carries the stricter floor
    FILE* log = nullptr;       ///< where parking is reported; the server reads these lines
};

template<class E>
class Conversations {
public:
    Conversations(E& engine, CacheOptions o)
        : e_(engine), o_(std::move(o)), slots_((size_t) std::max(o_.park_slots, 0) + 1) {}

    /// The tokens the engine has read.
    const std::vector<int32_t>& seq() const { return slots_[cur_].seq; }

    /// Leaves the engine at the furthest point of `ids` that can be come back to, and returns that position.
    int64_t resume(const std::vector<int32_t>& ids) {
        const int64_t n = (int64_t) ids.size();
        turn_ = -1;
        size_t best = cur_;
        int64_t at = 0, same = 0;
        for (size_t i = 0; i < slots_.size(); ++i) {
            const int64_t c = common(slots_[i].seq, ids), r = reach(i, c, n);
            if (r > at || (r == at && i == cur_)) best = i, at = r, same = c;
        }
        ++clock_;
        // The pinned conversation (pin()) gets the prompt further than any kept one: the engine takes a copy of it.
        if (const int64_t far = marked(pinned_, common(pinned_.seq, ids), n); far > at) return from_pinned(far);
        if (at == 0) {
            // nothing kept starts like this
            if (!cur().seq.empty() && park(slots_.size())) cur_ = take(cur_);
            slots_[cur_] = Slot{};
            e_.reset();
        } else if (best != cur_ || at < len(cur())) {
            if (best != cur_) enter(best, at);
            // The conversation is left at `at`.  If the prompt the client sent for it went on from there, the
            // client may send that again, so the conversation is parked and this one goes on in a copy.
            if (same >= cur().prompt || !branch(at)) cut(cur(), at);
            e_.load_state(state_at(cur(), at), at);
        }
        cur().stamp = clock_;
        cur().used = Clock::now();
        return at;
    }

    /// One more checkpoint of the prompt resume() took up: where its chat's last turn begins, which the server
    /// knows from the template.  The chat's next request renders the conversation up to there as this one does,
    /// but this turn's header and reply not always in the same tokens (a template drops the reply's thinking), so
    /// the checkpoint before the prompt's last token is seldom where it goes on from.  A place the read does not
    /// pass adds none.
    void turn_at(int64_t at) { turn_ = at; }

    /// Of `want` tokens from position `at`, how many to read before the next checkpoint is due.
    int64_t until_checkpoint(int64_t at, int64_t want) const {
        if (o_.checkpoints < 1) return want;
        if (o_.every > 0) want = std::min(want, (at / o_.every + 1) * o_.every - at);
        return turn_ > at ? std::min(want, turn_ - at) : want;
    }

    /// Before the engine reads the token at `at`; `last` says it is the prompt's last.  Takes a checkpoint if one is
    /// due.  One sits before the last token of every prompt, so coming back to it leaves a token to read and the
    /// logits that come with it.
    void before(int64_t at, bool last) {
        if (o_.checkpoints < 1 || at < 1 || (!last && at != turn_ && (o_.every < 1 || at % o_.every != 0))) return;
        if (const BlobPtr b = mark(at)) e_.save_state(b->data());
    }

    /// For an engine that reads past checkpoints in one go, of n tokens from `at`: the checkpoints due after the
    /// read's first token.  `out` gets, for each, how many of the read's tokens come before it and where its
    /// state_bytes() go; null where no state is wanted, and the engine still breaks its work there as if one
    /// were.  The engine writes a state when it has it.  The checkpoints are the conversation's once read() has
    /// the read's tokens, and not at all when the read is given up (rewind()).
    ///
    /// A state is wanted for the checkpoints the conversation would still have, had each been taken in turn as
    /// before() takes one.  A long prompt has more due than are kept, and a state can be hundreds of MiB.
    void inside(int64_t at, int64_t n, std::vector<std::pair<int64_t, uint8_t*>>& out) {
        out.clear();
        due_.clear();
        after_.clear();
        if (o_.checkpoints < 1) return;
        std::vector<int64_t> due;
        if (o_.every > 0)
            for (int64_t p = (at / o_.every + 1) * o_.every; p < at + n; p += o_.every) due.push_back(p);
        if (turn_ > at && turn_ < at + n && (o_.every < 1 || turn_ % o_.every != 0))
            due.insert(std::upper_bound(due.begin(), due.end(), turn_), turn_);
        if (due.empty()) return;
        for (const Checkpoint& m : cur().marks) after_.push_back(m.pos);
        for (const int64_t p : due) {
            after_.push_back(p);
            while (after_.size() > (size_t) o_.checkpoints) after_.erase(after_.begin() + (std::ptrdiff_t) drop_of(after_));
        }
        for (const int64_t p : due) {
            const bool stays = std::find(after_.begin(), after_.end(), p) != after_.end();
            due_.push_back({p, stays ? blob(e_.state_bytes()) : nullptr});
            out.emplace_back(p - at, due_.back().state ? due_.back().state->data() : nullptr);
        }
    }

    /// The engine has read these tokens.  A read that inside() was asked about is done with them, and its
    /// checkpoints take their places.
    void read(const int32_t* t, int64_t n) {
        Slot& s = cur();
        s.seq.insert(s.seq.end(), t, t + n);
        if (due_.empty()) return;
        const auto stays = [&](const Checkpoint& m) {
            return m.state && std::find(after_.begin(), after_.end(), m.pos) != after_.end();
        };
        std::vector<Checkpoint> marks;
        for (const Checkpoint& m : s.marks)
            if (stays(m)) marks.push_back(m);
        for (const Checkpoint& m : due_)
            if (stays(m)) marks.push_back(m);
        s.marks = std::move(marks);
        due_.clear();
    }

    /// A read was given up part way: what it left in the engine is not the conversation's, and the checkpoints
    /// inside it are not taken.  The conversation goes back to its last checkpoint, which the engine is left
    /// at, or is forgotten when it has none.  Returns where the conversation is then.
    int64_t rewind() {
        due_.clear();
        Slot& s = cur();
        if (s.marks.empty()) {
            slots_[cur_] = Slot{};
            e_.reset();
            return 0;
        }
        const int64_t at = s.marks.back().pos;
        cut(s, at);
        e_.load_state(s.marks.back().state->data(), at);
        return at;
    }
    /// The first n tokens of the conversation are the prompt the client sent.
    void prompt_was(int64_t n) { cur().prompt = std::min(n, len(cur())); }
    /// After an engine error: the conversation being read is lost.  The caller resets the engine.
    void drop() {
        slots_[cur_] = Slot{};
        due_.clear();
    }

    /// The engine's sequence is about to hold another conversation, read elsewhere (a batch slot's copy): the one
    /// it holds is parked, as resume() parks it, or forgotten.  adopt() then says what it holds.
    void leave() {
        if (!cur().seq.empty() && park(slots_.size())) cur_ = take(cur_);
        slots_[cur_] = Slot{};
        due_.clear();
    }
    /// The engine holds `seq` now, with the state at its end: a prompt that goes on from there reads none of it.
    void adopt(std::vector<int32_t> seq) {
        Slot& s = cur();
        s = Slot{};
        s.seq = std::move(seq);
        s.prompt = len(s);
        s.stamp = ++clock_;
    }

    // A session file's part of the conversation in the engine, to come back to in another run of the engine
    // (SAVE and RESTORE in main.cpp, which bind the file to the model and hash it).  The bytes, in order: a Saved,
    // the tokens, each checkpoint's position and state with the state at the conversation's end as the last,
    // then the rows in pieces of Saved::piece_rows rows, each as the engine's save_rows() writes it.
    //
    // Of the conversation's checkpoints a file has the last two: where its last turn began and the end of its
    // prompt, which is where its next turn goes on from when not from its end.  A state is 63 MiB of Qwen3.6's,
    // and with all of them the file of a 24K conversation was half states.  An edit before those two is read
    // from the start after a restore.
    struct Saved {
        uint64_t row_bytes, state_bytes, tokens, prompt, marks, piece_rows;
    };
    /// What save() writes for the conversation as it stands; 0: the engine holds none.
    uint64_t save_bytes() const {
        const Slot& s = cur();
        const uint64_t n = (uint64_t) len(s);
        return n == 0 ? 0 : sizeof(Saved) + n * 4 + marks_saved(s) * (8 + e_.state_bytes()) + n * e_.row_bytes();
    }
    /// put(data, bytes) gets save_bytes() bytes, the rows in pieces of `piece` bytes at most, and returns false
    /// to stop.  The engine must be at the conversation's end.  The conversation stays as it is.  False: put
    /// stopped, or there was no memory for a piece.
    template<class Put>
    bool save(size_t piece, Put&& put) {
        const Slot& s = cur();
        const int64_t n = len(s);
        const size_t row = e_.row_bytes(), state = e_.state_bytes();
        const int64_t per = std::max<int64_t>((int64_t) (piece / std::max<size_t>(row, 1)), 1);
        const Saved head{row, state, (uint64_t) n, (uint64_t) s.prompt, marks_saved(s), (uint64_t) per};
        const std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[std::max((size_t) per * row, state)]);
        if (n < 1 || !buf || !put(&head, sizeof head) || !put(s.seq.data(), (size_t) n * 4)) return false;
        for (size_t k = first_saved(s); k < s.marks.size(); ++k) {
            const Checkpoint& m = s.marks[k];
            if (!m.state->data() || !put(&m.pos, 8) || !put(m.state->data(), state)) return false;
        }
        if (!at_end(s)) {
            e_.save_state(buf.get());
            if (!put(&n, 8) || !put(buf.get(), state)) return false;
        }
        for (int64_t first = 0; first < n; first += per) {
            const int64_t m = std::min(per, n - first);
            e_.save_rows(first, m, buf.get());
            if (!put(buf.get(), (size_t) m * row)) return false;
        }
        return true;
    }

    enum class Restored {
        ok,
        other,    ///< not what save() writes for this engine and `bytes`, or longer than `most` tokens; nothing changed
        memory,   ///< no memory for it; nothing changed
        cut,      ///< get() ended inside the rows: the engine holds no conversation now
    };
    /// What save() wrote, `bytes` of it from get(data, bytes), becomes the conversation in the engine, with its
    /// checkpoints.  The one that was there is parked if it can be.  The caller that then finds the bytes were
    /// not the saved ones (a hash) calls drop() and resets the engine.
    template<class Get>
    Restored restore(uint64_t bytes, int64_t most, size_t piece, Get&& get) {
        const size_t row = e_.row_bytes();
        Saved head;
        Slot in;
        if (const Restored r = read_start(bytes, most, piece, get, head, in); r != Restored::ok) return r;
        const int64_t n = (int64_t) head.tokens, per = (int64_t) head.piece_rows;
        const std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[(size_t) per * row]);
        if (!buf) return Restored::memory;
        // from here the engine holds the file's conversation, or none
        leave();
        e_.reset();
        for (int64_t first = 0; first < n; first += per) {
            const int64_t m = std::min(per, n - first);
            if (!get(buf.get(), (size_t) m * row)) {
                e_.reset();
                return Restored::cut;
            }
            e_.load_rows(buf.get(), first, m, first, m);
        }
        e_.load_state(in.marks.back().state->data(), n);
        while (!in.marks.empty() && in.marks.size() > (size_t) std::max(o_.checkpoints, 0)) {
            std::vector<int64_t> pos;
            for (const Checkpoint& m : in.marks) pos.push_back(m.pos);
            in.marks.erase(in.marks.begin() + (std::ptrdiff_t) drop_of(pos));
        }
        in.prompt = (int64_t) head.prompt;
        in.stamp = ++clock_;
        slots_[cur_] = std::move(in);
        return Restored::ok;
    }

    /// What save() wrote becomes the pinned conversation, in place of one pinned before: a system prompt that every
    /// chat starts with, read once.  It is kept beside the others with its rows in memory, as a parked one's, but
    /// outside the budget and the slots, and nothing pushes it out.  A prompt that starts with it goes on from a
    /// copy of it (resume()), so it stays as it was read.  The engine is not touched.  The result is restore()'s;
    /// cut: get() ended inside the rows, and nothing is pinned.  The caller that then finds the bytes were not the
    /// saved ones calls unpin().
    template<class Get>
    Restored pin(uint64_t bytes, int64_t most, size_t piece, Get&& get) {
        const size_t row = e_.row_bytes();
        Saved head;
        Slot in;
        pinned_ = Slot{};
        if (const Restored r = read_start(bytes, most, piece, get, head, in); r != Restored::ok) return r;
        const int64_t n = (int64_t) head.tokens, per = (int64_t) head.piece_rows;
        for (int64_t first = 0; first < n; first += per) {
            const int64_t m = std::min(per, n - first);
            const BlobPtr b = blob((size_t) m * row);
            if (!b) return Restored::memory;
            if (!get(b->data(), (size_t) m * row)) return Restored::cut;
            in.rows.push_back({b, first, m, m});
        }
        in.prompt = n;
        pinned_ = std::move(in);
        return Restored::ok;
    }
    void unpin() { pinned_ = Slot{}; }
    /// The pinned conversation's tokens (0: none), and the bytes its rows and states take.
    int64_t pinned_tokens() const { return len(pinned_); }
    size_t pinned_bytes() const {
        size_t bytes = 0;
        for (const Checkpoint& m : pinned_.marks) bytes += m.state->bytes();
        for (const Rows& r : pinned_.rows) bytes += r.blob->bytes();
        return bytes;
    }

    /// Between requests: what the parked conversations hold leaves the process.  On the laptop four parked
    /// conversations of Qwen3.6 held 1.1 to 1.7 GB, one of 29K tokens 1.05 GB.  The one in the engine keeps its
    /// states in memory: every request saves one (63 MB of Qwen3.6's), and the next request most likely goes on
    /// from it, so written to the disk it would mostly be read back at once.
    void release() const {
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (i == cur_) continue;
            for (const Checkpoint& m : slots_[i].marks) m.state->release();
            for (const Rows& r : slots_[i].rows) r.blob->release();
        }
        for (const Checkpoint& m : pinned_.marks) m.state->release();
        for (const Rows& r : pinned_.rows) r.blob->release();
    }

    /// Between requests and in the idle tick: with the shelf allowed to spill (shelf_ram_mib > 0), a heap
    /// past that cap sends its oldest parked banks to their files - a bank on its file holds no RAM.  A
    /// shelf_ram_mib of 0 (the default) parks nothing but RAM: this is a no-op and no file is ever made.
    /// The bank in the engine and the focused ones (used within `focus` requests) are spared - the copy
    /// falls to the next candidate instead.
    void relieve() const {
        if (o_.shelf_ram_mib <= 0) return;
        const size_t cap = (size_t) o_.shelf_ram_mib << 20;
        if (spill_heap_bytes() <= cap) return;
        size_t moved = 0;
        std::vector<size_t> order;   // the parked slots, oldest first
        for (size_t i = 0; i < slots_.size(); ++i)
            if (i != cur_ && !slots_[i].seq.empty()) order.push_back(i);
        std::sort(order.begin(), order.end(),
                  [this](size_t a, size_t b) { return slots_[a].stamp < slots_[b].stamp; });
        for (const size_t i : order) {
            if (spill_heap_bytes() <= cap) break;
            if (o_.focus > 0 && clock_ - slots_[i].stamp <= (uint64_t) o_.focus) continue;
            for (const Checkpoint& m : slots_[i].marks) m.state->release();
            for (const Rows& r : slots_[i].rows) r.blob->release();
            ++moved;
        }
        if (moved && o_.log)
            std::fprintf(o_.log, "conversation cache: relieved %zu banks to their files for the heap cap; "
                                 "parked=%d bytes=%lld heap=%lld MiB\n", moved, parked(),
                                 (long long) parked_bytes(), (long long) (spill_heap_bytes() >> 20));
    }

    /// Between requests, when the engine is idle: copy what the current conversation holds only in the engine, up to
    /// its last checkpoint, to host memory, the way park() keeps a conversation that is leaving - but without cutting
    /// the conversation: it stays in the engine whole, and a request that goes on from here finds the engine where it
    /// was.  When another conversation later takes the engine, park() then has only the rows read since to copy.
    // A chunk is an eighth of the budget in tokens: big enough that a 64K-token conversation banks in
    // one piece, small enough that a request arriving mid-banking interrupts within one chunk.  The
    // idle loop (main.cpp) banks chunks back-to-back through the whole idle moment, so the pacing
    // comes from the loop, not from the chunk size.
    int64_t chunk_tokens() const {
        const int64_t rb = (int64_t) e_.row_bytes();
        if (rb < 1) return 0;
        int64_t bytes = ((int64_t) o_.park_mib << 20) / 8;
        if (o_.host_available) {
            const std::optional<uint64_t> avail = o_.host_available();
            if (avail) {
                // The spare in bytes, no MiB truncation: what matters is the host's real headroom.
                const uint64_t floor_bytes = (uint64_t) o_.pre_park_floor_mib << 20;
                if (*avail < floor_bytes) return 0;
                bytes = std::min(bytes, (int64_t) (*avail - floor_bytes));
            }
        }
        return bytes / rb;
    }

    bool pre_park() {
        Slot& s = cur();
        if (o_.park_mib < 1 || o_.park_slots < 1 || s.marks.empty() || s.marks.back().pos < o_.park_min) return false;
        const Clock::time_point t0 = Clock::now();
        const int64_t end = s.marks.back().pos, from = saved(s);
        if (from == end) return true;  // nothing to do, no logging
        // One idle tick copies at most a quarter of the budget, and only what the host can spare above the
        // smaller pre-park floor.  A delta bigger than the host's free RAM is refused whole and retried whole
        // every tick - the parked copy never advances, and a later switch destroys the engine's copy wholesale
        // (a 96K-token session then re-reads 92,499 tokens, 76 s).  Bounded chunks pass the gate even on a busy
        // day; the next tick continues from here.  The idle banking loop (when the shelf can spill) calls
        // pre_park() back-to-back; a request that arrives interrupts within one small chunk.
        const int64_t cap = chunk_tokens();
        if (cap < 1) return false;
        return save_rows(cur_, "pre-parked", t0, from + cap, true);
    }

    /// The parked conversations, the bytes they hold, and how many were pushed out so far.
    size_t parked() const { return held(slots_.size()).count; }
    size_t parked_bytes() const { return held(slots_.size()).bytes; }
    size_t evictions() const { return evictions_; }

    // Test hook: set the `used` time of a parked slot to age it past the TTL.
    void set_slot_used(size_t i, std::chrono::steady_clock::time_point tp) { slots_[i].used = tp; }

    // Drops the parked conversations the client has not used for `ttl_s` seconds — a session that ended long ago
    // left behind.  Runs in the idle tick, so nothing is waiting: a drop is free and a session that returns
    // anyway just reads its prompt again.  The current conversation never goes.  Returns how many were dropped.
    size_t drop_stale(long long ttl_s) {
        if (ttl_s <= 0) return 0;
        const Clock::time_point now = Clock::now();
        size_t n = 0;
        int64_t tokens = 0;
        for (size_t i = 0; i < slots_.size(); ++i) {
            Slot& s = slots_[i];
            if (i == cur_ || s.seq.empty() || s.used == Clock::time_point{}) continue;
            if (ms_since(s.used) < (double) ttl_s * 1000.0) continue;
            tokens += len(s);
            s = Slot{};
            ++n;
        }
        if (n && o_.log) {
            const Held h = held(cur_);
            stale_drops_ += n;
            std::fprintf(o_.log,
                         "strata-poly: conversation cache: dropped %zu stale conversations (%lld tokens idle over %lld s); parked=%zu bytes=%zu evictions=%zu heap=%lld MiB\n",
                         n, (long long) tokens, ttl_s, h.count, h.bytes, evictions_,
                         (long long) (spill_heap_bytes() >> 20));
        }
        return n;
    }

private:
    using Clock = std::chrono::steady_clock;

    using Blob = Spill;
    using BlobPtr = std::shared_ptr<const Blob>;
    struct Checkpoint {
        int64_t pos;
        BlobPtr state;
    };
    // The blob holds `count` rows from row `first`; the first `keep` of them are this conversation's.
    struct Rows {
        BlobPtr blob;
        int64_t first, count, keep;
    };
    struct Slot {
        std::vector<int32_t> seq;
        int64_t prompt = 0;             // seq's first tokens that the client sent
        std::vector<Checkpoint> marks;  // by position
        std::vector<Rows> rows;         // back to back from row 0; a parked conversation has all of its rows
        uint64_t stamp = 0;
        Clock::time_point used{};        // wall-clock time of the last turn — age for the idle-age TTL drop
    };
    struct Held {
        size_t count = 0, bytes = 0;
    };

    Slot& cur() { return slots_[cur_]; }
    const Slot& cur() const { return slots_[cur_]; }
    static int64_t len(const Slot& s) { return (int64_t) s.seq.size(); }
    static int64_t saved(const Slot& s) { return s.rows.empty() ? 0 : s.rows.back().first + s.rows.back().keep; }
    static double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
    // The checkpoints save() writes: the last two before the conversation's end, and one at its end, which is
    // the conversation's own when it has one there.
    static bool at_end(const Slot& s) { return !s.marks.empty() && s.marks.back().pos == len(s); }
    static size_t first_saved(const Slot& s) {
        const size_t keep = at_end(s) ? 3 : 2;
        return s.marks.size() > keep ? s.marks.size() - keep : 0;
    }
    static uint64_t marks_saved(const Slot& s) { return s.marks.size() - first_saved(s) + (at_end(s) ? 0 : 1); }

    static int64_t common(const std::vector<int32_t>& a, const std::vector<int32_t>& b) {
        size_t n = 0;
        while (n < a.size() && n < b.size() && a[n] == b[n]) ++n;
        return (int64_t) n;
    }

    // How far into a prompt of n tokens slot i gets it, given that they start with `same` tokens alike.
    int64_t reach(size_t i, int64_t same, int64_t n) const {
        const Slot& s = slots_[i];
        if (i == cur_ && same > 0 && same == len(s) && n > same) return same;   // the engine is right there
        return marked(s, same, n);
    }
    // The last checkpoint of s a prompt of n tokens can be taken up from, when they start with `same` tokens alike.
    static int64_t marked(const Slot& s, int64_t same, int64_t n) {
        int64_t at = 0;
        for (const Checkpoint& m : s.marks)
            if (m.pos <= std::min(same, n - 1)) at = m.pos;
        return at;
    }

    // The start of what save() wrote, for restore() and pin(): the sizes checked against this engine and `bytes`,
    // the tokens and the checkpoints with their states.  The checkpoints are taken as a request's are (mark()),
    // without asking CacheOptions::admit: that is for what parking adds, and on a PC whose RAM the model fills
    // the question refused every restore.
    template<class Get>
    Restored read_start(uint64_t bytes, int64_t most, size_t piece, Get&& get, Saved& head, Slot& in) {
        const size_t row = e_.row_bytes(), state = e_.state_bytes();
        if (bytes < sizeof head || !get(&head, sizeof head) || head.row_bytes != row || head.state_bytes != state ||
            head.tokens == 0 || head.tokens > (uint64_t) most || head.prompt > head.tokens || head.marks == 0 ||
            head.marks > 64 || head.piece_rows == 0 ||
            head.piece_rows > std::max<uint64_t>(piece / std::max<size_t>(row, 1), 1) ||
            bytes != sizeof head + head.tokens * 4 + head.marks * (8 + state) + head.tokens * row)
            return Restored::other;
        const int64_t n = (int64_t) head.tokens;
        try {
            in.seq.resize((size_t) n);
        } catch (const std::bad_alloc&) {
            return Restored::memory;
        }
        if (!get(in.seq.data(), (size_t) n * 4)) return Restored::other;
        for (uint64_t k = 0; k < head.marks; ++k) {
            int64_t pos = 0;
            const int64_t after = in.marks.empty() ? 0 : in.marks.back().pos;
            if (!get(&pos, 8) || pos <= after || pos > n || (pos == n) != (k + 1 == head.marks)) return Restored::other;
            const BlobPtr b = blob(state);
            if (!b) return Restored::memory;
            if (!get(b->data(), state)) return Restored::other;
            in.marks.push_back({pos, b});
        }
        return Restored::ok;
    }

    // The engine takes a copy of the pinned conversation's first `at` tokens, a checkpoint of it, and the pinned one
    // stays as it is.  The conversation that leaves is parked if it can be.  The rows it has from the pinned one's
    // blobs are in the caches already: a chat that began as such a copy gives way to the next one without a copy.
    int64_t from_pinned(int64_t at) {
        const Clock::time_point t0 = Clock::now();
        const Slot& p = pinned_;
        int64_t have = 0;
        for (size_t k = 0; k < p.rows.size() && k < cur().rows.size() && p.rows[k].blob == cur().rows[k].blob; ++k) {
            have = p.rows[k].first + std::min(p.rows[k].keep, cur().rows[k].keep);
            if (p.rows[k].keep != cur().rows[k].keep) break;
        }
        if (!cur().seq.empty() && park(slots_.size())) cur_ = take(cur_);
        Slot c;
        c.seq.assign(p.seq.begin(), p.seq.begin() + (std::ptrdiff_t) at);
        c.prompt = at;
        for (const Checkpoint& m : p.marks)
            if (m.pos <= at) c.marks.push_back(m);
        for (const Rows& r : p.rows) {
            if (r.first >= at) break;
            c.rows.push_back({r.blob, r.first, r.count, std::min(r.keep, at - r.first)});
            const int64_t a = std::max(r.first, have), b = std::min(r.first + r.keep, at);
            if (a < b) e_.load_rows(r.blob->data(), r.first, r.count, a, b - a);
        }
        e_.load_state(state_at(p, at), at);
        while (c.marks.size() > (size_t) std::max(o_.checkpoints, 0)) c.marks.erase(c.marks.begin());
        c.stamp = clock_;
        slots_[cur_] = std::move(c);
        say("restored", at, t0, slots_.size(), " (pinned)");
        return at;
    }

    static const uint8_t* state_at(const Slot& s, int64_t at) {
        for (const Checkpoint& m : s.marks)
            if (m.pos == at) return m.state->data();
        return nullptr;   // resume() only goes to positions reach() found
    }

    static BlobPtr blob(size_t bytes) {
        auto b = std::make_shared<Blob>(bytes);
        return b->data() ? b : nullptr;
    }

    // A checkpoint of the current conversation at `at`, its state still to be written.  Null: there is one there,
    // or no memory for it.
    BlobPtr mark(int64_t at) {
        Slot& s = cur();
        if (!s.marks.empty() && s.marks.back().pos == at) return nullptr;
        const BlobPtr b = blob(e_.state_bytes());
        if (!b) return nullptr;
        s.marks.push_back({at, b});
        while (s.marks.size() > (size_t) o_.checkpoints) {
            std::vector<int64_t> pos;
            for (const Checkpoint& m : s.marks) pos.push_back(m.pos);
            s.marks.erase(s.marks.begin() + (std::ptrdiff_t) drop_of(pos));
        }
        return b;
    }

    // Of more checkpoints than are kept, the one that goes: the one whose neighbours are closest together.  The
    // last two stay, the end of this prompt and of the one before being where a client most often comes back to.
    static size_t drop_of(const std::vector<int64_t>& pos) {
        size_t drop = 0;
        int64_t gap = INT64_MAX;
        for (size_t k = 0; k + 2 < pos.size(); ++k) {
            const int64_t g = pos[k + 1] - (k ? pos[k - 1] : 0);
            if (g < gap) gap = g, drop = k;
        }
        return drop;
    }

    static void cut(Slot& s, int64_t at) {
        if (at >= len(s)) return;
        s.seq.resize((size_t) at);
        s.prompt = std::min(s.prompt, at);
        while (!s.marks.empty() && s.marks.back().pos > at) s.marks.pop_back();
        while (!s.rows.empty() && s.rows.back().first >= at) s.rows.pop_back();
        if (!s.rows.empty()) s.rows.back().keep = std::min(s.rows.back().keep, at - s.rows.back().first);
    }

    // The parked conversations and what they hold; a blob several of them share counts once.  The current one
    // counts when `with` is its slot.
    Held held(size_t with) const {
        Held h;
        std::vector<const Blob*> seen;
        for (size_t i = 0; i < slots_.size(); ++i) {
            const Slot& s = slots_[i];
            if (s.seq.empty() || (i == cur_ && i != with)) continue;
            ++h.count;
            for (const Checkpoint& m : s.marks) seen.push_back(m.state.get());
            for (const Rows& r : s.rows) seen.push_back(r.blob.get());
        }
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        // what a conversation shares with the pinned one is the pinned one's, outside the budget
        const auto pinned = [this](const Blob* b) {
            for (const Checkpoint& m : pinned_.marks)
                if (m.state.get() == b) return true;
            for (const Rows& r : pinned_.rows)
                if (r.blob.get() == b) return true;
            return false;
        };
        for (const Blob* b : seen)
            if (!pinned(b)) h.bytes += b->bytes();
        return h;
    }

    // The bytes this slot's bank holds: its checkpoint states and its saved rows.
    size_t bank_bytes(size_t i) const {
        size_t bytes = 0;
        for (const Checkpoint& m : slots_[i].marks) bytes += m.state ? m.state->bytes() : 0;
        for (const Rows& r : slots_[i].rows) bytes += r.blob ? r.blob->bytes() : 0;
        return bytes;
    }

    // The parked conversation that gives way: the smallest bank first (its rows are the cheapest to
    // read again), among equals the one not used for longest.  A conversation the client was in within
    // the last `focus` requests keeps its place while any other candidate exists.  The newest parked
    // one never goes (a stale RAM reading must not wipe what just parked).  `keep` is a slot the
    // caller is about to reuse and is spared.  False: every candidate is protected.
    bool pick_victim(size_t keep, size_t& out) {
        out = slots_.size();
        size_t newest = 0;
        for (size_t i = 0; i < slots_.size(); ++i)
            if (i != cur_ && i != keep && !slots_[i].seq.empty() && slots_[i].stamp > newest)
                newest = slots_[i].stamp;
        for (int pass = 0; pass < 2 && out == slots_.size(); ++pass) {
            for (size_t i = 0; i < slots_.size(); ++i) {
                const Slot& s = slots_[i];
                if (i == cur_ || i == keep || s.seq.empty() || s.stamp == newest) continue;
                if (pass == 0 && o_.focus > 0 && clock_ - s.stamp <= (uint64_t) o_.focus) continue;
                if (out == slots_.size() || bank_bytes(i) < bank_bytes(out) ||
                    (bank_bytes(i) == bank_bytes(out) && s.stamp < slots_[out].stamp))
                    out = i;
            }
        }
        return out != slots_.size();
    }

    // Makes the parked conversations, the current one among them and `extra` bytes more, fit the budget and the
    // slots.  The least recently used goes first, whatever its length: a long conversation can lose its place to
    // several short ones.  Slot `keep` stays; it is the one about to take the engine, if any, so it frees a slot.
    // When the host refuses `extra` (the free-RAM gate), a parked conversation is evicted for it too and the check
    // is retried - the newest parked one stays, so a stale RAM reading cannot wipe the whole shelf.  The idle
    // pre-park (`pre`) never evicts: its chunk waits for the next tick instead.  False: it does not fit.
    bool fit(size_t extra, size_t keep, bool pre = false) {
        const size_t budget = (size_t) o_.park_mib << 20;
        const std::function<bool(size_t)>& admit = pre && o_.pre_park_admit ? o_.pre_park_admit : o_.admit;
        for (;;) {
            const Held h = held(cur_);
            const size_t leaving = keep < slots_.size() && keep != cur_ && !slots_[keep].seq.empty() ? 1 : 0;
            // The idle pre-park (`pre`) saves into the live conversation's own slot: it takes no shelf slot, so the
            // live conversation does not count against park_slots there (the switch's park then takes one and
            // evicts for it).  Counting it made every pre-park refuse at once with the shelf full - the state
            // opencode lives in - and the switch did all the saving one-shot.
            const size_t shelves = h.count - (pre ? 1 : 0);
            if (h.bytes + extra <= budget && shelves - leaving <= (size_t) o_.park_slots &&
                (extra == 0 || !admit || admit(extra)))
                return true;
            if (pre) return false;
            // The newest parked one stays, so a stale RAM reading cannot wipe the whole shelf.
            size_t out;
            if (!pick_victim(keep, out)) return false;
            slots_[out] = Slot{};
            ++evictions_;
        }
    }

    // Copies the rows the current conversation still has only in the engine, up to its last checkpoint; what it
    // read after that cannot be come back to and is dropped.  False: it cannot be kept.
    bool park(size_t keep) {
        Slot& s = cur();
        if (o_.park_mib < 1 || o_.park_slots < 1 || s.marks.empty() || s.marks.back().pos < o_.park_min) return false;
        const Clock::time_point t0 = Clock::now();
        const int64_t end = s.marks.back().pos;
        cut(s, end);
        return save_rows(keep, "parked", t0);
    }

    // Copies the rows the current conversation still has only in the engine, up to its last checkpoint, to host
    // memory.  `label` is what say() prints ("parked" or "pre-parked").  `up_to` caps the copy: the idle
    // pre-park passes a bound so one attempt stays small enough for the host's free RAM (the switch's park
    // passes none and saves to the last checkpoint).  Returns true when the rows are in host memory (or nothing
    // needed saving), false when the guards or the budget said no.
    bool save_rows(size_t keep, const char* label, Clock::time_point t0, int64_t up_to = INT64_MAX,
                   bool pre = false) {
        Slot& s = cur();
        const int64_t rb = e_.row_bytes();
        const int64_t from0 = saved(s);
        int64_t from = from0;
        const int64_t end = std::min(s.marks.back().pos, up_to);
        if (from < end && rb > 0) {
            // The switch's park used to be one blob of the whole delta; a long conversation could not be
            // banked (the allocation or the free-RAM gate refused it) and was lost whole, so its next turn
            // re-read everything.  Bank the delta in bounded chunks instead - an eighth of the budget each,
            // the rule the idle pre-park sizes by - and stop at the first chunk the guards refuse.  What
            // earlier chunks banked stays: a partial park keeps the conversation's prefix, and the client
            // re-reads only the tail beyond it.
            const int64_t cap = o_.park_mib > 0 ? ((int64_t) o_.park_mib << 20) / 8 / rb : 0;
            while (from < end && cap > 0) {
                const int64_t to = std::min(end, from + cap);
                const size_t bytes = (size_t) (to - from) * rb;
                const BlobPtr b = fit(bytes, keep, pre) ? blob(bytes) : nullptr;
                if (!b) break;
                e_.save_rows(from, to - from, b->data());
                s.rows.push_back({b, from, to - from, to - from});
                from = to;
                say(label, to, t0, cur_);
            }
        }
        if (from == from0 && from < end) {
            say("skipped", end, t0, slots_.size());
            return false;
        }
        if (!fit(0, keep, pre)) return false;
        if (from == from0) say(label, end, t0, cur_);
        return true;
    }

    // An empty slot other than `keep`; when none is free, the least valuable parked conversation goes,
    // by the same rule fit() uses.
    size_t take(size_t keep) {
        for (size_t i = 0; i < slots_.size(); ++i)
            if (i != keep && slots_[i].seq.empty()) return i;
        size_t out;
        if (!pick_victim(keep, out)) {
            out = slots_.size();
            for (size_t i = 0; i < slots_.size(); ++i)
                if (i != keep && (out == slots_.size() || slots_[i].stamp < slots_[out].stamp)) out = i;
            if (out == slots_.size()) return out;   // nothing to give (should not happen)
        }
        slots_[out] = Slot{};
        ++evictions_;
        return out;
    }

    // Slot i takes the engine, with its rows before `need` back in the caches.  The conversation that leaves is
    // parked if it can be.  Rows that both have from the same blob are in the caches already.
    void enter(size_t i, int64_t need) {
        const Clock::time_point t0 = Clock::now();
        const Slot& in = slots_[i];
        int64_t have = 0;
        for (size_t k = 0; k < in.rows.size() && k < cur().rows.size() && in.rows[k].blob == cur().rows[k].blob; ++k) {
            have = in.rows[k].first + std::min(in.rows[k].keep, cur().rows[k].keep);
            if (in.rows[k].keep != cur().rows[k].keep) break;
        }
        if (cur().seq.empty() || !park(i)) slots_[cur_] = Slot{};
        for (const Rows& r : in.rows) {
            const int64_t a = std::max(r.first, have), b = std::min(r.first + r.keep, need);
            if (a < b) e_.load_rows(r.blob->data(), r.first, r.count, a, b - a);
        }
        cur_ = i;
        say("restored", need, t0, slots_.size());
    }

    // Parks the current conversation whole and goes on at `at` in a copy of its start, which shares what is
    // saved.  False: it cannot be parked.
    bool branch(int64_t at) {
        const size_t from = cur_;
        if (!park(slots_.size())) return false;
        const size_t to = take(from);
        const Slot& a = slots_[from];
        Slot b;
        b.seq.assign(a.seq.begin(), a.seq.begin() + (std::ptrdiff_t) at);
        b.prompt = std::min(a.prompt, at);
        for (const Checkpoint& m : a.marks)
            if (m.pos <= at) b.marks.push_back(m);
        for (const Rows& r : a.rows)
            if (r.first < at) b.rows.push_back({r.blob, r.first, r.count, std::min(r.keep, at - r.first)});
        slots_[to] = std::move(b);
        cur_ = to;
        return true;
    }

    // `with` is the current slot when its conversation is the one just parked.  The heap figure is what the
    // process really holds for the banks right now (spill.hpp): bytes= counts their logical size, and a bank
    // that reached its file holds no heap at all - a heap figure that stays high while parked= falls says the
    // writes are not getting through.
    void say(const char* what, int64_t tokens, Clock::time_point t0, size_t with, const char* note = "") const {
        if (!o_.log) return;
        const Held h = held(with);
        std::fprintf(o_.log,
                     "strata-poly: conversation cache: %s %lld tokens%s in %.1f ms; parked=%zu bytes=%zu evictions=%zu heap=%lld MiB\n",
                     what, (long long) tokens, note,
                     std::chrono::duration<double, std::milli>(Clock::now() - t0).count(),
                     h.count, h.bytes, evictions_, (long long) (spill_heap_bytes() >> 20));
    }

    E& e_;
    CacheOptions o_;
    std::vector<Slot> slots_;   // the current conversation and the parked ones
    Slot pinned_;               // pin()'s: beside them, never the engine's own, never pushed out
    size_t cur_ = 0;
    uint64_t clock_ = 0;
    size_t evictions_ = 0;
    size_t stale_drops_ = 0;     // conversations dropped by the idle-age TTL
    std::vector<Checkpoint> due_;   // inside()'s checkpoints, until read() makes them the conversation's
    std::vector<int64_t> after_;    // where the conversation's checkpoints are once that read is done
    int64_t turn_ = -1;             // turn_at()'s place in the prompt being read; -1: none
};

}  // namespace strata::poly
