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
// Repetition (* + ?) is handled during parsing by synthesizing auxiliary
// rules via the grammar-rule vector.  Each quantifier gets a new rule id;
// the parent rule receives a RuleRef to it instead of duplicated atoms.
// The runtime splits on Alt separators for alternatives and RuleRefs.

#include "strata/core/grammar.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <array>
#include <map>
#include <set>
#include <utility>
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
    for (const auto& e : k.tops) { mix(e.rule_id); mix(e.pos); }
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
                               std::vector<GrammarRule>* rules,
                               std::string& err);
static void parse_alternative(std::string_view src, size_t& i,
                               std::vector<GrammarElement>* out,
                               std::unordered_map<std::string, size_t>* name_to_id,
                               std::vector<GrammarRule>* rules,
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
        if (cp > 0xFF) {
            // Codepoint escape (e.g. \uXXXX): lower to its UTF-8 bytes so each
            // element matches one token byte.
            uint8_t seq[4];
            int n = utf8_encode(cp, seq);
            for (int b = 0; b < n; ++b) out->push_back({ElemType::Char, seq[b]});
        } else {
            out->push_back({ElemType::Char, cp});
        }
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
            if (lo > 0xFF || hi > 0xFF) {
                err = "class ranges on codepoints > 0xFF are not supported; "
                      "use raw characters or byte ranges";
                return;
            }
            out->push_back({base, lo});
            out->push_back({ElemType::CharRngUpper, hi});
        } else {
            if (lo > 0xFF) {
                // Codepoint escape in char class: lower to UTF-8 bytes.
                uint8_t seq[4];
                int n = utf8_encode(lo, seq);
                for (int b = 0; b < n; ++b) {
                    out->push_back({base, seq[b]});
                    out->push_back({ElemType::CharAlt, 0});
                }
            } else {
                out->push_back({base, lo});
                out->push_back({ElemType::CharAlt, 0});
            }
        }
    }
    if (i >= src.size()) { err = "unterminated char class"; return; }
    i++; // skip ]
}

static void parse_group(std::string_view src, size_t& i,
                         std::vector<GrammarElement>* out,
                         std::unordered_map<std::string, size_t>* name_to_id,
                         std::vector<GrammarRule>* rules,
                         std::string& err);

static void parse_element(std::string_view src, size_t& i,
                           std::vector<GrammarElement>* out,
                           std::unordered_map<std::string, size_t>* name_to_id,
                           std::vector<GrammarRule>* rules,
                           std::string& err) {
    if (i >= src.size()) { err = "unexpected end"; return; }
    size_t start = out->size();
    char c = src[i];
    if (c == '"') parse_quoted(src, i, out, err);
    else if (c == '\'') parse_quoted(src, i, out, err);
    else if (c == '[') parse_char_class(src, i, out, err);
    else if (c == '(') parse_group(src, i, out, name_to_id, rules, err);
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
        const size_t atoms_begin = start;
        const uint32_t aux_id = static_cast<uint32_t>(rules->size());
        rules->emplace_back();
        std::vector<GrammarElement>& aux = rules->back();
        if (rep == '?') {
            // S' ::= S |      (empty last alternative = zero path)
            aux.assign(out->begin() + static_cast<long>(atoms_begin), out->end());
            aux.push_back({ElemType::Alt, 0});
            out->erase(out->begin() + static_cast<long>(atoms_begin), out->end());
            out->push_back({ElemType::RuleRef, aux_id});
        } else if (rep == '*') {
            // S' ::= S S' |   (zero or more)
            aux.assign(out->begin() + static_cast<long>(atoms_begin), out->end());
            aux.push_back({ElemType::RuleRef, aux_id});
            aux.push_back({ElemType::Alt, 0});
            out->erase(out->begin() + static_cast<long>(atoms_begin), out->end());
            out->push_back({ElemType::RuleRef, aux_id});
        } else { // '+'
            // S S'  with  S' ::= S S' |   (one or more)
            aux.assign(out->begin() + static_cast<long>(atoms_begin), out->end());
            aux.push_back({ElemType::RuleRef, aux_id});
            aux.push_back({ElemType::Alt, 0});
            out->push_back({ElemType::RuleRef, aux_id});   // atoms stay in the parent
        }
    }
}

static void parse_group(std::string_view src, size_t& i,
                         std::vector<GrammarElement>* out,
                         std::unordered_map<std::string, size_t>* name_to_id,
                         std::vector<GrammarRule>* rules,
                         std::string& err) {
    i++; // skip (
    parse_alternatives(src, i, out, name_to_id, rules, err);
    if (!err.empty()) return;
    if (i >= src.size() || src[i] != ')') { err = "unbalanced paren"; return; }
    i++; // skip )
}

static void parse_alternative(std::string_view src, size_t& i,
                               std::vector<GrammarElement>* out,
                               std::unordered_map<std::string, size_t>* name_to_id,
                               std::vector<GrammarRule>* rules,
                               std::string& err) {
    while (i < src.size() && src[i] != '|' && src[i] != ')' && src[i] != '\n' && src[i] != '\r') {
        skip_ws(src, i);
        if (i >= src.size() || src[i] == '|' || src[i] == ')' || src[i] == '\n' || src[i] == '\r') break;
        parse_element(src, i, out, name_to_id, rules, err);
        if (!err.empty()) return;
    }
}

static void parse_alternatives(std::string_view src, size_t& i,
                                std::vector<GrammarElement>* out,
                                std::unordered_map<std::string, size_t>* name_to_id,
                                std::vector<GrammarRule>* rules,
                                std::string& err) {
    parse_alternative(src, i, out, name_to_id, rules, err);
    if (!err.empty()) return;
    while (i < src.size() && src[i] == '|') {
        i++;
        out->push_back({ElemType::Alt, 0});
        parse_alternative(src, i, out, name_to_id, rules, err);
        if (!err.empty()) return;
    }
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
    out.m_rules.assign(names.size(), {});   // named rules at first-pass ids; aux append after
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

        const size_t rid = names[nm];   // first-pass id (scan order matches)
        std::vector<GrammarElement> elems;
        parse_alternatives(gbnf, pi, &elems, &nm2, &out.m_rules, error);
        if (!error.empty()) return false;
        if (elems.empty()) {
            error = "rule '" + nm + "' has no right-hand side";
            return false;
        }
        elems.push_back({ElemType::End, 0});
        out.m_rules[rid] = std::move(elems);

        // Aux rules (from quantifiers) live after named rules.
        while (out.m_rules.size() > names.size()) {
            out.m_rules.back().push_back({ElemType::End, 0});
            break;
        }
    }
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

    // Precompute the vocab-shaped structures the mask fills use: EOG flags,
    // the EOG token list (empty-string tokens are legacy EOG, see
    // compute_mask), and non-EOG tokens grouped by first byte so mask fills
    // are bucket ORs instead of a vocab-wide string walk per stack per step.
    m_is_eog.assign((size_t) m_vocab_size, 0);
    for (int eid : m_eog_ids)
        if (eid >= 0 && eid < m_vocab_size) m_is_eog[(size_t) eid] = 1;
    m_eog_token_ids.clear();
    m_tokens_by_first_byte.assign(256, {});
    for (int tid = 0; tid < m_vocab_size; ++tid) {
        const std::string& tok = m_tokens[(size_t) tid];
        if (tok.empty() || m_is_eog[(size_t) tid]) {
            m_eog_token_ids.push_back(tid);
            continue;
        }
        m_tokens_by_first_byte[(uint8_t) tok[0]].push_back(tid);
    }
    m_first_memo.clear();

    // Initialise stacks from the root rule (rule 0). Rule 0 is never expanded
    // via a RuleRef, so its own alternatives must be enumerated here: one
    // stack per alternative start (an Alt separator or the rule end), plus an
    // empty stack for a trailing empty alternative (accepting from the start).
    m_stacks.clear();
    {
        const GrammarRule& root = m_rules[0];
        size_t alt_start = 0;
        for (size_t s = 0; s <= root.size(); ++s) {
            const bool boundary = (s == root.size()) || root[s].type == ElemType::Alt;
            if (!boundary) continue;
            if (alt_start < s) {
                Stack st;
                st.push_back({0, alt_start});
                m_stacks.push_back(std::move(st));
            } else {
                m_stacks.push_back(Stack{}); // empty alternative = accepting
            }
            alt_start = s + 1;
        }
    }
}
// ═══════════════════════════════════════════════════════════════════════════
// RUNTIME — llama.cpp stack model (replaces lines 370-653)
// ═══════════════════════════════════════════════════════════════════════════

// ── match_char ────────────────────────────────────────────────────────────

bool Grammar::match_char(const GrammarElement* pos, uint32_t chr) const {
    // llama.cpp-style: Char exact / CharNot / CharRngUpper (with preceding
    // Char as lower bound) / CharAlt chains.
    bool found = false;
    bool is_positive = (pos->type == ElemType::Char || pos->type == ElemType::CharNot);

    do {
        if (pos->type == ElemType::CharRngUpper) {
            // This shouldn't be called with CharRngUpper on top; the caller
            // should pass the preceding Char element as the lower bound.
            found = false;
            pos++;
        } else if (pos->type == ElemType::Char) {
            found = (pos->value == chr);
            pos++;
        } else if (pos->type == ElemType::CharNot) {
            found = (pos->value != chr);
            pos++;
        } else if (pos->type == ElemType::CharAlt) {
            pos++;
        } else {
            break;
        }
    } while (pos->type == ElemType::CharAlt);

    return found == is_positive;
}

// ── match_partial_char ────────────────────────────────────────────────────

bool Grammar::match_partial_char(const GrammarElement* pos) const {
    // Called when m_n_remain > 0 (inside a multi-byte codepoint).
    // A continuation byte token (first byte 0x80-0xBF) is valid here.
    uint32_t cp = pos->value;
    return utf8_byte_count(cp) > 1;
}

// ── close_stack_set (extracted Phase A) ───────────────────────────────────

static void close_stack_set(const std::vector<GrammarRule>& rules,
                             std::vector<Stack> input,
                             std::vector<Stack>& out) {
    // Expand RuleRefs / skip Alts / pop Ends until every stack's top is a
    // consuming element or the stack is empty (rule complete = accepting).
    std::vector<Stack> todo = std::move(input);
    std::set<Stack> seen;
    for (auto& stk : todo) seen.insert(stk);

    size_t steps = 0;
    while (!todo.empty()) {
        if (++steps > 100000) {
            break;
        }
        Stack curr = std::move(todo.back());
        todo.pop_back();

        if (curr.empty()) { out.push_back(std::move(curr)); continue; }

        const StackElem top = curr.back();
        const GrammarRule& rule = rules[top.rule_id];

        // FIX 2: past-end — pop and CONTINUE closure (don't drop parent remainder).
        if (top.pos >= rule.size()) {
            curr.pop_back();
            if (curr.empty()) {
                out.push_back(std::move(curr));
            } else if (seen.insert(curr).second) {
                todo.push_back(std::move(curr));
            }
            continue;
        }

        const GrammarElement& elem = rule[top.pos];
        if (elem.type == ElemType::RuleRef) {
            const GrammarRule& ref_rule = rules[elem.value];
            Stack parent = curr;
            parent.pop_back();
            if (top.pos + 1 < rule.size()) {
                const GrammarElement& nxt = rule[top.pos + 1];
                if (nxt.type != ElemType::End && nxt.type != ElemType::Alt)
                    parent.push_back({top.rule_id, top.pos + 1});
            }
            size_t alt_start = 0;
            for (size_t s = 0; s <= ref_rule.size(); ++s) {
                bool boundary = (s == ref_rule.size()) || ref_rule[s].type == ElemType::Alt;
                if (!boundary) continue;
                Stack fork = parent;
                if (alt_start < s) fork.push_back({elem.value, alt_start});
                bool fresh = seen.insert(fork).second;
                if (fresh) todo.push_back(std::move(fork));
                alt_start = s + 1;
            }
            continue;
        }
        if (elem.type == ElemType::Alt) {
            curr.back().pos++;
            if (seen.insert(curr).second)
                todo.push_back(std::move(curr));
            continue;
        }
        if (elem.type == ElemType::End) {
            curr.pop_back();
            while (!curr.empty()) {
                const StackElem& t2 = curr.back();
                const GrammarRule& r2 = rules[t2.rule_id];
                if (t2.pos < r2.size() && r2[t2.pos].type == ElemType::End) curr.pop_back();
                else break;
            }
            if (seen.insert(curr).second)
                todo.push_back(std::move(curr));
            continue;
        }
        out.push_back(std::move(curr));
    }
}

// ── Character-class chain matching (llama.cpp semantics) ───────────────────

// A character class is encoded as a run of member units:
//   Char/CharNot(v) + CharAlt(0)        → single member v (CharAlt(0) terminator)
//   Char/CharNot(lo) + CharRngUpper(hi) → range member lo..hi
// A Char/CharNot NOT followed by CharAlt/CharRngUpper is a lone literal char
// (from a quoted string) — it ends the chain and is its own member.
// Member hit test: byte == member value, uniformly (the Char vs CharNot type
// only marks the member; the FIRST member's type marks the class negated).
// Positive class: matches iff ANY member hits. Negated: iff NO member hits.

// Next stack position after ONE byte is consumed at a class position pos:
// the first element past the whole member chain (or pos+1 for a lone char).
static size_t class_advance_end(const GrammarRule& rule, size_t pos) {
    size_t i = pos;
    bool first = true;
    while (i < rule.size()) {
        const GrammarElement& e = rule[i];
        if (e.type != ElemType::Char && e.type != ElemType::CharNot) break;
        if (i + 1 < rule.size()) {
            const GrammarElement& n = rule[i + 1];
            if (n.type == ElemType::CharRngUpper || n.type == ElemType::CharAlt) {
                i += 2; // member unit consumed (terminator or range pair)
                first = false;
                continue;
            }
        }
        if (first) ++i; // lone char AS the matched position: advance past it
        break;          // a later lone char is the next element, not a member
    }
    return i;
}

static bool class_matches(const GrammarRule& rule, size_t pos, uint8_t byte,
                           bool negated) {
    bool any_matches = false;
    size_t i = pos;
    bool first = true;
    while (i < rule.size()) {
        const GrammarElement& e = rule[i];
        if (e.type != ElemType::Char && e.type != ElemType::CharNot) break;
        if (i + 1 < rule.size()) {
            const GrammarElement& n = rule[i + 1];
            if (n.type == ElemType::CharRngUpper) {
                if (e.value <= byte && byte <= n.value) any_matches = true;
                i += 2;
                first = false;
            } else if (n.type == ElemType::CharAlt) {
                if (e.value == byte) any_matches = true;
                i += 2;
                first = false;
            } else {
                // Lone literal char: a member only when it IS the matched
                // position; a later lone char is the next element, not ours.
                if (first && e.value == byte) any_matches = true;
                ++i;
                break;
            }
        } else {
            if (first && e.value == byte) any_matches = true;
            ++i;
            break;
        }
        if (any_matches && !negated) break; // positive: one hit decides (OR)
    }
    return negated ? !any_matches : any_matches;
}

// ── advance_stack (llama.cpp closure model) ───────────────────────────────

void Grammar::advance_stack(const Stack& stack, uint8_t byte,
                             std::vector<Stack>& out) const {
    // Phase A — CLOSURE: expand RuleRefs (llama.cpp advance_stack).
    std::vector<Stack> closed;
    close_stack_set(m_rules, {stack}, closed);


    // Phase B — CONSUME: match byte against closed-stack tops.
    std::vector<Stack> advanced;
    for (const auto& stk : closed) {
        // Empty stacks are accepting — pass through unchanged.
        if (stk.empty()) { advanced.push_back(stk); continue; }

        const StackElem& top = stk.back();
        const GrammarRule& rule = m_rules[top.rule_id];
        if (top.pos >= rule.size()) { advanced.push_back(stk); continue; }

        const GrammarElement& elem = rule[top.pos];

        bool matched = false;
        size_t advance_pos = top.pos + 1;

        switch (elem.type) {
            case ElemType::Char:
                matched = class_matches(rule, top.pos, byte, false);
                advance_pos = class_advance_end(rule, top.pos);
                break;
            case ElemType::CharNot:
                matched = class_matches(rule, top.pos, byte, true);
                advance_pos = class_advance_end(rule, top.pos);
                break;
            case ElemType::CharRngUpper:
                // Needs lower bound from preceding Char in same rule.
                if (top.pos > 0) {
                    const GrammarElement& prev = rule[top.pos - 1];
                    matched = (prev.type == ElemType::Char || prev.type == ElemType::CharNot)
                           && byte >= prev.value && byte <= elem.value;
                }
                break;
            case ElemType::Token:
                matched = (elem.value == byte);
                break;
            case ElemType::TokenNot:
                matched = (elem.value != byte);
                break;
            default:
                // End/Alt/RuleRef/CharAlt — shouldn't appear on closed stack.
                matched = false;
                break;
        }

        if (!matched) {
            continue; // stack dies on mismatch.
        }

        Stack advanced_stack(stk.begin(), stk.end() - 1);
        // FIX 1: push next position ONLY when NOT End/Alt (else element is dropped).
        if (advance_pos < rule.size()) {
            const GrammarElement& next_elem = rule[advance_pos];
            if (next_elem.type != ElemType::End && next_elem.type != ElemType::Alt) {
                advanced_stack.push_back({top.rule_id, advance_pos});
            }
        }
        advanced.push_back(std::move(advanced_stack));
    }

    // FIX 3: close the advanced stacks (RuleRef top gets expanded, empty = accepting).
    std::vector<Stack> reclosed;
    close_stack_set(m_rules, std::move(advanced), reclosed);

    for (auto& stk : reclosed) {
        out.push_back(std::move(stk));
    }
}

// ── first-byte mask helpers ───────────────────────────────────────────────

// Class member values are UTF-8 bytes (the parser byte-splits multi-byte
// ranges), used directly.

// OR the bytes that can start a match at (rule_id, pos) into first[4].
// RuleRef heads recurse into the sub-rule's head (a fresh stack sits on
// root's RuleRef, so this is what makes the first mask correct); Alt
// elements are transparent separators (the closure forks before them).
// Memoised per compute_mask call; in_progress breaks rule-reference cycles
// (a cycle contributes nothing new to FIRST bytes).
static void elem_first(const std::vector<GrammarRule>& rules, int rule_id, size_t pos,
                       uint64_t first[4],
                       std::map<std::pair<int, size_t>, std::array<uint64_t, 4>>& memo,
                       std::set<std::pair<int, size_t>>& in_progress) {
    if (rule_id < 0 || rule_id >= (int) rules.size()) return;
    const GrammarRule& rule = rules[(size_t) rule_id];
    if (pos >= rule.size()) return;
    const std::pair<int, size_t> key{rule_id, pos};
    auto hit = memo.find(key);
    if (hit != memo.end()) {
        for (int i = 0; i < 4; i++) first[i] |= hit->second[i];
        return;
    }
    if (in_progress.count(key)) return;   // cycle: nothing new
    in_progress.insert(key);

    uint64_t t[4] = {0, 0, 0, 0};
    auto allow = [&](uint8_t b) { t[b >> 6] |= (1ULL << (b & 63)); };
    auto forbid = [&](uint8_t b) { t[b >> 6] &= ~(1ULL << (b & 63)); };

    const GrammarElement& head = rule[pos];
    if (head.type == ElemType::Char || head.type == ElemType::CharNot) {
        const bool negated = head.type == ElemType::CharNot;
        if (negated) { t[0] = t[1] = t[2] = t[3] = ~0ULL; }   // start from the complement
        // Walk the member units exactly like class_matches: a unit is
        // Char/CharNot(v) + CharAlt(terminator) (single member) or
        // Char/CharNot(lo) + CharRngUpper(hi) (range).  A Char/CharNot with
        // any other follower is a lone literal char — a member only at the
        // chain start (it is the matched position there); a later lone char
        // is the next byte position and ends the chain.  Unlike
        // class_matches there is NO early exit: the mask needs every member.
        size_t i = pos;
        bool first = true;
        while (i < rule.size()) {
            const GrammarElement& e = rule[i];
            if (e.type != ElemType::Char && e.type != ElemType::CharNot) break;
            if (i + 1 < rule.size()) {
                const GrammarElement& n = rule[i + 1];
                if (n.type == ElemType::CharRngUpper) {
                    for (uint32_t c = e.value; c <= n.value && c < 256; c++) {
                        if (negated) forbid((uint8_t) c); else allow((uint8_t) c);
                    }
                    i += 2;
                    first = false;
                } else if (n.type == ElemType::CharAlt) {
                    if (negated) forbid((uint8_t) e.value); else allow((uint8_t) e.value);
                    i += 2;
                    first = false;
                } else {
                    // Lone literal char: member only when it IS the matched
                    // position.
                    if (first) {
                        if (negated) forbid((uint8_t) e.value); else allow((uint8_t) e.value);
                    }
                    break;
                }
            } else {
                if (first) {
                    if (negated) forbid((uint8_t) e.value); else allow((uint8_t) e.value);
                }
                break;
            }
        }
    } else if (head.type == ElemType::RuleRef) {
        elem_first(rules, (int) head.value, 0, t, memo, in_progress);
    } else if (head.type == ElemType::Alt) {
        elem_first(rules, rule_id, pos + 1, t, memo, in_progress);
    }
    // End / Token / others: no first bytes here.

    memo[key] = {t[0], t[1], t[2], t[3]};
    for (int i = 0; i < 4; i++) first[i] |= t[i];
    in_progress.erase(key);
}

// ── compute_mask ──────────────────────────────────────────────────────────

void Grammar::compute_mask() const {
    m_mask.assign((m_vocab_size + 63) / 64, 0ULL);

    // Build the cache key from the stack TOPS (see CacheKey): the mask only
    // ever reads stack.back() and stack.empty(), so equal tops + partial +
    // remain means equal mask.  Stack bodies differ all the time in
    // production, so the tops key is what makes the cache hit across steps.
    CacheKey key;
    key.partial_value = m_partial_value;
    key.n_remain      = m_n_remain;
    bool eog_allowed = false;
    {
        std::vector<StackElem> tops;
        tops.reserve(m_stacks.size());
        for (const auto& stack : m_stacks) {
            if (stack.empty()) { eog_allowed = true; continue; }
            tops.push_back(stack.back());
        }
        std::sort(tops.begin(), tops.end());
        tops.erase(std::unique(tops.begin(), tops.end()), tops.end());
        key.tops = std::move(tops);
    }
    auto it = m_cache.find(key);
    if (it != m_cache.end()) { m_mask = it->second; m_mask_valid = true; return; }

    // Allowed first bytes = union over the live stack tops of the bytes that
    // can start a match there (recursive over RuleRef heads so raw unclosed
    // stacks like the fresh [0:0] work).  The memo is a member: first-byte
    // sets depend only on the parsed rules, which accept() never touches, so
    // entries stay valid across steps; set_vocab clears them.  Cycle-guarded
    // (truncated) entries are only ever written for RuleRef-head keys, which
    // the closure never leaves as stack tops, so no truncated entry is read.
    uint64_t allowed_first[4] = {0, 0, 0, 0};
    {
        std::set<std::pair<int, size_t>> fb_in_progress;
        for (const auto& top : key.tops) {
            if (top.rule_id == 0xFFFFFFFFu) continue;   // empty-stack sentinel
            elem_first(m_rules, (int) top.rule_id, top.pos, allowed_first,
                       m_first_memo, fb_in_progress);
        }
    }

    // EOG (empty-string tokens and engine EOG ids) is allowed iff any live
    // stack is empty (accepting); the empty stacks contribute no first bytes.
    if (eog_allowed)
        for (int tid : m_eog_token_ids)
            m_mask[(size_t) tid / 64] |= (1ULL << (tid % 64));

    if (m_n_remain > 0) {
        // Mid-UTF-8: only continuation tokens are legal, and only when some
        // stack top accepts a continuation byte.  The per-stack decision is
        // hoisted out of the token loop (identical to the old per-token
        // switch, now run once per top instead of once per stack x token).
        bool cont_allowed = false;
        for (const auto& top : key.tops) {
            if (top.rule_id == 0xFFFFFFFFu) continue;
            const GrammarRule& rule = m_rules[top.rule_id];
            if (top.pos >= rule.size()) continue;
            const GrammarElement& elem = rule[top.pos];
            bool stack_ok = false;
            switch (elem.type) {
                case ElemType::Char:
                case ElemType::CharNot:
                    stack_ok = utf8_byte_count(elem.value) > 1;
                    break;
                case ElemType::CharRngUpper:
                    if (top.pos > 0) {
                        const GrammarElement& prev = rule[top.pos - 1];
                        if (prev.type == ElemType::Char || prev.type == ElemType::CharNot) {
                            for (uint32_t c = prev.value; c <= elem.value && c <= 0x10FFFF; c++) {
                                if (utf8_byte_count(c) > 1) { stack_ok = true; break; }
                            }
                        }
                    }
                    break;
                default: break;
            }
            if (stack_ok) { cont_allowed = true; break; }
        }
        if (cont_allowed) {
            for (int b = 0x80; b < 0xC0; ++b)
                for (int tid : m_tokens_by_first_byte[(size_t) b])
                    m_mask[(size_t) tid / 64] |= (1ULL << (tid % 64));
        }
    } else {
        // Normal path: every non-EOG token is decided by its first byte via
        // the bucket tables built in set_vocab.  A multi-byte token whose
        // later bytes fail the strict accept() is rejected there and the
        // sampler retries.
        for (int b = 0; b < 256; ++b) {
            if (!(allowed_first[b >> 6] & (1ULL << (b & 63)))) continue;
            for (int tid : m_tokens_by_first_byte[(size_t) b])
                m_mask[(size_t) tid / 64] |= (1ULL << (tid % 64));
        }
    }

    m_cache[key] = m_mask;
    m_mask_valid = true;
}

// ── apply_logit_mask ──────────────────────────────────────────────────────

void Grammar::apply_logit_mask(float* logits, int n_vocab) {
    compute_mask();
    for (int id = 0; id < n_vocab; ++id) {
        const bool allowed = id < m_vocab_size
            && (m_mask[(size_t)id >> 6] >> (id & 63)) & 1ULL;
        if (!allowed) logits[id] = -1e30f;
    }
}

bool Grammar::is_allowed(int id) const {
    if (id < 0 || id >= m_vocab_size) return false;
    compute_mask();
    return (m_mask[(size_t)id >> 6] >> (id & 63)) & 1ULL;
}

// ── allowed_mask ──────────────────────────────────────────────────────────

const std::vector<unsigned long long>& Grammar::allowed_mask() {
    if (m_terminal) {
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

    const std::string& tok = m_tokens[token_id];
    if (tok.empty()) return;

    // Advance all stacks byte-by-byte.  Each byte walks from the state the
    // previous byte committed (m_stacks), so multi-byte tokens consume
    // byte-by-byte against the evolving grammar state.  advance_stack passes
    // completed (empty) stacks through unchanged, so accepting paths survive
    // inside a multi-byte token.
    std::vector<Stack> new_stacks;
    for (size_t byte_idx = 0; byte_idx < tok.size(); byte_idx++) {
        uint8_t byte = static_cast<uint8_t>(tok[byte_idx]);
        new_stacks.clear();
        for (const auto& stk : m_stacks) {
            advance_stack(stk, byte, new_stacks);
        }
        std::sort(new_stacks.begin(), new_stacks.end());
        new_stacks.erase(std::unique(new_stacks.begin(), new_stacks.end()), new_stacks.end());

        if (new_stacks.empty()) {
            m_mask_valid = false;
            return; // state unchanged.
        }
        m_stacks = std::move(new_stacks);
    }

    // Update m_partial_value / m_n_remain (token ending mid-UTF-8 sets carry).
    uint8_t first = static_cast<uint8_t>(tok[0]);
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

    m_mask_valid = false;
}

// ── accepting ─────────────────────────────────────────────────────────────

bool Grammar::accepting() const {
    if (m_terminal) return true;
    for (const auto& stack : m_stacks) {
        if (stack.empty()) return true;
    }
    return false;
}

// ── vocab_size ────────────────────────────────────────────────────────────

int Grammar::vocab_size() const { return m_vocab_size; }

} // namespace strata::core
