// Regression for non-unwinding frame service during synchronous exceptions.
// Synthetic CPU state only: no ROM, BIOS, audio device, or user save is needed.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

extern "C" uint32_t g_irq_nest_depth;
extern "C" unsigned long long g_runtime_cycles;
void runtime_set_frame_present_hook(std::function<bool()>);
bool runtime_host_unwind_safe();
void runtime_request_host_control_yield();
void runtime_clear_host_control_yield();

namespace {
int failures = 0;
void check(const char* label, bool ok) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", label); ++failures; }
}
void state(uint32_t mode, uint32_t depth) {
    g_cpu = {};
    g_cpu.cpsr = mode | CPSR_I_BIT;
    g_cpu.R[15] = 0x08000100u;
    g_cpu.R[0] = 0x12345678u;
    g_irq_nest_depth = depth;
}
void next_vblank() { ++g_runtime_vblank_starts; }

void test_legacy_unwind_guard() {
    runtime_set_frame_present_hook(nullptr);
    state(0x12u, 1u);
    next_vblank();
    check("legacy IRQ does not unwind", !runtime_should_yield());
    state(0x1Fu, 1u);
    check("IRQ switched to System does not unwind", !runtime_should_yield());
    g_irq_nest_depth = 0u;
    check("legacy deferred VBlank unwinds after IRQ", runtime_should_yield());
    check("legacy VBlank unwinds only once", !runtime_should_yield());
}

void test_protected_presentation() {
    unsigned calls = 0;
    runtime_set_frame_present_hook([&] { ++calls; return false; });
    const uint32_t modes[] = {0x12u, 0x1Fu, 0x10u, 0x13u};
    const uint32_t depths[] = {1u, 1u, 2u, 0u};
    for (unsigned i = 0; i < 4; ++i) {
        state(modes[i], depths[i]);
        const ArmCpuState cpu_before = g_cpu;
        const auto cycles_before = g_runtime_cycles;
        next_vblank();
        check("protected callback does not unwind", !runtime_should_yield());
        check("protected callback runs once", calls == i + 1u);
        check("CPU unchanged by presentation",
              std::memcmp(&cpu_before, &g_cpu, sizeof(g_cpu)) == 0);
        check("IRQ depth unchanged", g_irq_nest_depth == depths[i]);
        check("guest cycles unchanged", g_runtime_cycles == cycles_before);
        check("same VBlank does not unwind", !runtime_should_yield());
        check("same VBlank not presented twice", calls == i + 1u);
    }
    state(0x1Fu, 0u);
    check("already serviced frame not re-yielded", !runtime_should_yield());
    next_vblank();
    check("ordinary presentation also resumes in place", !runtime_should_yield());
    check("ordinary callback called", calls == 5u);
}

void test_quit_is_deferred() {
    unsigned calls = 0;
    runtime_set_frame_present_hook([&] { ++calls; return true; });
    state(0x12u, 1u);
    next_vblank();
    check("quit does not abandon IRQ", !runtime_should_yield());
    check("sticky quit does not spin same IRQ PC", !runtime_should_yield());
    state(0x1Fu, 1u);
    check("quit stays pending in System-mode IRQ", !runtime_should_yield());
    next_vblank();
    check("pending quit keeps IRQ presentation alive", !runtime_should_yield());
    check("two protected frames serviced", calls == 2u);
    g_irq_nest_depth = 0u;
    check("quit unwinds immediately after handler", runtime_should_yield());
    check("quit remains sticky across host call chain", runtime_should_yield());
    runtime_set_frame_present_hook(nullptr);
    check("reset hook clears pending quit", !runtime_should_yield());
}

void test_host_control_is_deferred() {
    runtime_set_frame_present_hook(nullptr);
    state(0x13u, 0u);
    runtime_request_host_control_yield();
    check("state control defers in SVC", !runtime_should_yield());
    state(0x1Fu, 2u);
    check("state control defers in nested IRQ", !runtime_should_yield());
    g_irq_nest_depth = 0u;
    check("state control unwinds when safe", runtime_should_yield());
    check("state control remains sticky until outer loop", runtime_should_yield());
    runtime_clear_host_control_yield();
    check("outer loop can clear control request", !runtime_should_yield());

    state(0x12u, 1u);
    runtime_set_frame_present_hook([] {
        runtime_request_host_control_yield();
        return false;
    });
    next_vblank();
    check("control requested within IRQ callback does not unwind",
          !runtime_should_yield());
    state(0x1Fu, 0u);
    check("callback control request preserved after handler", runtime_should_yield());
    runtime_set_frame_present_hook(nullptr);
}

void test_call_depth_does_not_unwind_irq() {
    runtime_init(nullptr);
    for (unsigned i = 0; i < 512u; ++i)
        runtime_call_push_return(0x08001000u + i * 4u);
    runtime_set_frame_present_hook([] { return false; });
    state(0x1Fu, 1u);
    next_vblank();
    check("depth limit does not unwind IRQ", !runtime_should_yield());
    check("protected service preserves host return stack",
          runtime_call_stack_depth() == 512u);
    g_irq_nest_depth = 0u;
    next_vblank();
    check("depth limit still unwinds ordinary mode", runtime_should_yield());
    runtime_init(nullptr);
    runtime_set_frame_present_hook(nullptr);
}
} // namespace

int main() {
#if defined(_WIN32)
    _putenv_s("GBARECOMP_HANG_WATCHDOG", "0");
    _putenv_s("GBARECOMP_YIELD_ON_VBLANK", "1");
    _putenv_s("GBARECOMP_PRESENT_IN_PLACE_CALL_DEPTH", "512");
#else
    setenv("GBARECOMP_HANG_WATCHDOG", "0", 1);
    setenv("GBARECOMP_YIELD_ON_VBLANK", "1", 1);
    setenv("GBARECOMP_PRESENT_IN_PLACE_CALL_DEPTH", "512", 1);
#endif
    gbarecomp::set_active_bus(nullptr);
    gbarecomp::set_active_ppu(nullptr);
    runtime_init(nullptr);
    test_legacy_unwind_guard();
    test_protected_presentation();
    test_quit_is_deferred();
    test_host_control_is_deferred();
    test_call_depth_does_not_unwind_irq();
    state(0x1Fu, 0u);
    std::printf("frame_present_tests: %s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
