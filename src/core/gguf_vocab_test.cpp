// src/core/gguf_vocab_test.cpp - Standalone test: verifies full vocab read from
// a synthetic GGUF v3 file and decode_token_bytes.  Not CMake-registered;
// compile with:  cl /nologo /std:c++20 /W4 /EHsc /Iinclude src\core\gguf_vocab_test.cpp /Fe:test\gguf_vocab_test.exe
// Then run; exit 0 = passed.

#include "strata/artifact/gguf_reader.hpp"
#include "strata/core/grammar.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Write little-endian u32 / u64 to a buffer (works on x86 / x64; on big-endian
// would need explicit byte swaps — Windows x86 is always LE).
static void write_u32(unsigned char* buf, uint32_t v) { std::memcpy(buf, &v, 4); }
static void write_u64(unsigned char* buf, uint64_t v) { std::memcpy(buf, &v, 8); }

// Build a minimal GGUF v3 file with one KV: tokenizer.ggml.tokens (ARRAY of STRING,
// count=70: 60 plain strings "tok0".."tok59" + 10 byte-fallback tokens).
static std::string build_gguf() {
    // --- token strings ---
    std::vector<std::string> plain(60);
    for (int i = 0; i < 60; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "tok%d", i);
        plain[i] = buf;
    }
    // byte-fallback: <0x00> .. <0x09>  (always 6 chars)
    const char* byte_tok[10] = {
        "<0x00>", "<0x01>", "<0x02>", "<0x03>", "<0x04>",
        "<0x05>", "<0x06>", "<0x07>", "<0x08>", "<0x09>"
    };
    const std::vector<std::string> all = [&plain, &byte_tok]() {
        std::vector<std::string> v;
        v.reserve(70);
        for (const auto& s : plain) v.push_back(s);
        for (const char* s : byte_tok) v.push_back(s);
        return v;
    }();

    // --- compute total size ---
    // Header: magic(4) + version(4) + tensor_count(8) + kv_count(8) = 24
    size_t size = 24;
    // KV entry:
    //   key: "tokenizer.ggml.tokens" (21 chars)
    const size_t key_len = std::strlen("tokenizer.ggml.tokens");
    size += 8 + key_len;
    //   value type: ARRAY = 9 (4 bytes)
    size += 4;
    //   element type: STRING = 8 (4 bytes)
    size += 4;
    //   count: 70 (8 bytes)
    size += 8;
    //   70 items, each: length(8) + bytes
    for (int i = 0; i < 70; ++i) {
        size += 8 + all[i].size();
    }

    unsigned char* buf = new unsigned char[size];
    size_t p = 0;

    // Header
    write_u32(buf + p, 0x46554747u); p += 4;  // magic "GGUF"
    write_u32(buf + p, 3u); p += 4;           // version 3
    write_u64(buf + p, 0u); p += 8;           // tensor_count = 0
    write_u64(buf + p, 1u); p += 8;           // kv_count = 1

    // KV: key
    write_u64(buf + p, key_len); p += 8;
    std::memcpy(buf + p, "tokenizer.ggml.tokens", key_len); p += key_len;

    // KV: value — ARRAY of STRING
    write_u32(buf + p, 9u); p += 4;  // type = ARRAY
    write_u32(buf + p, 8u); p += 4;  // elem type = STRING
    write_u64(buf + p, 70u); p += 8; // count = 70

    for (int i = 0; i < 70; ++i) {
        const std::string& s = all[i];
        write_u64(buf + p, (uint64_t)s.size()); p += 8;
        std::memcpy(buf + p, s.c_str(), s.size()); p += s.size();
    }

    std::string result(reinterpret_cast<const char*>(buf), size);
    delete[] buf;
    return result;
}

int main() {
    // 1. Build and write the synthetic GGUF
    std::string gguf_data = build_gguf();

    // Write to a temp file
    const char* tmpdir = std::getenv("TEMP");
    if (!tmpdir) tmpdir = ".";
    std::string path = std::string(tmpdir) + "\\gguf_vocab_test.gguf";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot create temp file\n");
        return 1;
    }
    std::fwrite(gguf_data.data(), 1, gguf_data.size(), f);
    std::fclose(f);

    // 2. Parse with GgufFile
    strata::GgufFile g(path);

    // Verify header fields
    if (g.version() != 3) {
        std::fprintf(stderr, "bad version: %u\n", g.version());
        return 1;
    }
    if (g.tensors().size() != 0) {
        std::fprintf(stderr, "expected 0 tensors, got %zu\n", g.tensors().size());
        return 1;
    }

    // 3. Check the KV entry
    const auto& meta = g.metadata();
    auto it = meta.find("tokenizer.ggml.tokens");
    if (it == meta.end()) {
        std::fprintf(stderr, "missing KV key tokenizer.ggml.tokens\n");
        return 1;
    }
    const strata::MetaValue& tv = it->second;
    if (tv.type != strata::MetaType::ARRAY) {
        std::fprintf(stderr, "expected ARRAY type, got %u\n", (unsigned)tv.type);
        return 1;
    }
    if (tv.count != 70) {
        std::fprintf(stderr, "expected count=70, got %lu\n", (unsigned long)tv.count);
        return 1;
    }
    if (tv.items.size() != 70) {
        std::fprintf(stderr, "expected items.size()=70, got %zu\n", tv.items.size());
        return 1;
    }

    // 4. Verify all 70 items are present (proves full read, not 64-sample)
    for (int i = 0; i < 60; ++i) {
        static char buf[8];
        std::snprintf(buf, sizeof(buf), "tok%d", i);
        if (tv.items[i].s != buf) {
            std::fprintf(stderr, "item %d: expected '%s', got '%s'\n", i, buf, tv.items[i].s.c_str());
            return 1;
        }
    }
    // byte-fallback tokens
    const char* expected_byte[10] = {
        "<0x00>", "<0x01>", "<0x02>", "<0x03>", "<0x04>",
        "<0x05>", "<0x06>", "<0x07>", "<0x08>", "<0x09>"
    };
    for (int i = 0; i < 10; ++i) {
        if (tv.items[60 + i].s != expected_byte[i]) {
            std::fprintf(stderr, "item %d: expected '%s', got '%s'\n",
                         60 + i, expected_byte[i], tv.items[60 + i].s.c_str());
            return 1;
        }
    }

    // 5. Test decode_token_bytes
    using strata::core::decode_token_bytes;

    if (decode_token_bytes("<0x41>") != "A") {
        std::fprintf(stderr, "decode_token_bytes(\"<0x41>\") != 'A'\n");
        return 1;
    }
    if (decode_token_bytes("hello") != "hello") {
        std::fprintf(stderr, "decode_token_bytes(\"hello\") != \"hello\"\n");
        return 1;
    }
    if (decode_token_bytes("<0xZZ>") != "<0xZZ>") {
        std::fprintf(stderr, "decode_token_bytes(\"<0xZZ>\") != \"<0xZZ>\"\n");
        return 1;
    }
    if (decode_token_bytes("<0x1") != "<0x1") {
        std::fprintf(stderr, "decode_token_bytes(\"<0x1\") != \"<0x1\"\n");
        return 1;
    }

    std::puts("gguf_vocab_test: passed");
    std::remove(path.c_str());  // clean up temp file
    return 0;
}
