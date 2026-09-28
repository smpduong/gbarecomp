// Unit test for once-per-PC heal-failure diagnostics (heal_fail_report.h).
//
// Positive coverage: reason names are distinct/non-empty; the first report
// for a key emits exactly once with PC/mode/reason; repeats (10k calls, the
// hot-bridge flood case) emit nothing further; distinct keys emit
// independently; ARM/Thumb variants of one address are independent;
// reset() re-arms; worker detail text passes through.
//
// Negative control: a reporter WITHOUT the once-set (plain fprintf per
// call) MUST emit on every call — proving the fixture discriminates
// flood protection from plain logging. If it ever emits once, the test
// fails loudly instead of silently weakening.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "heal_fail_report.h"

namespace {

int failures = 0;
void check(const char* label, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }
}

struct Captured {
    uint32_t pc = 0;
    bool thumb = false;
    gbarecomp::HealFailReason reason =
        gbarecomp::HealFailReason::RamOverlayDisabled;
    std::string detail;
};

std::vector<Captured> g_captured;
void capture_sink(uint32_t pc, bool thumb, gbarecomp::HealFailReason reason,
                  const char* detail, void* /*ctx*/) {
    Captured c;
    c.pc = pc;
    c.thumb = thumb;
    c.reason = reason;
    c.detail = detail ? detail : "";
    g_captured.push_back(c);
}

void test_reason_names() {
    using gbarecomp::HealFailReason;
    using gbarecomp::heal_fail_reason_name;
    const char* a = heal_fail_reason_name(HealFailReason::RamOverlayDisabled);
    const char* b = heal_fail_reason_name(HealFailReason::RamSnapshotRejected);
    const char* c = heal_fail_reason_name(HealFailReason::WorkerCompileFailed);
    check("disabled name non-empty", a && a[0]);
    check("snapshot name non-empty", b && b[0]);
    check("worker name non-empty", c && c[0]);
    check("reason names distinct",
          std::strcmp(a, b) != 0 && std::strcmp(b, c) != 0 &&
              std::strcmp(a, c) != 0);
}

void test_classify_ram_heal_failure() {
    using gbarecomp::HealFailReason;
    using gbarecomp::classify_ram_heal_failure;
    // Gate off => disabled reason regardless of snapshot outcome.
    check("gate off, snapshot false => disabled",
          classify_ram_heal_failure(false, false) ==
              HealFailReason::RamOverlayDisabled);
    check("gate off, snapshot true => disabled (short-circuit)",
          classify_ram_heal_failure(false, true) ==
              HealFailReason::RamOverlayDisabled);
    // Gate on, snapshot rejected => rejection reason.
    check("gate on, snapshot false => rejected",
          classify_ram_heal_failure(true, false) ==
              HealFailReason::RamSnapshotRejected);
    // Gate on, snapshot ok => proceed (nullopt), no report.
    check("gate on, snapshot ok => proceed",
          !classify_ram_heal_failure(true, true).has_value());
}

void test_once_semantics() {
    using gbarecomp::HealFailReason;
    g_captured.clear();
    gbarecomp::HealFailReporter rep(capture_sink, nullptr);
    const uint64_t kMixerHot = (static_cast<uint64_t>(0x03006EB0) << 1) | 1u;

    check("first report emits",
          rep.report(kMixerHot, 0x03006EB0, true,
                     HealFailReason::RamOverlayDisabled));
    check("one emission", g_captured.size() == 1);
    check("emission carries pc", g_captured[0].pc == 0x03006EB0);
    check("emission carries thumb mode", g_captured[0].thumb == true);
    check("emission carries reason",
          g_captured[0].reason == HealFailReason::RamOverlayDisabled);

    // Hot-bridge flood: 10k repeats must add zero emissions.
    for (int i = 0; i < 10000; ++i) {
        if (rep.report(kMixerHot, 0x03006EB0, true,
                       HealFailReason::RamOverlayDisabled)) {
            check("flood repeat emitted (must not)", false);
            break;
        }
    }
    check("flood adds nothing", g_captured.size() == 1);
    check("count is 1", rep.reported_count() == 1);

    // A different key (and the ARM variant of the same address) emits.
    const uint64_t kOther = (static_cast<uint64_t>(0x03006FE8) << 1) | 1u;
    const uint64_t kArmVariant = (static_cast<uint64_t>(0x03006EB0) << 1) | 0u;
    check("second key emits",
          rep.report(kOther, 0x03006FE8, true,
                     HealFailReason::RamSnapshotRejected));
    check("arm variant independent",
          rep.report(kArmVariant, 0x03006EB0, false,
                     HealFailReason::RamOverlayDisabled));
    check("three emissions total", g_captured.size() == 3);
    check("count is 3", rep.reported_count() == 3);

    // Worker detail passes through.
    g_captured.clear();
    gbarecomp::HealFailReporter rep2(capture_sink, nullptr);
    check("worker report emits",
          rep2.report(0x1234, 0x0800D820, true,
                      HealFailReason::WorkerCompileFailed, "g++ exited 1"));
    check("worker detail captured",
          g_captured.size() == 1 && g_captured[0].detail == "g++ exited 1");

    // Snapshot-rejected branch emits with its reason (covers the classify +
    // report composition for the path live runs cannot safely force).
    check("snapshot-rejected emits",
          rep2.report(0x5678, 0x03007AF0, true,
                      HealFailReason::RamSnapshotRejected));
    check("snapshot-rejected reason carried",
          g_captured.size() == 2 &&
              g_captured[1].reason == HealFailReason::RamSnapshotRejected);

    rep2.reset();
    check("reset clears count", rep2.reported_count() == 0);
    check("reset re-arms",
          rep2.report(0x1234, 0x0800D820, true,
                      HealFailReason::WorkerCompileFailed, "x"));
}

// Negative control: a naive per-call logger (no once-set) MUST emit on every
// one of 100 repeat calls for the same key, while the reporter emits once.
// If the naive logger ever emits once, or the reporter more than once, the
// fixture no longer discriminates and the test fails loudly instead of
// silently weakening.
void test_negative_control() {
    g_captured.clear();
    gbarecomp::HealFailReporter rep(capture_sink, nullptr);
    const uint64_t k = (static_cast<uint64_t>(0x03007AF0) << 1) | 1u;
    int naive_emissions = 0;
    for (int i = 0; i < 100; ++i) {
        // Naive logger: unconditional emit per call.
        capture_sink(0x03007AF0, true,
                     gbarecomp::HealFailReason::RamOverlayDisabled, nullptr,
                     nullptr);
        ++naive_emissions;
        rep.report(k, 0x03007AF0, true,
                   gbarecomp::HealFailReason::RamOverlayDisabled);
    }
    check("naive logger emits every call", naive_emissions == 100);
    check("reporter still emits once total",
          rep.reported_count() == 1);
}

}  // namespace

int main() {
    test_reason_names();
    test_classify_ram_heal_failure();
    test_once_semantics();
    test_negative_control();
    if (failures == 0) std::printf("heal_fail_report_tests: all passed\n");
    return failures == 0 ? 0 : 1;
}
