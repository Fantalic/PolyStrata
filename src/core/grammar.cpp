// grammar.cpp — GBNF grammar-constrained decoding engine.
// Implements a PDA over GBNF rules; per step computes which token ids may
// legally follow given a byte-level token vocabulary (llama.cpp-style).
//
// Design
// ------
// * GBNF text is parsed into rules (GrammarRule), each a vector of
//   GrammarElements.  Char/CharNot/CharRngUpper carry a Unicode codepoint
//   in 'value'.  RuleRef carries the target rule id.
//
// * At runtime the grammar maintains a set of stacks (std::vector<Stack>).
//   Each Stack is a list of StackElem{rule_id, pos}.  We look at the top
//   element of every stack, collect all tokens whose bytes can be consumed
//   by that element, and OR them into m_mask.
//
// * UTF-8 partials: a token may end mid-codepoint.  We track
//   m_partial_value (accumulated bytes) and m_n_remain (continuation bytes
//   still expected).  A token whose leading byte is a valid UTF-8 prefix
//   is allowed when the grammar expects a multi-byte character.  A token
//   whose first byte is a continuation byte (0x80-0xBF) is allowed only
//   when m_n_remain > 0.
//
// Repetition (* + ?) is handled during parsing by duplicating the matched
// element(s) in the rule; the duplicated copy creates the loop so the PDA
// can re-visit the first copy.  An Alt element is appended for * and ?.

#include "strata/core/grammar.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace strata::core {

// ── CacheKeyHash ──────────────────────────────────────────────────────────

size_t CacheKeyHash::operator()(const CacheKey& k) const {
    size_t h = 0;
    auto mix = [&](uint64_t v) {
        v += 0x9e3779b1 + (h << 6) + (h >> 2);
        h ^= v;
    };
    for (const auto& stk : k.stacks)
        for (const auto& e : stk) { mix(e.rule_id); mix(e.pos); }
    mix(k.partial_value);
    mix(k.n_remain);
    return h;
}

// ── UTF-8 helpers ─────────────────────────────────────────────────────────

static int utf8_byte_count(uint32_t cp) {
    if (cp <= 0x7F) return 1;
    if (cp <= 0x7FF) return 2;
    if (cp <= 0xFFFF) return 3;
    return 4;
}

static int utf8_encode(uint32_t cp, uint8_t* bytes) {
    if (cp <= 0x7F) { bytes[0] = static_cast<uint8_t>(cp); return 1; }
    if (cp <= 0x7FF) {
        bytes[0] = 0xC0 | static_cast<uint8_t>(cp >> 6);
        bytes[1] = 0x80 | static_cast<uint8_t>(cp & 0x3F);
        return 2;
    }
    if (cp <= 0xFFFF) {
        bytes[0] = 0xE0 | static_cast<uint8_t>(cp >> 12);
        bytes[1] = 0x80 | static_cast<uint8_t>((cp >> 6) & 0x3F);
        bytes[2] = 0x80 | static_cast<uint8_t>(cp & 0x3F);
        return 3;
    }
    bytes[0] = 0xF0 | static_cast<uint8_t>(cp >> 18);
    bytes[1] = 0x80 | static_cast<uint8_t>((cp >> 12) & 0x3F);
    bytes[2] = 0x80 | static_cast<uint8_t>((cp >> 6) & 0x3F);
    bytes[3] = 0x80 | static_cast<uint8_t>(cp & 0x3F);
    return 4;
}

// ── Grammar parser helpers (free functions, called from member parse()) ──

// Forward declarations for mutual recursion.
static void parse_alternatives(std::string_view src, size_t& i,
                               std::vector<GrammarElement>* out,
                               std::unordered_map<std::string, size_t>* name_to_id,
                               std::string& err);
static void parse_alternative(std::string_view src, size_t& i,
                               std::vector<GrammarElement>* out,
                               std::unordered_map<std::string, size_t>* name_to_id,
                               std::string& err);

static void skip_ws(std::string_view src, size_t& i) {
    while (i < src.size()) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '#') { while (i < src.size() && src[i] != '\n') i++; continue; }
        break;
    }
}

static uint32_t parse_hex2(std::string_view src, size_t& i, std::string& err) {
    uint32_t v = 0;
    for (int j = 0; j < 2; j++) {
        if (i >= src.size()) { err = "trunc hex"; return 0; }
        char h = src[i++];
        v <<= 4;
        if (h >= '0' && h <= '9') v |= h - '0';
        else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
        else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
        else { err = "bad hex"; return 0; }
    }
    return v;
}

static uint32_t parse_escape(std::string_view src, size_t& i, std::string& err) {
    if (i >= src.size()) { err = "trailing backslash"; return 0; }
    if (src[i] != '\\') { err = "not an escape"; return 0; }
    i++;                                  // the backslash itself
    if (i >= src.size()) { err = "trunc escape"; return 0; }
    char c = src[i++];                    // the escape character
    switch (c) {
        case '"': return '"';
        case '\\': return '\\';
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case 'x': return parse_hex2(src, i, err);
        case 'u': {
            uint32_t v = 0;
            for (int j = 0; j < 4; j++) {
                if (i >= src.size()) { err = "trunc \\u"; return 0; }
                v <<= 4;
                char h = src[i++];
                if (h >= '0' && h <= '9') v |= h - '0';
                else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
                else { err = "bad \\u"; return 0; }
            }
            return v;
        }
        default: err = "bad escape"; return 0;
    }
}

static uint32_t parse_char_val(std::string_view src, size_t& i, std::string& err) {
    if (src[i] == '\\') return parse_escape(src, i, err);
    uint32_t v = static_cast<uint32_t>(static_cast<unsigned char>(src[i++]));
    return v;
}

static void parse_quoted(std::string_view src, size_t& i,
                         std::vector<GrammarElement>* out, std::string& err) {
    char q = src[i++];
    while (i < src.size() && src[i] != q) {
        uint32_t cp = parse_char_val(src, i, err);
        if (!err.empty()) return;
        out->push_back({ElemType::Char, cp});
    }
    if (i >= src.size()) { err = "unterminated string"; return; }
    i++; // closing quote
}

static void parse_char_class(std::string_view src, size_t& i,
                             std::vector<GrammarElement>* out, std::string& err) {
    i++; // skip [
    bool neg = false;
    if (i < src.size() && src[i] == '^') { neg = true; i++; }
    ElemType base = neg ? ElemType::CharNot : ElemType::Char;
    while (i < src.size() && src[i] != ']') {
        uint32_t lo = parse_char_val(src, i, err);
        if (!err.empty()) return;
        if (i + 1 < src.size() && src[i] == '-' && src[i + 1] != ']') {
            i++; // skip '-'
            uint32_t hi = parse_char_val(src, i, err);
            if (!err.empty()) return;
            out->push_back({base, lo});
            out->push_back({ElemType::CharRngUpper, hi});
        } else {
            out->push_back({base, lo});
            out->push_back({ElemType::CharAlt, 0});
        }
    }
    if (i >= src.size()) { err = "unterminated char class"; return; }
    i++; // skip ]
}

static void parse_group(std::string_view src, size_t& i,
                        std::vector<GrammarElement>* out,
                        std::unordered_map<std::string, size_t>* name_to_id,
                        std::string& err);

static void parse_element(std::string_view src, size_t& i,
                          std::vector<GrammarElement>* out,
                          std::unordered_map<std::string, size_t>* name_to_id,
                          std::string& err) {
    if (i >= src.size()) { err = "unexpected end"; return; }
    size_t start = out->size();
    char c = src[i];
    if (c == '"') parse_quoted(src, i, out, err);
    else if (c == '\'') parse_quoted(src, i, out, err);
    else if (c == '[') parse_char_class(src, i, out, err);
    else if (c == '(') parse_group(src, i, out, name_to_id, err);
    else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        size_t ns = i;
        while (i < src.size() && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_')) i++;
        std::string nm(src.data() + ns, i - ns);
        auto it = name_to_id->find(nm);
        if (it == name_to_id->end()) { err = "unknown rule: " + nm; return; }
        out->push_back({ElemType::RuleRef, static_cast<uint32_t>(it->second)});
    } else { err = "unexpected char at " + std::to_string(i) + " (byte " + std::to_string((int)(unsigned char)src[i]) + ")"; return; }
    if (!err.empty()) return;

    if (i < src.size() && (src[i] == '*' || src[i] == '+' || src[i] == '?')) {
        char rep = src[i++];
        size_t end = out->size();
        for (size_t j = start; j < end; j++)
            out->push_back(out->operator[](j));
        if (rep == '*' || rep == '?')
            out->push_back({ElemType::Alt, 0});
    }
}

static void parse_group(std::string_view src, size_t& i,
                        std::vector<GrammarElement>* out,
                        std::unordered_map<std::string, size_t>* name_to_id,
                        std::string& err) {
    i++; // skip (
    parse_alternatives(src, i, out, name_to_id, err);
    if (!err.empty()) return;
    if (i >= src.size() || src[i] != ')') { err = "unbalanced paren"; return; }
    i++; // skip )
}

static void parse_alternative(std::string_view src, size_t& i,
                              std::vector<GrammarElement>* out,
                              std::unordered_map<std::string, size_t>* name_to_id,
                              std::string& err) {
    while (i < src.size() && src[i] != '|' && src[i] != ')' && src[i] != '\n') {
        skip_ws(src, i);
        parse_element(src, i, out, name_to_id, err);
        if (!err.empty()) return;
    }
}

static void parse_alternatives(std::string_view src, size_t& i,
                               std::vector<GrammarElement>* out,
                               std::unordered_map<std::string, size_t>* name_to_id,
                               std::string& err) {
    parse_alternative(src, i, out, name_to_id, err);
    if (!err.empty()) return;
    while (i < src.size() && src[i] == '|') { i++; parse_alternative(src, i, out, name_to_id, err); if (!err.empty()) return; }
}

// ── Grammar::parse ────────────────────────────────────────────────────────

bool Grammar::parse(const std::string& gbnf, Grammar& out, std::string& error) {
    out.m_rules.clear();
    if (gbnf.empty()) { error = "empty grammar"; return false; }

    // First pass: collect rule names.
    std::unordered_map<std::string, size_t> names;
    {
        size_t pi = 0;
        while (pi < gbnf.size()) {
            skip_ws(gbnf, pi);
            if (pi >= gbnf.size()) break;
            size_t ns = pi;
            while (pi < gbnf.size() && (std::isalnum(static_cast<unsigned char>(gbnf[pi])) || gbnf[pi] == '_')) pi++;
            if (pi == ns) { error = "expected rule name"; return false; }
            std::string nm(gbnf.data() + ns, pi - ns);
            skip_ws(gbnf, pi);
            if (std::getenv("STRATA_GRAMMAR_TRACE")) std::fprintf(stderr, "[G] pass1 head nm='%s' pi=%zu size=%zu c1=%d c2=%d c3=%d c4=%d\n",
                                      nm.c_str(), pi, gbnf.size(),
                                      (int)(pi + 3 > gbnf.size()),
                                      (int)(gbnf[pi] != ':'),
                                      (int)(gbnf[pi+1] != '='),
                                      (int)(gbnf[pi+2] != '='));
            if (std::getenv("STRATA_GRAMMAR_TRACE")) std::fprintf(stderr, "[G] bytes at pi: %d %d %d\n",
                                      (int)(unsigned char)gbnf[pi],
                                      (int)(unsigned char)gbnf[pi+1],
                                      (int)(unsigned char)gbnf[pi+2]);
            if (pi + 3 > gbnf.size() || gbnf[pi] != ':' || gbnf[pi+1] != ':' || gbnf[pi+2] != '=') { error = "expected ::= (pass 1 rule '" + nm + "' at " + std::to_string(pi) + " byte " + std::to_string((int)(unsigned char)gbnf[pi]) + ")"; return false; }
            pi += 3;
            while (pi < gbnf.size() && gbnf[pi] != '\n') pi++;   // skip the rule body: heads only
            if (!names.emplace(nm, names.size()).second) {
                error = "duplicate rule: " + nm;
                return false;
            }
        }
    }

    // Second pass: full parse.
    out.m_rules.clear();
    std::unordered_map<std::string, size_t> nm2(names); // copy
    size_t pi = 0;
    while (pi < gbnf.size()) {
        skip_ws(gbnf, pi);
        if (pi >= gbnf.size()) break;

        size_t ns = pi;
        while (pi < gbnf.size() && (std::isalnum(static_cast<unsigned char>(gbnf[pi])) || gbnf[pi] == '_')) pi++;
        if (pi == ns) { error = "expected rule name"; return false; }
        std::string nm(gbnf.data() + ns, pi - ns);
        skip_ws(gbnf, pi);

        if (pi + 3 > gbnf.size() || gbnf[pi] != ':' || gbnf[pi+1] != ':' || gbnf[pi+2] != '=') { error = "expected ::= (pass 2 at " + std::to_string(pi) + ")"; return false; }
        pi += 3;

        size_t rid = out.m_rules.size();
        out.m_rules.emplace_back();
        // rule ids were assigned by the first pass (same scan order)

        std::vector<GrammarElement> elems;
        parse_alternatives(gbnf, pi, &elems, &nm2, error);
        if (!error.empty()) return false;
        elems.push_back({ElemType::End, 0});
        out.m_rules[rid] = std::move(elems);

        // Left-recursion check: the rule may not reach itself from a
        // leftmost position (start of the rule or right after an Alt
        // separator, before any consuming element of that alternative).
        // Forward references to later rules are legal.
        {
            bool left = true;
            for (const auto& e : out.m_rules[rid]) {
                if (e.type == ElemType::Alt) {
                    left = true;
                } else if (e.type == ElemType::RuleRef) {
                    if (left && e.value == rid) {
                        error = "left recursion detected";
                        return false;
                    }
                    left = false;
                } else if (e.type != ElemType::End) {
                    left = false;
                }
            }
        }
    }

    if (out.m_rules.empty()) { error = "empty grammar"; return false; }
    return true;
}

// ── set_vocab ─────────────────────────────────────────────────────────────

void Grammar::set_vocab(std::vector<std::string> token_bytes, std::vector<int> eog_ids) {
    m_tokens = std::move(token_bytes);
    m_eog_ids = std::move(eog_ids);
    m_vocab_size = static_cast<int>(m_tokens.size());
    m_mask.assign((m_vocab_size + 63) / 64, 0ULL);
    m_mask_valid = false;
    m_terminal = false;
    m_partial_value = 0;
    m_n_remain = 0;
    m_cache.clear();

    // Initialise stacks from the root rule (rule 0).
    m_stacks.clear();
    Stack s;
    s.push_back({0, 0});
    m_stacks.push_back(std::move(s));
}

// ── match_char ────────────────────────────────────────────────────────────

bool Grammar::match_char(const GrammarElement* pos, uint32_t cp) const {
    switch (pos->type) {
        case ElemType::Char: return pos->value == cp;
        case ElemType::CharNot: return pos->value != cp;
        case ElemType::CharRngUpper: return false; // needs lower bound from caller
        case ElemType::CharAlt: return true; // neutral separator
        default: return false;
    }
}

// ── match_partial_char ────────────────────────────────────────────────────

bool Grammar::match_partial_char(const GrammarElement* pos) const {
    // Called when m_n_remain > 0 (inside a multi-byte codepoint).
    // A continuation byte token (first byte 0x80-0xBF) is valid here.
    uint32_t cp = pos->value;
    return utf8_byte_count(cp) > 1;
}

// ── token helper functions ────────────────────────────────────────────────

static bool token_matches_char(const std::string& tok, uint32_t cp) {
    int need = utf8_byte_count(cp);
    if (static_cast<int>(tok.size()) == need) {
        uint8_t enc[4];
        utf8_encode(cp, enc);
        return std::memcmp(tok.data(), enc, need) == 0;
    }
    if (static_cast<int>(tok.size()) < need && tok.size() == 1) {
        uint8_t enc[4];
        utf8_encode(cp, enc);
        return static_cast<uint8_t>(tok[0]) == enc[0];
    }
    return false;
}

static bool token_matches_char_not(const std::string& tok, uint32_t cp) {
    int need = utf8_byte_count(cp);
    if (static_cast<int>(tok.size()) == need) {
        uint8_t enc[4];
        utf8_encode(cp, enc);
        return std::memcmp(tok.data(), enc, need) != 0;
    }
    if (static_cast<int>(tok.size()) < need && tok.size() == 1) {
        uint8_t enc[4];
        utf8_encode(cp, enc);
        return static_cast<uint8_t>(tok[0]) != enc[0];
    }
    return true; // token longer than negated char encoding — it's different
}

static bool token_matches_range(const std::string& tok, uint32_t lower_cp, uint32_t upper_cp) {
    if (tok.empty()) return false;
    uint8_t first = static_cast<uint8_t>(tok[0]);
    if (first <= 0x7F) {
        return first >= static_cast<uint8_t>(lower_cp) &&
               first <= static_cast<uint8_t>(upper_cp);
    }
    // For multi-byte: check if any codepoint in range has this leading byte.
    for (uint32_t cp = lower_cp; cp <= upper_cp && cp <= 0x10FFFF; cp++) {
        int need = utf8_byte_count(cp);
        if (need == 1) {
            if (static_cast<uint8_t>(cp) == first) return true;
        } else {
            uint8_t enc[4];
            utf8_encode(cp, enc);
            if (enc[0] == first) return true;
        }
    }
    return false;
}

bool Grammar::match_token(const GrammarElement* pos, uint32_t token_id) const {
    if (token_id >= static_cast<uint32_t>(m_vocab_size)) return false;
    const std::string& tok = m_tokens[token_id];
    if (tok.empty()) return false;

    switch (pos->type) {
        case ElemType::Char: return token_matches_char(tok, pos->value);
        case ElemType::CharNot: return token_matches_char_not(tok, pos->value);
        case ElemType::CharRngUpper: return false; // handled in compute_mask
        case ElemType::CharAlt: return false;
        case ElemType::RuleRef: return false;
        case ElemType::End: case ElemType::Alt: return false;
        default: return false;
    }
}

// ── advance_stack ─────────────────────────────────────────────────────────

void Grammar::advance_stack(const Stack& stack, uint8_t byte, std::vector<Stack>& out) const {
    if (stack.empty()) return;
    const StackElem& top = stack.back();
    const GrammarRule& rule = m_rules[top.rule_id];
    if (top.pos >= rule.size()) return;

    const GrammarElement& elem = rule[top.pos];
    Stack ns(stack);

    switch (elem.type) {
        case ElemType::End:
            if (stack.size() > 1) { ns.pop_back(); out.push_back(std::move(ns)); }
            break;
        case ElemType::Alt: {
            size_t nxt = top.pos + 1;
            if (nxt < rule.size()) { ns.back().pos = nxt; out.push_back(std::move(ns)); }
            break;
        }
        case ElemType::RuleRef: {
            ns.push_back({elem.value, 0});
            out.push_back(std::move(ns));
            break;
        }
        default: {
            ns.back().pos++;
            out.push_back(std::move(ns));
            break;
        }
    }
}

// ── compute_mask ──────────────────────────────────────────────────────────

void Grammar::compute_mask() const {
    m_mask.assign((m_vocab_size + 63) / 64, 0ULL);

    // Check cache.
    CacheKey key{m_stacks, m_partial_value, m_n_remain};
    auto it = m_cache.find(key);
    if (it != m_cache.end()) { m_mask = it->second; m_mask_valid = true; return; }

    for (const auto& stack : m_stacks) {
        if (stack.empty()) continue;
        const StackElem& top = stack.back();
        const GrammarRule& rule = m_rules[top.rule_id];
        if (top.pos >= rule.size()) continue;

        const GrammarElement& elem = rule[top.pos];

        // EOG allowed when grammar can end (End or Alt at top).
        bool can_end = (elem.type == ElemType::End || elem.type == ElemType::Alt);

        for (int tid = 0; tid < m_vocab_size; tid++) {
            const std::string& tok = m_tokens[tid];

            // EOG tokens.
            if (tok.empty()) {
                if (can_end) m_mask[tid / 64] |= (1ULL << (tid % 64));
                continue;
            }

            // Continuation-byte tokens: allowed when m_n_remain > 0
            // and the grammar element expects a multi-byte char.
            bool is_cont = (static_cast<uint8_t>(tok[0]) & 0xC0) == 0x80;
            if (is_cont && m_n_remain > 0) {
                switch (elem.type) {
                    case ElemType::Char:
                    case ElemType::CharNot:
                        if (utf8_byte_count(elem.value) > 1)
                            m_mask[tid / 64] |= (1ULL << (tid % 64));
                        break;
                    case ElemType::CharRngUpper: {
                        // Check if any codepoint in [lower, upper] is multi-byte.
                        if (top.pos > 0) {
                            const GrammarElement& prev = rule[top.pos - 1];
                            if (prev.type == ElemType::Char || prev.type == ElemType::CharNot) {
                                for (uint32_t c = prev.value; c <= elem.value && c <= 0x10FFFF; c++) {
                                    if (utf8_byte_count(c) > 1) {
                                        m_mask[tid / 64] |= (1ULL << (tid % 64));
                                        break;
                                    }
                                }
                            }
                        }
                        break;
                    }
                    default: break;
                }
                continue;
            }

            // Normal tokens.
            switch (elem.type) {
                case ElemType::Char:
                    if (token_matches_char(tok, elem.value))
                        m_mask[tid / 64] |= (1ULL << (tid % 64));
                    break;
                case ElemType::CharNot:
                    if (token_matches_char_not(tok, elem.value))
                        m_mask[tid / 64] |= (1ULL << (tid % 64));
                    break;
                case ElemType::CharRngUpper: {
                    if (top.pos > 0) {
                        const GrammarElement& prev = rule[top.pos - 1];
                        if (prev.type == ElemType::Char || prev.type == ElemType::CharNot)
                            if (token_matches_range(tok, prev.value, elem.value))
                                m_mask[tid / 64] |= (1ULL << (tid % 64));
                    }
                    break;
                }
                default: break;
            }
        }
    }

    m_cache[key] = m_mask;
    m_mask_valid = true;
}

// ── allowed_mask ──────────────────────────────────────────────────────────

const std::vector<unsigned long long>& Grammar::allowed_mask() {
    if (m_terminal) {
        // Return an all-zero mask (static to avoid reallocation).
        static thread_local std::vector<unsigned long long> empty;
        if (empty.empty()) empty.assign((m_vocab_size + 63) / 64, 0ULL);
        return empty;
    }
    if (!m_mask_valid) compute_mask();
    return m_mask;
}

// ── accept ────────────────────────────────────────────────────────────────

void Grammar::accept(int token_id) {
    if (m_terminal) return;

    // Check if token was actually allowed.
    auto& mask = const_cast<std::vector<unsigned long long>&>(allowed_mask());
    bool allowed = false;
    if (token_id >= 0 && token_id < m_vocab_size)
        allowed = (mask[token_id / 64] >> (token_id % 64)) & 1ULL;
    if (!allowed) return; // no-op when token not in mask.

    // EOG → terminal.
    for (int eid : m_eog_ids)
        if (eid == token_id) { m_terminal = true; m_mask_valid = false; return; }

    // Advance all stacks.
    std::vector<Stack> new_stacks;
    for (const auto& stk : m_stacks)
        advance_stack(stk, 0, new_stacks); // byte unused by the baseline runtime; the new runtime uses it

    // Update partial UTF-8 state.
    const std::string& tok = m_tokens[token_id];
    if (!tok.empty()) {
        uint8_t first = static_cast<uint8_t>(tok[0]);
        // Check if this starts a multi-byte sequence.
        if ((first & 0xE0) == 0xC0 || (first & 0xF0) == 0xE0 || (first & 0xF8) == 0xF0) {
            int bytes = utf8_byte_count(first);
            if (static_cast<int>(tok.size()) < bytes) {
                m_partial_value = first;
                m_n_remain = bytes - 1;
            } else {
                m_partial_value = 0;
                m_n_remain = 0;
            }
        }
    }

    m_stacks = std::move(new_stacks);
    m_mask_valid = false;
}

// ── accepting ─────────────────────────────────────────────────────────────

bool Grammar::accepting() const {
    if (m_terminal) return true;
    for (const auto& stack : m_stacks) {
        if (stack.empty()) return true;
        const StackElem& top = stack.back();
        const GrammarRule& rule = m_rules[top.rule_id];
        if (top.pos >= rule.size()) return true;
        if (rule[top.pos].type == ElemType::End || rule[top.pos].type == ElemType::Alt)
            return true;
    }
    return false;
}

// ── vocab_size ────────────────────────────────────────────────────────────

int Grammar::vocab_size() const { return m_vocab_size; }

} // namespace strata::core