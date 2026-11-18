// src/poly/spill_test.cpp - Verify the idle gate that delays spill writes until the engine is idle.
//
//   g++ -std=c++20 src/poly/spill_test.cpp src/poly/spill.cpp -o spill_test && ./spill_test
#include "spill.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using strata::poly::SpillGate;

namespace {

int checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

// idle_seconds == 0: wait_for_idle returns immediately even when busy() is set.
void idle_s_zero() {
    SpillGate gate;
    gate.busy();
    auto start = std::chrono::steady_clock::now();
    check(gate.wait_for_idle(), "idle_s==0 returns immediately even when busy");
    auto elapsed = std::chrono::steady_clock::now() - start;
    check(elapsed < std::chrono::milliseconds(50), "idle_s==0 returns at once even when busy");
}

// busy() -> wait_for_idle blocks until idle() is called, then returns.
// Uses the condition seam (returns false initially, true after idle()) to keep it fast.
void busy_then_idle() {
    SpillGate gate;
    gate.idle_seconds(5);
    gate.busy();

    bool ready = false;
    bool done = false;
    std::thread t([&] {
        check(gate.wait_for_idle(nullptr, [&] { return ready; }), "wait_for_idle returns when the condition holds");
        done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(!done, "wait_for_idle blocks while busy");

    gate.idle();
    ready = true;
    t.join();
    check(done, "wait_for_idle returns after idle()");
}

// idle_seconds > 0 with a probe: wait stays false until the probe's condition holds.
void probe_with_idle_s_positive() {
    SpillGate gate;
    gate.idle_seconds(3);
    gate.busy();

    bool probe_ready = false;
    bool done = false;
    std::thread t([&] {
        check(gate.wait_for_idle([&] { return probe_ready; }), "wait_for_idle with probe returns when probe is true");
        done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(!done, "wait_for_idle with probe stays false");

    probe_ready = true;
    gate.idle();
    t.join();
    check(done, "wait_for_idle with probe returns when probe is true");
}

// The bug case: busy() after idle() must block even though idle_since_ is old.
// OR semantics would return immediately; AND semantics block until idle() clears busy_.
void busy_blocks_even_after_long_idle() {
    SpillGate gate;
    gate.idle_seconds(5);
    gate.idle();   // idle_since_ is set
    gate.busy();   // busy_ = true, idle_since_ is old

    bool ready = false;
    bool done = false;
    std::thread t([&] {
        check(gate.wait_for_idle(nullptr, [&] { return ready; }), "wait_for_idle returns when the condition holds");
        done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(!done, "wait_for_idle blocks even when idle_since_ is old");

    ready = true;
    gate.idle();
    t.join();
    check(done, "wait_for_idle returns when ready is set");
}

// The heap counter (spill_heap_bytes): a Spill's bytes count from its making, a release() that writes them
// out takes them off again (idle_seconds 0: the writer runs at once), and one destroyed before that returns
// them with its destructor.
void heap_counter() {
    using strata::poly::Spill;
    using strata::poly::spill_heap_bytes;
    SpillGate::instance().idle_seconds(0);
    const int64_t before = spill_heap_bytes();
    {
        auto s = std::make_shared<Spill>(size_t(1) << 20);
        check(s->data() != nullptr, "a Spill of 1 MiB is made");
        check(spill_heap_bytes() >= before + (int64_t)(size_t(1) << 20), "its bytes are on the counter");
        s->release();   // hands the write to the writer's thread
        for (int i = 0; i < 300 && spill_heap_bytes() > before; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(spill_heap_bytes() <= before, "the write freed the heap copy and the counter with it");
    }
    {
        auto s = std::make_shared<Spill>(size_t(1) << 20);
        check(s->data() != nullptr, "a second Spill is made");
        check(spill_heap_bytes() >= before + (int64_t)(size_t(1) << 20), "its bytes are on the counter too");
    }   // destroyed without a release(): ~Spill frees the heap at once
    check(spill_heap_bytes() <= before, "a Spill destroyed before its write returns its bytes at once");
}

// The cap: a busy gate whose window never opens (a server whose requests come closer together than
// idle_seconds) must not hold the writes back forever - past max_wait_ms the wait returns anyway.
void max_wait_caps_a_busy_gate() {
    SpillGate gate;
    gate.idle_seconds(5);
    gate.busy();
    const auto start = std::chrono::steady_clock::now();
    check(gate.wait_for_idle(nullptr, nullptr, 300), "the cap ends the wait of a gate that never goes idle");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    check(elapsed >= 250 && elapsed < 2000, "the wait ended within a slice or two past its cap");
}

#ifdef _WIN32
// The startup sweep: a temp file named after a dead PID is junk and goes; one named after this process
// stays - the rule must never eat a live process's files, even when a PID was reused.
void sweep_removes_only_dead_pids() {
    wchar_t dir[MAX_PATH + 1];
    check(GetTempPathW(MAX_PATH, dir) != 0, "the temp path is there");
    const std::wstring dead = std::wstring(dir) + L"strata-poly-999999999-0.tmp";
    const std::wstring mine =
        std::wstring(dir) + L"strata-poly-" + std::to_wstring(GetCurrentProcessId()) + L"-0.tmp";
    for (const std::wstring& path : {dead, mine}) {
        const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        check(f != INVALID_HANDLE_VALUE, "the test file is made");
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    strata::poly::sweep_orphan_temp_files();
    check(GetFileAttributesW(dead.c_str()) == INVALID_FILE_ATTRIBUTES, "the dead PID's file was swept");
    check(GetFileAttributesW(mine.c_str()) != INVALID_FILE_ATTRIBUTES, "the live PID's file stayed");
    DeleteFileW(mine.c_str());
}
#endif

}  // namespace

int main() {
    idle_s_zero();
    busy_then_idle();
    probe_with_idle_s_positive();
    busy_blocks_even_after_long_idle();
    max_wait_caps_a_busy_gate();
    heap_counter();
#ifdef _WIN32
    sweep_removes_only_dead_pids();
#endif
    std::printf("spill_test: %d checks passed\n", checks);
    return 0;
}
