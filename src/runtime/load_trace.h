#pragma once

// Live dynamic-load trace — GBARECOMP_LOAD_TRACE=1, opt-in, default OFF.
//
// WHY THIS EXISTS
// A macOS event-pump stall showed the emulation thread parked on dyld's
// loaders lock. dyld holds that lock while it maps images, and this engine is
// itself a dlopen() user: every self-heal install of a compiled overlay shard
// goes through dlopen (overlay_compile.cpp). To decide whether the engine's
// own loads or some other dlopen in the process is the holder, we need the
// engine's load windows on a clock that can be compared with the SDL-call
// windows printed by the GBARECOMP_SDL_COST probe.
//
// WHAT IT DOES
// Wraps the engine's dlopen() and prints one stderr line per load:
//
//   [load-trace] begin_us=<n> end_us=<n> wall_us=<n> tid=<n> path=<...>
//
// begin_us/end_us are microseconds on the process-wide monotonic clock, the
// same clock the [sdl-cost] probe stamps as mono_us, so an SDL call window
// [mono_us - wall_us, mono_us] and a load window [begin_us, end_us] can be
// intersected arithmetically by a log analyzer. Nothing is sampled, injected
// or suspended: the cost when the toggle is off is one cached bool test.
//
// LIMITS (read before trusting a conclusion)
//   * It only covers loads this engine performs. AppKit / CoreAudio / dyld
//     soft-links (_sl_dlopen) and anything else in the process are invisible
//     to it; they need dyld's own DYLD_PRINT_APIS tracing to be named.
//   * The end_us stamp is written after dlopen() returns, i.e. after dyld has
//     released the loaders lock. A non-overlapping window therefore does not
//     by itself exonerate this load from having *started* a wait; the analyzer
//     must compare windows, not just their endpoints.
//   * Opening a log line is itself I/O on the loading thread, so an enabled
//     trace perturbs load timing (the same caveat as any print-based probe).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>

namespace gbarecomp {

// Microseconds on the process-wide monotonic clock. Used for both the
// [load-trace] windows here and the [sdl-cost] `mono_us` field, so the two
// streams share one time base without sharing an origin.
inline long long load_trace_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline bool load_trace_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("GBARECOMP_LOAD_TRACE");
        return e && *e && *e != '0';
    }();
    return on;
}

// One line for work that was moved OFF the loaders lock -- currently the
// executable-mapping pre-warm in overlay_compile.cpp. Printed only when the
// trace is on, so the A/B can show cost moving from [load-trace] windows to
// this line rather than disappearing.
inline void load_trace_note_prewarm(const char* path, long long wall_us) {
    if (!load_trace_enabled()) return;
    std::fprintf(stderr, "[load-trace] prewarm_wall_us=%lld path=%s\n",
                 wall_us, path ? path : "?");
    std::fflush(stderr);
}

// RAII around one dlopen()/LoadLibrary(): stamps the window and prints it.
// Non-copyable; construct on the stack of the thread performing the load.
struct LoadTraceScope {
    bool        on;
    long long   t0;
    const char* path;

    explicit LoadTraceScope(const char* p)
        : on(load_trace_enabled()), t0(on ? load_trace_now_us() : 0), path(p) {}
    ~LoadTraceScope() {
        if (!on) return;
        const long long t1 = load_trace_now_us();
        const unsigned long long tid = static_cast<unsigned long long>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
        std::fprintf(stderr,
                     "[load-trace] begin_us=%lld end_us=%lld wall_us=%lld "
                     "tid=%llu path=%s\n",
                     t0, t1, t1 - t0, tid, path ? path : "?");
        std::fflush(stderr);
    }
    LoadTraceScope(const LoadTraceScope&) = delete;
    LoadTraceScope& operator=(const LoadTraceScope&) = delete;
};

}  // namespace gbarecomp
