// heal_fail_report.h — once-per-PC diagnostics for heal failures.
//
// The self-heal background worker can fail a dispatch PC permanently for a
// session (RAM-overlay gate disabled, RAM snapshot rejected, worker compile
// error). Those paths must stay loud (one line per PC with PC/mode/reason)
// without flooding the log when a hot PC bridges thousands of times.
//
// This header is self-contained and header-only so the unit test can exercise
// the once-semantics with a counting sink and no engine linkage. Production
// wires one shared instance with the default stderr sink in overlay_loader.cpp.
// Game-thread paths (request, drain) and the worker thread share the instance;
// all state is mutex-guarded. Behavior of the heal gates is unchanged — this
// only reports.

#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

namespace gbarecomp {

// Why a dispatch PC can never heal to native this session.
enum class HealFailReason {
    // GBARECOMP_RAM_OVERLAY_HEAL is unset: RAM PCs stay on the interpreter
    // bridge by configuration, not by error.
    RamOverlayDisabled,
    // The gate is on but no snapshot could be taken (no bus/image yet, or the
    // PC is outside EWRAM/IWRAM so there is no immutable image to compile).
    RamSnapshotRejected,
    // The worker attempted the compile and it failed (err detail carries
    // the compiler message).
    WorkerCompileFailed,
};

inline const char* heal_fail_reason_name(HealFailReason reason) {
    switch (reason) {
        case HealFailReason::RamOverlayDisabled:
            return "RAM-overlay healing disabled (GBARECOMP_RAM_OVERLAY_HEAL unset)";
        case HealFailReason::RamSnapshotRejected:
            return "RAM snapshot unavailable (no bus/image or unsupported region)";
        case HealFailReason::WorkerCompileFailed:
            return "worker compile failed";
    }
    return "unknown heal failure";
}

// Pure classification of a RAM-heal request outcome, mirroring the call-site
// logic in overlay_request_compile (unit-testable without engine linkage).
// ram_gate_on: ram_overlay_heal_enabled(); snapshot_ok: snapshot_ram_region()
// result (only meaningful when ram_gate_on — the call site short-circuits).
// Returns the reason to report, or nullopt when healing proceeds.
inline std::optional<HealFailReason> classify_ram_heal_failure(
    bool ram_gate_on, bool snapshot_ok) {
    if (!ram_gate_on) return HealFailReason::RamOverlayDisabled;
    if (!snapshot_ok) return HealFailReason::RamSnapshotRejected;
    return std::nullopt;
}

// Once-per-key failure reporter. `key` must be the heal_key(pc, thumb) value
// so ARM/Thumb variants of one address report independently.
class HealFailReporter {
 public:
    // Sink receives one call per first-seen key. `detail` is non-null only
    // for WorkerCompileFailed (the compiler error text, may be empty).
    using Sink = void (*)(uint32_t pc, bool thumb, HealFailReason reason,
                          const char* detail, void* ctx);

    explicit HealFailReporter(Sink sink = nullptr, void* ctx = nullptr)
        : sink_(sink), ctx_(ctx) {}

    // Report a failure. Returns true iff this call emitted (first time for
    // this key). Thread-safe.
    bool report(uint64_t key, uint32_t pc, bool thumb, HealFailReason reason,
                const char* detail = nullptr) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!reported_.insert(key).second) return false;
        }
        if (sink_) {
            sink_(pc, thumb, reason, detail, ctx_);
        } else {
            default_sink(pc, thumb, reason, detail);
        }
        return true;
    }

    size_t reported_count() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return reported_.size();
    }

    void reset() {
        std::lock_guard<std::mutex> lk(mtx_);
        reported_.clear();
    }

 private:
    static void default_sink(uint32_t pc, bool thumb, HealFailReason reason,
                             const char* detail) {
        if (reason == HealFailReason::WorkerCompileFailed) {
            std::fprintf(stderr,
                         "self_heal: compile FAILED for 0x%08X (%s): %s - "
                         "staying on the interpreter bridge this session.\n",
                         pc, thumb ? "thumb" : "arm",
                         detail && detail[0] ? detail : "unknown error");
        } else {
            std::fprintf(stderr,
                         "self_heal: heal FAILED for 0x%08X (%s): %s - staying "
                         "on the interpreter bridge this session.\n",
                         pc, thumb ? "thumb" : "arm",
                         heal_fail_reason_name(reason));
        }
        std::fflush(stderr);
    }

    Sink sink_;
    void* ctx_;
    mutable std::mutex mtx_;
    std::unordered_set<uint64_t> reported_;
};

}  // namespace gbarecomp
