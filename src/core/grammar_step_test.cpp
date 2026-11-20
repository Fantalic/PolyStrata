// src/core/grammar_step_test.cpp — standalone tests for GrammarStep.
//
// Compile standalone (needs grammar.cpp linked):
//   cl /nologo /std:c++20 /W4 /EHsc /Iinclude
//       /Fo:%TEMP%\opencode\ src\core\grammar_step_test.cpp src\core\grammar.cpp
//       /Fe:%TEMP%\opencode\grammar_step_test.exe
//
// Grammar used (same as grammar_test.cpp test 3):
//   root ::= "{" ws? "\"" chars "\"" ws? "}"
//   chars ::= [^\x22]
//   ws ::= [ \t]*
//
// Vocab: 0="{" 1="}" 2="\"" 3="a" 4=" " 5="\t" 6="<eog>"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstdint>
#include "strata/core/grammar_step.hpp"

static std::string hex_encode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        out += "0123456789abcdef"[c >> 4];
        out += "0123456789abcdef"[c & 0xF];
    }
    return out;
}

static void test_set() {
    strata::core::GrammarStep gs;
    std::string err;
    std::vector<std::string> vocab = { "{", "}", "\"", "a", " ", "\t", "<eog>" };
    std::vector<int32_t> eog = { 6 };

    // Valid hex → active() true
    const char* gbnf = R"(root ::= "{" ws? "\"" chars "\"" ws? "}"
chars ::= [^\x22]
ws ::= [ \t]*)";
    std::string hex = hex_encode(gbnf);
    if (!gs.set(hex, vocab, eog, err)) {
        std::fprintf(stderr, "test_set 1: valid hex failed: %s\n", err.c_str());
        std::exit(1);
    }
    if (!gs.active()) {
        std::fprintf(stderr, "test_set 2: should be active\n");
        std::exit(1);
    }

    // Empty hex → false + err
    strata::core::GrammarStep gs2;
    if (gs2.set("", vocab, eog, err)) {
        std::fprintf(stderr, "test_set 3: empty hex should fail\n");
        std::exit(1);
    }
    if (err != "empty grammar") {
        std::fprintf(stderr, "test_set 4: expected 'empty grammar', got '%s'\n", err.c_str());
        std::exit(1);
    }

    // Odd-length hex → false + "bad hex encoding"
    strata::core::GrammarStep gs3;
    if (gs3.set("abc", vocab, eog, err)) {
        std::fprintf(stderr, "test_set 5: odd hex should fail\n");
        std::exit(1);
    }
    if (err != "bad hex encoding") {
        std::fprintf(stderr, "test_set 6: expected 'bad hex encoding', got '%s'\n", err.c_str());
        std::exit(1);
    }

    // Garbage GBNF → false + err
    strata::core::GrammarStep gs4;
    std::string bad_hex = hex_encode("root ::=");
    if (gs4.set(bad_hex, vocab, eog, err)) {
        std::fprintf(stderr, "test_set 7: bad grammar should fail\n");
        std::exit(1);
    }
    if (err.empty()) {
        std::fprintf(stderr, "test_set 8: should have error message\n");
        std::exit(1);
    }

    std::fprintf(stdout, "test_set: pass\n");
}

static void test_mask() {
    strata::core::GrammarStep gs;
    std::string err;
    std::vector<std::string> vocab = { "{", "}", "\"", "a", " ", "\t", "<eog>" };
    std::vector<int32_t> eog = { 6 };

    const char* gbnf = R"(root ::= "{" ws? "\"" chars "\"" ws? "}"
chars ::= [^\x22]
ws ::= [ \t]*)";
    std::string hex = hex_encode(gbnf);
    if (!gs.set(hex, vocab, eog, err)) {
        std::fprintf(stderr, "test_mask 1: set failed: %s\n", err.c_str());
        std::exit(1);
    }

    // logits[i] = (float)i; only "{" (id 0) should survive at start.
    std::vector<float> logits(7);
    for (int i = 0; i < 7; i++) logits[i] = (float)i;

    gs.mask(logits.data(), 7);

    // "{" (id 0) must keep its value.
    if (logits[0] != 0.f) {
        std::fprintf(stderr, "test_mask 2: '{' (0) should keep 0.f, got %f\n", logits[0]);
        std::exit(1);
    }
    // All other tokens must be clamped to -1e30f.
    for (int i = 1; i < 7; i++) {
        if (logits[i] != -1e30f) {
            std::fprintf(stderr, "test_mask 3: id %d should be -1e30f, got %f\n", i, logits[i]);
            std::exit(1);
        }
    }

    std::fprintf(stdout, "test_mask: pass\n");
}

static void test_greedy_walk() {
    strata::core::GrammarStep gs;
    std::string err;
    std::vector<std::string> vocab = { "{", "}", "\"", "a", " ", "\t", "<eog>" };
    std::vector<int32_t> eog = { 6 };

    const char* gbnf = R"(root ::= "{" ws? "\"" chars "\"" ws? "}"
chars ::= [^\x22]
ws ::= [ \t]*)";
    std::string hex = hex_encode(gbnf);
    if (!gs.set(hex, vocab, eog, err)) {
        std::fprintf(stderr, "test_greedy_walk 1: set failed: %s\n", err.c_str());
        std::exit(1);
    }

    std::string result;
    for (int step = 0; step < 20; step++) {
        std::vector<float> logits(7);
        for (int i = 0; i < 7; i++) logits[i] = (float)i;

        gs.mask(logits.data(), 7);

        // Argmin over LEGAL ids only: the clamped ids are -1e30f and must be
        // excluded (they are more negative than any real logit).  The lowest
        // legal id makes the walk terminate on this grammar; argmax can loop
        // legally inside ws*.
        int32_t best = -1;
        for (int i = 0; i < 7; i++) {
            if (logits[i] > -1e29f && (best < 0 || logits[i] < logits[best]))
                best = i;
        }
        if (best < 0) {
            std::fprintf(stderr, "test_greedy_walk %d: no allowed token at step %d\n", step, step);
            std::exit(1);
        }

        // EOG ends the walk.
        if (best == 6) break;

        // Accept the token.
        if (!gs.accept(best)) {
            std::fprintf(stderr, "test_greedy_walk %d: grammar rejected token %d\n", step, best);
            std::exit(1);
        }

        result += vocab[best];
    }

    // Result should be the deterministic argmin walk of this grammar:
    //   `{` `"` `{` `"` `}`  (the `{` is legal string content — [^\x22])
    if (result != "{\"{\"}") {
        std::fprintf(stderr, "test_greedy_walk: expected '{\"{\"}', got '%s'\n", result.c_str());
        std::exit(1);
    }
    std::fprintf(stdout, "test_greedy_walk: pass (result='%s')\n", result.c_str());
}

static void test_eog_masking() {
    strata::core::GrammarStep gs;
    std::string err;
    std::vector<std::string> vocab = { "{", "}", "\"", "a", " ", "\t", "<eog>" };
    std::vector<int32_t> eog = { 6 };

    const char* gbnf = R"(root ::= "{" ws? "\"" chars "\"" ws? "}"
chars ::= [^\x22]
ws ::= [ \t]*)";
    std::string hex = hex_encode(gbnf);
    if (!gs.set(hex, vocab, eog, err)) {
        std::fprintf(stderr, "test_eog_masking 1: set failed: %s\n", err.c_str());
        std::exit(1);
    }

    // In the initial state (not accepting), EOG (id 6) must be masked.
    std::vector<float> logits(7);
    for (int i = 0; i < 7; i++) logits[i] = (float)i;
    gs.mask(logits.data(), 7);
    if (logits[6] != -1e30f) {
        std::fprintf(stderr, "test_eog_masking 2: EOG should be masked initially\n");
        std::exit(1);
    }

    // Walk to accepting state: accept "{", "\"", "a", "\"", "}"
    std::vector<int32_t> walk = { 0, 2, 3, 2, 1 };
    for (int32_t tok : walk) {
        std::vector<float> l(7);
        for (int i = 0; i < 7; i++) l[i] = (float)i;
        gs.mask(l.data(), 7);
        if (!gs.accept(tok)) {
            std::fprintf(stderr, "test_eog_masking 3: failed at token %d\n", tok);
            std::exit(1);
        }
    }

    // Now in accepting state: EOG (id 6) should be allowed (not masked).
    std::vector<float> l2(7);
    for (int i = 0; i < 7; i++) l2[i] = (float)i;
    gs.mask(l2.data(), 7);
    if (l2[6] == -1e30f) {
        std::fprintf(stderr, "test_eog_masking 4: EOG should be allowed in accepting state\n");
        std::exit(1);
    }

    std::fprintf(stdout, "test_eog_masking: pass\n");
}

static void test_accept_masked_id() {
    strata::core::GrammarStep gs;
    std::string err;
    std::vector<std::string> vocab = { "{", "}", "\"", "a", " ", "\t", "<eog>" };
    std::vector<int32_t> eog = { 6 };

    const char* gbnf = R"(root ::= "{" ws? "\"" chars "\"" ws? "}"
chars ::= [^\x22]
ws ::= [ \t]*)";
    std::string hex = hex_encode(gbnf);
    if (!gs.set(hex, vocab, eog, err)) {
        std::fprintf(stderr, "test_accept_masked_id 1: set failed: %s\n", err.c_str());
        std::exit(1);
    }

    // At the start, only "{" (id 0) is allowed. Try to accept "}" (id 1).
    if (gs.accept(1)) {
        std::fprintf(stderr, "test_accept_masked_id 2: should reject masked id\n");
        std::exit(1);
    }

    std::fprintf(stdout, "test_accept_masked_id: pass\n");
}

// The exact json_object_grammar() output (serve/json_schema_to_grammar.py):
// one JSON object with at least one member — {} must not be derivable.
// Vocab: 0="{" 1="}" 2="\"" 3="a" 4=":" 5="1" 6="," 7="<eog>"
static void test_json_object_member_required() {
    strata::core::GrammarStep gs;
    std::string err;
    std::vector<std::string> vocab = { "{", "}", "\"", "a", ":", "1", ",", "<eog>" };
    std::vector<int32_t> eog = { 7 };

    const char* gbnf = R"GBNF(root ::= "{" ws string ":" ws value ("," ws string ":" ws value)* ws "}"
array ::= "[" ws ( value ("," ws value)* )? ws "]"
boolean ::= "true" | "false"
char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\bfnrt] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F])
dec_part ::= [0-9] [0-9]*
int_part ::= "0" | [1-9] [0-9]*
integer ::= "-"? int_part
null ::= "null"
number ::= "-"? int_part ("." dec_part)? ([eE] [-+]? dec_part)?
object ::= "{" ws ( string ":" ws value ("," ws string ":" ws value)* )? ws "}"
string ::= "\"" char* "\""
value ::= object | array | string | number | boolean | null
ws ::= [ \t\n\r]*)GBNF";
    std::string hex = hex_encode(gbnf);
    if (!gs.set(hex, vocab, eog, err)) {
        std::fprintf(stderr, "test_json_object 1: set failed: %s\n", err.c_str());
        std::exit(1);
    }

    // At the start only "{" (id 0) is legal; "}" and "," must be masked.
    std::vector<float> logits(8);
    for (int i = 0; i < 8; i++) logits[i] = (float)i;
    gs.mask(logits.data(), 8);
    if (logits[0] != 0.f || logits[1] != -1e30f || logits[6] != -1e30f) {
        std::fprintf(stderr, "test_json_object 2: start mask wrong ({=%f }=%f ,=%f)\n",
                     logits[0], logits[1], logits[6]);
        std::exit(1);
    }

    // Accept "{" — "}" must STILL be masked (a member is required) while the
    // string quote must be allowed. This is the regression proof for the
    // measured temp-0 "{}" degeneration.
    if (!gs.accept(0)) {
        std::fprintf(stderr, "test_json_object 3: '{' rejected\n");
        std::exit(1);
    }
    for (int i = 0; i < 8; i++) logits[i] = (float)i;
    gs.mask(logits.data(), 8);
    if (logits[1] != -1e30f) {
        std::fprintf(stderr, "test_json_object 4: '}' allowed after '{' — {} is derivable\n");
        std::exit(1);
    }
    if (logits[2] == -1e30f) {
        std::fprintf(stderr, "test_json_object 5: '\"' masked after '{' — nothing can follow\n");
        std::exit(1);
    }

    // A full member walk: {"a":1}
    const int32_t walk[] = { 2, 3, 2, 4, 5, 1 };
    for (int step = 0; step < 6; step++) {
        if (!gs.accept(walk[step])) {
            std::fprintf(stderr, "test_json_object 6: step %d rejected token %d\n", step, walk[step]);
            std::exit(1);
        }
    }

    // Root is complete: EOG must now be allowed (the model may stop).
    for (int i = 0; i < 8; i++) logits[i] = (float)i;
    gs.mask(logits.data(), 8);
    if (logits[7] == -1e30f) {
        std::fprintf(stderr, "test_json_object 7: EOG masked after a complete object\n");
        std::exit(1);
    }

    std::fprintf(stdout, "test_json_object_member_required: pass\n");
}

int main() {
    test_set();
    test_mask();
    test_greedy_walk();
    test_eog_masking();
    test_accept_masked_id();
    test_json_object_member_required();
    std::fprintf(stdout, "All tests pass\n");
    return 0;
}
