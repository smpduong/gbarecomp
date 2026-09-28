// runtime_bus_bridge.h — public surface for binding the active bus
// to the recompiled-code runtime.

#pragma once

namespace gba { class GbaBus; }
namespace gba { class GbaPpu; }

// Count of PPU VBlank-start events (scanline 159->160), incremented in
// runtime_tick. The debug step-one-frame primitive stops on its increment
// so the recomp's TCP `step` parks at VBlank-start, matching the
// interpreter and mGBA oracles. Defined in runtime_bus_bridge.cpp.
extern "C" unsigned long long g_runtime_vblank_starts;

namespace gbarecomp {

// Install the active bus pointer. Subsequent bus_read_u*/bus_write_u*
// calls from generated code (declared in src/armv4t/runtime_arm.h)
// will delegate to this bus.
void set_active_bus(gba::GbaBus* bus);
void set_active_ppu(gba::GbaPpu* ppu);

// Retrieve the currently-bound bus / ppu, or nullptr if none.
gba::GbaBus* active_bus();
gba::GbaPpu* active_ppu();

}  // namespace gbarecomp

// Host-control yield gate (defined in runtime_bus_bridge.cpp, consumed by
// runtime.cpp's runner and the frame-present tests). Host controls that
// replace guest state must wait for a clean outer-loop boundary: User/System
// mode with no live synchronous exception (IRQ nest depth zero).
bool runtime_host_unwind_safe();
void runtime_request_host_control_yield();
void runtime_clear_host_control_yield();

// Snapshot/restore diagnostics (read-only; defined in
// runtime_bus_bridge.cpp). Reports guest cycles advanced but not yet
// materialized into device state, and cycles remaining until the next
// scheduled device event. Neither is serialized.
extern "C" void runtime_pending_cycle_stats(unsigned long long* pending,
                                            long long* budget);

// Read-only source-audio sample marker at the save/load boundary: the guest
// mixer's absolute sample counter (serialized with the snapshot). Lets an
// offline harness align post-restore audio by exact source sample instead of
// guest-frame arithmetic. Defined in runtime_bus_bridge.cpp.
extern "C" unsigned long long runtime_audio_sample_marker(void);

// Opt-in save/load phase normalization (see GBARECOMP_SAVELOAD_PHASE_SYNC
// in runtime.cpp). Drops stale host-side pending cycles after a restore
// and re-arms the horizon from live devices. Read-only w.r.t. guest state.
extern "C" void runtime_load_phase_reset(void);

// Materialize lagged device state up to 'now' (defined in
// runtime_bus_bridge.cpp). Fires no new events: the flushed delta never
// reaches past the already-computed horizon.
extern "C" void runtime_mmio_catch_up(void);
