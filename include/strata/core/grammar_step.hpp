#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "strata/core/grammar.hpp"

namespace strata::core {

// Grammar-constrained decoding helper: parses a hex-encoded GBNF grammar,
// masks logits to disallowed tokens, and advances the grammar state on
// each picked token.  Header-only, single responsibility.
//
// Usage (one instance per request):
//   GrammarStep gs;
//   std::string err;
//   if (!gs.set(hex, vocab, eog, err)) { /* error */ }
//   gs.mask(logits, n_vocab);
//   int32_t t = pick(logits, n_vocab);
//   if (!gs.accept(t)) { /* grammar rejected forced token */ }

class GrammarStep {
public:
    // Decode `hex` (hex-encoded GBNF text), parse it, set the vocab and EOG
    // ids.  On success returns true and the step is active.  On failure
    // `err` describes why.
    bool set(const std::string& hex, const std::vector<std::string>& vocab,
             const std::vector<int32_t>& eog, std::string& err);

    // Apply the grammar mask to the `logits` buffer (n_vocab entries):
    // every grammar-banned id is clamped to -1e30f.  No-op when inactive.
    // (Non-const: the grammar's lazy mask cache may fill on first use.)
    void mask(float* logits, int n_vocab);

    // Feed the picked token id to the grammar.  Returns false if the token
    // was not allowed by the grammar (a forced pick or a bug).
    bool accept(int32_t id);

    // True when a grammar is active (set() succeeded).
    bool active() const;

private:
    Grammar g_;
    bool active_ = false;
};

// ── Implementation (header-only) ──────────────────────────────────────────

inline bool GrammarStep::set(const std::string& hex,
                             const std::vector<std::string>& vocab,
                             const std::vector<int32_t>& eog,
                             std::string& err) {
    // 1. Empty hex check.
    if (hex.empty()) {
        err = "empty grammar";
        return false;
    }

    // 2. Hex-decode: length must be even, every nibble valid.
    if (hex.size() % 2 != 0) {
        err = "bad hex encoding";
        return false;
    }
    std::string text;
    text.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            err = "bad hex encoding";
            return false;
        }
        text += static_cast<char>((hi << 4) | lo);
    }

    // 3. Vocab must not be empty.
    if (vocab.empty()) {
        err = "engine has no token vocabulary";
        return false;
    }

    // 3b. Every EOG id must be inside the vocabulary (the engine's token list
    // contains its eog tokens, so this holds for real vocabularies).
    for (int32_t id : eog) {
        if (id < 0 || id >= (int)vocab.size()) {
            err = "eog id out of vocabulary range";
            return false;
        }
    }

    // 4. Parse the GBNF grammar.
    std::string parse_err;
    if (!Grammar::parse(text, g_, parse_err)) {
        err = parse_err.empty() ? "parse error" : parse_err;
        return false;
    }

    // 5. Set vocab and EOG ids, mark active.
    g_.set_vocab(vocab, std::vector<int>(eog.begin(), eog.end()));
    active_ = true;
    return true;
}

inline void GrammarStep::mask(float* logits, int n_vocab) {
    if (!active_) return;
    g_.apply_logit_mask(logits, n_vocab);
}

inline bool GrammarStep::accept(int32_t id) {
    if (!active_) return true;
    if (!g_.is_allowed(id)) return false;
    return g_.accept(id);   // false: the token's text overran the grammar — ban it
}

inline bool GrammarStep::active() const { return active_; }

} // namespace strata::core
