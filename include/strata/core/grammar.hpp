#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace strata::core {

// GGUF token strings store sentencepiece byte-fallback tokens as the literal
// text "<0xHH>"; the grammar module needs the raw byte. Returns the token's
// byte content: decoded when the whole string is a well-formed <0xHH> token,
// otherwise unchanged. (llama.cpp applies the same convention.)
inline std::string decode_token_bytes(const std::string& tok) {
    if (tok.size() == 6 && tok[0] == '<' && tok[1] == '0' && tok[2] == 'x' && tok[5] == '>') {
        auto hv = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = hv(tok[3]), lo = hv(tok[4]);
        if (hi >= 0 && lo >= 0) return std::string(1, (char)((hi << 4) | lo));
    }
    return tok;
}

// Grammar-constrained decoding: parse GBNF text, maintain a pushdown-automaton
// state, and per-step compute which token ids may legally follow given a byte-
// level token vocabulary.  Algorithm follows llama.cpp's grammar semantics.
//
// Design notes
// ------------
// * One Grammar instance per request.  `set_vocab` must be called before use.
// * `allowed_mask()` returns a bit-vector indexed by token id.  Zero bits
//   means "banned".  The caller must never reach a state where every bit is
//   zero — that is a grammar bug.
// * After accepting an EOG token the grammar becomes terminal: every later
//   call to `allowed_mask()` returns an empty (all-zero) mask and
//   `accepting()` stays true.
//

// --- Nested types moved to namespace scope for use by free functions ------

struct StackElem { uint32_t rule_id = 0; size_t pos = 0; };

inline bool operator==(const StackElem& a, const StackElem& b) {
    return a.rule_id == b.rule_id && a.pos == b.pos;
}

inline bool operator<(const StackElem& a, const StackElem& b) {
    return a.rule_id != b.rule_id ? a.rule_id < b.rule_id : a.pos < b.pos;
}

using Stack = std::vector<StackElem>;

struct CacheKey {
    std::vector<Stack> stacks;
    uint32_t partial_value = 0;
    int      n_remain      = 0;
};

struct CacheKeyHash {
    size_t operator()(const CacheKey& k) const;
};

inline bool operator==(const CacheKey& a, const CacheKey& b) {
    return a.stacks == b.stacks &&
           a.partial_value == b.partial_value &&
           a.n_remain == b.n_remain;
}

enum class ElemType : uint8_t {
    End, Alt, RuleRef, Char, CharNot,
    CharRngUpper, CharAlt, Token, TokenNot,
};

struct GrammarElement { ElemType type; uint32_t value = 0; };
using GrammarRule = std::vector<GrammarElement>;

// --- Main class ---------------------------------------------------------

class Grammar {
public:
    Grammar() = default;

    // Parse `gbnf` text into `out`.  Returns false and fills `error` on bad
    // grammar (empty text, unbalanced parens, unknown rule reference, left
    // recursion, unsupported constructs like `~[` or `[` without `]`).
    static bool parse(const std::string& gbnf, Grammar& out, std::string& error);

    // Provide the token vocabulary (token id → raw byte string) and the set of
    // end-of-generation token ids.  Must be called before `allowed_mask` /
    // `accept`.
    void set_vocab(std::vector<std::string> token_bytes, std::vector<int> eog_ids);

    // Bitmap over token ids: bit v set means token v may be sampled next.
    // Stable reference valid until the next `accept()` or `set_vocab()`.
    // Zero bits (all tokens banned) is a hard-error condition — document it
    // so the caller knows to treat it as a bug.
    const std::vector<unsigned long long>& allowed_mask();

    // Advance the state by one accepted token.  The caller must only pass a
    // token whose bit was set in `allowed_mask()`.  If the token has no set
    // bit the state is left unchanged (no-op).  After accepting an EOG token
    // the grammar becomes terminal.
    void accept(int token_id);

    // True when the grammar can end at the current state (some rule stack is
    // complete); EOG tokens are then part of `allowed_mask()`.
    bool accepting() const;

    // Vocab size the mask covers (0 before `set_vocab`).
    int vocab_size() const;

private:
    mutable std::vector<Stack> m_stacks;
    uint32_t m_partial_value = 0;
    int      m_n_remain      = 0;

    std::vector<std::string> m_tokens;
    std::vector<int>         m_eog_ids;
    int m_vocab_size = 0;

    std::vector<GrammarRule> m_rules;

    bool m_terminal = false;
    mutable bool m_mask_valid = false;

    mutable std::vector<unsigned long long> m_mask;

    mutable std::unordered_map<CacheKey, std::vector<unsigned long long>,
                               CacheKeyHash> m_cache;

    bool match_char(const GrammarElement* pos, uint32_t chr) const;
    bool match_partial_char(const GrammarElement* pos) const;
    bool match_token(const GrammarElement* pos, uint32_t token_id) const;

    // Advance a single stack by one byte: match the byte against the
    // top element, advance past the matched element (and any range
    // suffix), expand RuleRefs, pop on End — then dedup via sort+unique.
    void advance_stack(const Stack& stack, uint8_t byte,
                       std::vector<Stack>& out) const;
    void compute_mask() const;
};

} // namespace strata::core
