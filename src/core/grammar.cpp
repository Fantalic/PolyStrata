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
    while (i < src.size() && src[i] != '|' && src[i] != ')' && src[i] != '\n') {
        skip_ws(src, i);
        if (i >= src.size() || src[i] == '|' || src[i] == ')' || src[i] == '\n') break;
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
        elems.push_back({ElemType::End, 0});
        out.m_rules[rid] = std::move(elems);

        // Left-recursion check: the rule may not reach itself from a
        // leftmost position (start of the rule or right after an Alt
        // separator, before any consuming element of that alternative).
        // Forward references to later rules are legal.
        // Aux rules (created by quantifiers) are appended later and are
        // not checked here — their self-references are non-leftmost.
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
    while (i < rule.size()) {
        const GrammarElement& e = rule[i];
        if (e.type != ElemType::Char && e.type != ElemType::CharNot) break;
        if (i + 1 < rule.size()) {
            const GrammarElement& n = rule[i + 1];
            if (n.type == ElemType::CharRngUpper || n.type == ElemType::CharAlt) {
                i += 2; // member unit consumed (terminator or range pair)
                continue;
            }
        }
        ++i; // lone literal char (or unterminated trailing member)
        break;
    }
    return i;
}

static bool class_matches(const GrammarRule& rule, size_t pos, uint8_t byte,
                           bool negated) {
    bool any_matches = false;
    size_t i = pos;
    while (i < rule.size()) {
        const GrammarElement& e = rule[i];
        if (e.type != ElemType::Char && e.type != ElemType::CharNot) break;
        bool hit = false;
        if (i + 1 < rule.size()) {
            const GrammarElement& n = rule[i + 1];
            if (n.type == ElemType::CharRngUpper) {
                hit = (e.value <= byte && byte <= n.value);
                i += 2;
            } else if (n.type == ElemType::CharAlt) {
                hit = (e.value == byte);
                i += 2;
            } else {
                // Lone literal char member: accumulate BEFORE the break — the
                // `if (hit)` block below is skipped once we leave the loop.
                if (e.value == byte) any_matches = true;
                ++i;
                break;
            }
        } else {
            if (e.value == byte) any_matches = true;
            ++i;
            break;
        }
        if (hit) {
            any_matches = true;
            if (!negated) break; // positive: one hit decides (OR)
        }
        // Negated class: keep scanning — the match needs NO member to hit.
    }
    const bool matched = negated ? !any_matches : any_matches;
    return matched;
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

// ── compute_mask ──────────────────────────────────────────────────────────

void Grammar::compute_mask() const {
    m_mask.assign((m_vocab_size + 63) / 64, 0ULL);

    // Cache lookup.
    CacheKey key{m_stacks, m_partial_value, m_n_remain};
    auto it = m_cache.find(key);
    if (it != m_cache.end()) { m_mask = it->second; m_mask_valid = true; return; }

    for (const auto& stack : m_stacks) {
        // EOG allowed iff any live stack is empty (accepting).
        if (stack.empty()) {
            for (int tid = 0; tid < m_vocab_size; tid++) {
                if (m_tokens[tid].empty())
                    m_mask[tid / 64] |= (1ULL << (tid % 64));
            }
            continue;
        }

        const StackElem& top = stack.back();
        const GrammarRule& rule = m_rules[top.rule_id];
        if (top.pos >= rule.size()) continue;

        const GrammarElement& elem = rule[top.pos];

        for (int tid = 0; tid < m_vocab_size; tid++) {
            const std::string& tok = m_tokens[tid];
            if (tok.empty()) continue; // EOG handled above.

            // Continuation-byte tokens: allowed ONLY when m_n_remain > 0.
            // When m_n_remain == 0 a lone continuation byte is illegal.
            if ((static_cast<uint8_t>(tok[0]) & 0xC0) == 0x80) {
                if (m_n_remain == 0) continue;
            }
            // Continuation-byte tokens: allowed when m_n_remain > 0
            // and the grammar element expects a multi-byte char.
            if ((static_cast<uint8_t>(tok[0]) & 0xC0) == 0x80 && m_n_remain > 0) {
                switch (elem.type) {
                    case ElemType::Char:
                    case ElemType::CharNot:
                        if (utf8_byte_count(elem.value) > 1)
                            m_mask[tid / 64] |= (1ULL << (tid % 64));
                        break;
                    case ElemType::CharRngUpper:
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
                    default: break;
                }
                continue;
            }

            // Normal tokens: walk ALL bytes through a fresh copy of the
            // current stack set via advance_stack (dedup sort+unique after
            // each byte); allowed iff every byte consumed and ≥1 stack survives.
            std::vector<Stack> walk_stacks;
            for (const auto& st : m_stacks)
                if (!st.empty()) walk_stacks.push_back(st);
            bool ok = true;
            for (size_t bi = 0; bi < tok.size(); bi++) {
                std::vector<Stack> next_walk;
                for (const auto& ws : walk_stacks) {
                    advance_stack(ws, static_cast<uint8_t>(tok[bi]), next_walk);
                }
                // Dedup.
                std::sort(next_walk.begin(), next_walk.end());
                next_walk.erase(std::unique(next_walk.begin(), next_walk.end()), next_walk.end());
                if (next_walk.empty()) { ok = false; break; }
                walk_stacks = std::move(next_walk);
            }
            if (ok) m_mask[tid / 64] |= (1ULL << (tid % 64));
        }
    }

    m_cache[key] = m_mask;
    m_mask_valid = true;
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

    // Advance all stacks byte-by-byte, committing the final stack set.
    const std::string& tok = m_tokens[token_id];
    if (tok.empty()) return;

    std::vector<Stack> walk_from;
    for (const auto& st : m_stacks)
        if (!st.empty()) walk_from.push_back(st);
    std::vector<Stack> new_stacks;

    for (size_t byte_idx = 0; byte_idx < tok.size(); byte_idx++) {
        uint8_t byte = static_cast<uint8_t>(tok[byte_idx]);
        new_stacks.clear();
        for (const auto& stk : walk_from) {
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
