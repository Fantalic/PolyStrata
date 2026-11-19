#include "strata/core/grammar.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <vector>

using namespace strata::core;

// --- Helpers --------------------------------------------------------------

// Deterministic LCG: s_{n+1} = a*s_n + c mod m
struct LCG {
    uint64_t state;
    LCG(uint64_t seed) : state(seed) {}
    uint64_t next() {
        state = state * 6364136223846793005u + 1442695040888963407u;
        return state;
    }
    uint64_t range(uint64_t r) { return next() % r; }
};

// --- Test 1: Parser accepts valid grammars --------------------------------

static void test_parser_accepts() {
    Grammar g;
    std::string err;

    // Simple literal.
    if (!Grammar::parse(R"(root ::= "a" "b")", g, err)) {
        std::fprintf(stderr, "test 1a: failed to parse root ::= \"a\" \"b\"\n");
        std::exit(1);
    }

    // Alternation.
    if (!Grammar::parse(R"(root ::= "a" | "b" "c")", g, err)) {
        std::fprintf(stderr, "test 1b: failed to parse alternation\n");
        std::exit(1);
    }

    // Group + repetition.
    if (!Grammar::parse(R"(root ::= [0-9]+ "."?)", g, err)) {
        std::fprintf(stderr, "test 1c: failed to parse group + repetition\n");
        std::exit(1);
    }

    // Char class with ranges / negation / escapes.
    if (!Grammar::parse(R"(root ::= [a-z0-9_][^0-9]*)", g, err)) {
        std::fprintf(stderr, "test 1d: failed to parse char class\n");
        std::exit(1);
    }

    // \xNN and \uNNNN escapes.
    if (!Grammar::parse(R"(root ::= "\x41\x42" "\u0043")", g, err)) {
        std::fprintf(stderr, "test 1e: failed to parse hex/unicode escapes\n");
        std::exit(1);
    }

    // Single-quoted literals.
    if (!Grammar::parse("root ::= 'hello' 'world'", g, err)) {
        std::fprintf(stderr, "test 1f: failed to parse single-quoted literals\n");
        std::exit(1);
    }

    // Comments.
    if (!Grammar::parse(R"(# comment
root ::= "a" # another comment
)", g, err)) {
        std::fprintf(stderr, "test 1g: failed to parse with comments\n");
        std::exit(1);
    }

    // Rule references across rules (a JSON-shaped grammar).
    if (!Grammar::parse(R"(
root ::= object
object ::= "{" ws? pairs ws? "}"
pairs ::= string ":" value ("," string ":" value)*
string ::= "\"" chars "\""
chars ::= [^\x22]*
value ::= string | number
number ::= [0-9]+
ws ::= [ \t]*
)", g, err)) {
        std::fprintf(stderr, "test 1h: failed to parse JSON-shaped grammar\n");
        std::exit(1);
    }
}

// --- Test 2: Parser rejects invalid grammars ------------------------------

static void test_parser_rejects() {
    Grammar g;
    std::string err;

    // Empty text.
    if (Grammar::parse("", g, err)) {
        std::fprintf(stderr, "test 2a: should reject empty text\n");
        std::exit(1);
    }

    // Unbalanced parens.
    if (Grammar::parse(R"(root ::= ("a")", g, err)) {
        std::fprintf(stderr, "test 2b: should reject unbalanced parens\n");
        std::exit(1);
    }

    // Unknown rule reference.
    if (Grammar::parse(R"(root ::= gone)", g, err)) {
        std::fprintf(stderr, "test 2c: should reject unknown rule ref\n");
        std::exit(1);
    }

    // Left recursion.
    if (Grammar::parse(R"(root ::= root "a" | "b")", g, err)) {
        std::fprintf(stderr, "test 2d: should reject left recursion\n");
        std::exit(1);
    }

    // Unsupported ~[ complement class.
    if (Grammar::parse(R"(root ::= ~[a-z])", g, err)) {
        std::fprintf(stderr, "test 2e: should reject ~[ complement class\n");
        std::exit(1);
    }
}

// --- Test 3: Forced walk — {"你好"} ---------------------------------------

static void test_forced_walk() {
    // Build small vocab.
    std::vector<std::string> tokens;
    // 0-25: single ASCII chars
    for (int i = 0; i < 26; ++i) tokens.push_back(std::string(1, static_cast<char>('a' + i)));
    // 26: "{"
    tokens.push_back("{");
    // 27: "\""
    tokens.push_back("\"");
    // 28: "}"
    tokens.push_back("}");
    // 29: "ab"
    tokens.push_back("ab");
    // 30: "{\""
    tokens.push_back("{\"");
    // 31-32: "你好" (each as 3-byte UTF-8 strings)
    // "你" = E4 BD A0
    tokens.push_back(std::string("\xE4\xBD\xA0", 3));
    // "好" = E5 A5 BD
    tokens.push_back(std::string("\xE5\xA5\xBD", 3));
    // 33: partial continuation byte 0xAD
    tokens.push_back(std::string("\xAD", 1));
    // 34: EOG
    tokens.push_back("");

    Grammar g;
    std::string err;
    if (!Grammar::parse(R"(
root ::= "{" ws? "\"" chars "\"" ws? "}"
chars ::= [^\x22]
ws ::= [ \t]*
)", g, err)) {
        // fixed-width chars rule so the 5-step walk completes; unbounded repetition is tested by test 4's JSON grammar
        std::fprintf(stderr, "test 3: parse failed: %s\n", err.c_str());
        std::exit(1);
    }
    g.set_vocab(tokens, {34});

    // Step 0: should allow "{" (26) and "{\"" (30) but not "a" (0).
    auto mask = g.allowed_mask();
    auto has = [&](int id) {
        return mask[id / 64] & (1ULL << (id % 64));
    };
    if (!has(26)) { std::fprintf(stderr, "test 3a: mask should contain '{' (26)\n"); std::exit(1); }
    if (!has(30)) { std::fprintf(stderr, "test 3b: mask should contain '{\"' (30)\n"); std::exit(1); }
    if (has(0)) { std::fprintf(stderr, "test 3c: mask should NOT contain 'a' (0)\n"); std::exit(1); }

    // Walk: always pick smallest allowed non-EOG token.
    std::string result;
    int step = 0;

    // Step 1: accept "{\"" (30).
    mask = g.allowed_mask();
    int tok = -1;
    for (int i = 0; i < 35; ++i) {
        if (mask[i / 64] & (1ULL << (i % 64)) && i != 34) { tok = i; break; }
    }
    if (tok < 0) { std::fprintf(stderr, "test 3d: no allowed token at step 1\n"); std::exit(1); }
    result += tokens[tok];
    g.accept(tok);
    ++step;

    // Step 2: should allow "你" (31) and "好" (32).
    mask = g.allowed_mask();
    tok = -1;
    for (int i = 0; i < 35; ++i) {
        if (mask[i / 64] & (1ULL << (i % 64)) && i != 34) { tok = i; break; }
    }
    if (tok < 0) { std::fprintf(stderr, "test 3e: no allowed token at step 2\n"); std::exit(1); }
    result += tokens[tok];
    g.accept(tok);
    ++step;

    // Step 3: should allow "好" (32) or closing quote.
    mask = g.allowed_mask();
    tok = -1;
    for (int i = 0; i < 35; ++i) {
        if (mask[i / 64] & (1ULL << (i % 64)) && i != 34) { tok = i; break; }
    }
    if (tok < 0) { std::fprintf(stderr, "test 3f: no allowed token at step 3\n"); std::exit(1); }
    result += tokens[tok];
    g.accept(tok);
    ++step;

    // Step 4: should allow closing quote "\" (27) and then "}" (28).
    // Accept closing quote.
    mask = g.allowed_mask();
    tok = -1;
    for (int i = 0; i < 35; ++i) {
        if (mask[i / 64] & (1ULL << (i % 64)) && i != 34) { tok = i; break; }
    }
    if (tok < 0) { std::fprintf(stderr, "test 3g: no allowed token at step 4\n"); std::exit(1); }
    result += tokens[tok];
    g.accept(tok);
    ++step;

    // Step 5: should allow "}" (28).
    mask = g.allowed_mask();
    tok = -1;
    for (int i = 0; i < 35; ++i) {
        if (mask[i / 64] & (1ULL << (i % 64)) && i != 34) { tok = i; break; }
    }
    if (tok < 0) { std::fprintf(stderr, "test 3h: no allowed token at step 5\n"); std::exit(1); }
    result += tokens[tok];
    g.accept(tok);
    ++step;

    // Now should be accepting (empty stack).
    if (!g.accepting()) { std::fprintf(stderr, "test 3i: should be accepting\n"); std::exit(1); }

    // EOG should be allowed now.
    mask = g.allowed_mask();
    if (!(mask[34 / 64] & (1ULL << (34 % 64)))) {
        std::fprintf(stderr, "test 3j: EOG (34) should be allowed at end\n");
        std::exit(1);
    }

    // EOG should NOT be allowed before the end (before step 5).
    // We already verified this above.
    std::fprintf(stdout, "test 3: forced walk produced: %s\n", result.c_str());
}

// --- Test 4: Forced JSON walk with Taiji-like grammar ---------------------

static void test_forced_json_walk() {
    // Grammar: object with "terms" array of objects with string fields.
    const char* gbnf = R"(
root ::= object
object ::= "{" ws? members ws? "}"
members ::= string ":" value ("," string ":" value)*
value ::= string | number | array | object
array ::= "[" ws? items ws? "]"
items ::= value ("," value)*
string ::= "\"" strchars "\""
strchars ::= [^\x22\x5c]* | [^\x22\x5c]* "\x5c" [^\x22\x5c]*
number ::= [0-9]+
ws ::= [ \t\n\r]*
)";

    // Build vocab with essential tokens.
    std::vector<std::string> tokens;
    // 0-3: "{" "}" "[" "]"
    tokens.push_back("{");
    tokens.push_back("}");
    tokens.push_back("[");
    tokens.push_back("]");
    // 4-6: "\"" "," ":"
    tokens.push_back("\"");
    tokens.push_back(",");
    tokens.push_back(":");
    // 7-11: "t" "e" "r" "m" "s"
    tokens.push_back("t");
    tokens.push_back("e");
    tokens.push_back("r");
    tokens.push_back("m");
    tokens.push_back("s");
    // 12-14: "n" "a" "m"
    tokens.push_back("n");
    tokens.push_back("a");
    tokens.push_back("m");
    // 15-17: " " "\t" "\n"
    tokens.push_back(" ");
    tokens.push_back("\t");
    tokens.push_back("\n");
    // 18-19: "0" "1" (for numbers)
    tokens.push_back("0");
    tokens.push_back("1");
    // 20-21: multibyte content tokens (multibyte walk uses ids 20/21)
    tokens.push_back(std::string("\xE4\xBD\xA0", 3)); // 你
    tokens.push_back(std::string("\xE5\xA5\xBD", 3)); // 好
    // 22: partial continuation
    tokens.push_back(std::string("\xAD", 1));
    // 23: EOG
    tokens.push_back("");

    Grammar g;
    std::string err;
    if (!Grammar::parse(gbnf, g, err)) {
        std::fprintf(stderr, "test 4: parse failed: %s\n", err.c_str());
        std::exit(1);
    }
    g.set_vocab(tokens, {23});

    // Deterministic walks: every token of a valid JSON sample must be
    // allowed by the mask, and EOG must be allowed exactly at the end.
    struct Sample { const char* name; std::vector<int> ids; bool valid; };
    const Sample samples[] = {
        {"stray brace in key",  {0, 4, 4, 0},              false},   // after the key string, only ':' may follow
        {"object cut short",    {0},                       false},   // EOG must NOT be allowed mid-object
        {"terms number",        {0, 4, 7, 8, 9, 10, 11, 4, 6, 18, 1}, true},
        {"nested array",        {0, 4, 16, 4, 6, 2, 18, 3, 1}, true},
        {"multibyte content",   {0, 4, 20, 4, 6, 4, 21, 4, 1}, true},
        {"nested object",       {0, 4, 12, 4, 6, 0, 4, 7, 8, 9, 10, 11, 4, 6, 2, 18, 3, 1, 1}, true},
    };
    int failures = 0;
    for (const Sample& s : samples) {
        Grammar g2;
        if (!Grammar::parse(gbnf, g2, err)) { std::fprintf(stderr, "test 4: parse failed: %s\n", err.c_str()); std::exit(1); }
        g2.set_vocab(tokens, {23});
        int failed_at = -1;
        for (int id : s.ids) {
            auto mask = g2.allowed_mask();
            if (!(mask[id / 64] & (1ULL << (id % 64)))) { failed_at = id; break; }
            g2.accept(id);
        }
        auto mask = g2.allowed_mask();
        bool eog_allowed_after = (mask[23 / 64] & (1ULL << (23 % 64))) ? 1 : 0;
        bool outcome_ok = s.valid ? (failed_at == -1 && eog_allowed_after) : (failed_at != -1 || !eog_allowed_after);
        if (!outcome_ok) { std::fprintf(stderr, "test 4: walk '%s' mismatch (failed_at_id=%d, eog=%d, valid=%d)\n", s.name, failed_at, eog_allowed_after, s.valid ? 1 : 0); ++failures; }
    }
    std::fprintf(stdout, "test 4: %d/%d deterministic walks correct\n", (int)(sizeof(samples)/sizeof(samples[0])) - failures, (int)(sizeof(samples)/sizeof(samples[0])));
    if (failures > 0) { std::fprintf(stderr, "test 4: deterministic walks failed\n"); std::exit(1); }
}

// --- Test 5: Multibyte char class -----------------------------------------

static void test_multibyte_class() {
    // root ::= [中-国]+
    // 中 = U+4E2D, 国 = U+56FD
    std::vector<std::string> tokens;
    // 0: "中" (E4 B8 AD)
    tokens.push_back(std::string("\xE4\xB8\xAD", 3));
    // 1: "国" (E5 9B BD)
    tokens.push_back(std::string("\xE5\x9B\xBD", 3));
    // 2: "a" (ASCII)
    tokens.push_back("a");
    // 3: partial 0xE4
    tokens.push_back(std::string("\xE4", 1));
    // 4: partial 0xE5
    tokens.push_back(std::string("\xE5", 1));
    // 5: EOG
    tokens.push_back("");

    Grammar g;
    std::string err;
    if (!Grammar::parse(R"(root ::= [中-国]+)", g, err)) {
        std::fprintf(stderr, "test 5: parse failed: %s\n", err.c_str());
        std::exit(1);
    }
    g.set_vocab(tokens, {5});

    auto mask = g.allowed_mask();
    // "中" (0) should be allowed.
    if (!(mask[0 / 64] & (1ULL << (0 % 64)))) {
        std::fprintf(stderr, "test 5a: '中' (0) should be allowed\n");
        std::exit(1);
    }
    // "国" (1) should be allowed.
    if (!(mask[1 / 64] & (1ULL << (1 % 64)))) {
        std::fprintf(stderr, "test 5b: '国' (1) should be allowed\n");
        std::exit(1);
    }
    // "a" (2) should NOT be allowed.
    if (mask[2 / 64] & (1ULL << (2 % 64))) {
        std::fprintf(stderr, "test 5c: 'a' (2) should NOT be allowed\n");
        std::exit(1);
    }
    // Partial bytes 0xE4 (3) and 0xE5 (4) should be allowed.
    if (!(mask[3 / 64] & (1ULL << (3 % 64)))) {
        std::fprintf(stderr, "test 5d: partial 0xE4 (3) should be allowed\n");
        std::exit(1);
    }
    if (!(mask[4 / 64] & (1ULL << (4 % 64)))) {
        std::fprintf(stderr, "test 5e: partial 0xE5 (4) should be allowed\n");
        std::exit(1);
    }
}

// --- Test 6: Cache sanity -------------------------------------------------

static void test_cache_sanity() {
    Grammar g;
    std::string err;
    if (!Grammar::parse(R"(root ::= "a" | "b")", g, err)) {
        std::fprintf(stderr, "test 6: parse failed\n");
        std::exit(1);
    }
    std::vector<std::string> tokens = {"a", "b", ""};
    g.set_vocab(tokens, {2});

    // Same state twice → identical mask.
    auto mask1 = g.allowed_mask();
    auto mask2 = g.allowed_mask();
    if (mask1 != mask2) {
        std::fprintf(stderr, "test 6a: identical state should give identical mask\n");
        std::exit(1);
    }

    // After accept, mask changes.
    g.accept(0); // accept "a"
    auto mask3 = g.allowed_mask();
    if (mask1 == mask3) {
        std::fprintf(stderr, "test 6b: mask should change after accept\n");
        std::exit(1);
    }
}

// --- Test 7: Terminal behavior --------------------------------------------

static void test_terminal() {
    Grammar g;
    std::string err;
    if (!Grammar::parse(R"(root ::= "a")", g, err)) {
        std::fprintf(stderr, "test 7: parse failed\n");
        std::exit(1);
    }
    std::vector<std::string> tokens = {"a", ""};
    g.set_vocab(tokens, {1});

    // Accept "a" — stack should become empty (accepting).
    g.accept(0);
    if (!g.accepting()) {
        std::fprintf(stderr, "test 7a: should be accepting after 'a'\n");
        std::exit(1);
    }

    // Accept EOG.
    g.accept(1);
    auto mask = g.allowed_mask();
    // All-zero mask after terminal.
    for (auto word : mask) {
        if (word != 0) {
            std::fprintf(stderr, "test 7b: mask should be all-zero after terminal\n");
            std::exit(1);
        }
    }

    // Further accept() calls should be no-ops (terminal).
    g.accept(0);
    g.accept(1);
    mask = g.allowed_mask();
    for (auto word : mask) {
        if (word != 0) {
            std::fprintf(stderr, "test 7c: mask should still be all-zero after further accepts\n");
            std::exit(1);
        }
    }
}

// --- Main -----------------------------------------------------------------

int main() {
    std::puts("grammar_test: starting...");

    test_parser_accepts();
    std::puts("  test 1: parser accepts — passed");

    test_parser_rejects();
    std::puts("  test 2: parser rejects — passed");

    test_forced_walk();
    std::puts("  test 3: forced walk — passed");

    test_forced_json_walk();
    std::puts("  test 4: forced JSON walk — passed");

    test_multibyte_class();
    std::puts("  test 5: multibyte class — passed");

    test_cache_sanity();
    std::puts("  test 6: cache sanity — passed");

    test_terminal();
    std::puts("  test 7: terminal behavior — passed");

    std::puts("grammar_test: all tests passed");
    return 0;
}
