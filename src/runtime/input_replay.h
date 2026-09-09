// input_replay.h — frame-indexed KEYINPUT trace lookup (pure, unit-tested).
//
// Semantics (the contract the runner and the tests share):
//   * Events are sorted by frame (non-decreasing; enforced at load).
//   * The effective input at guest frame F is the LAST event with
//     frame <= F ("last-event-wins"); before the first event the pad is
//     released (0x03FF).
//   * Lookup seeks from F itself (upper_bound), never from a forward-only
//     cursor, so a save load or rewind that moves guest time backward still
//     resolves the correct input. Same-frame conflicting entries resolve to
//     the last one listed.

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace gbarecomp {

struct InputReplayEvent {
    uint64_t frame = 0;
    uint16_t keyinput = 0x03FFu;
};

inline uint16_t replay_keyinput_at(const std::vector<InputReplayEvent>& events,
                                   uint64_t frame) {
    const auto next = std::upper_bound(
        events.begin(), events.end(), frame,
        [](uint64_t f, const InputReplayEvent& event) {
            return f < event.frame;
        });
    return next == events.begin() ? 0x03FFu : (next - 1)->keyinput;
}

}  // namespace gbarecomp
