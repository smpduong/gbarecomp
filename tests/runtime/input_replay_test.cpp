// Unit test for frame-indexed KEYINPUT replay lookup (input_replay.h).
//
// Positive coverage: released-before-first-event, changing directions,
// button releases, same-frame conflicting entries (last-wins), and backward
// seeking after a simulated load/rewind (lookup is a pure function of the
// queried frame — no forward-only cursor).
//
// Negative controls (in-test deliberately broken alternatives): a forward-
// only cursor and a first-duplicate-wins lookup MUST fail on the same
// fixture. If either alternative ever passes, the fixture no longer
// discriminates and the test fails loudly instead of silently weakening.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "input_replay.h"

namespace {

int failures = 0;
void check(const char* label, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }
}

// GBA KEYINPUT is active-low; RELEASED == 0x03FF.
constexpr uint16_t kReleased = 0x03FFu;
constexpr uint16_t kUp = 0x03BFu;     // bit 6
constexpr uint16_t kDown = 0x037Fu;   // bit 7
constexpr uint16_t kLeft = 0x03DFu;   // bit 5
constexpr uint16_t kRight = 0x03EFu;  // bit 4
constexpr uint16_t kA = 0x03FEu;      // bit 0

// Fixture: hold Up, release, conflicting same-frame pair (Up then A),
// Down span, conflicting pair resolving to released.
std::vector<gbarecomp::InputReplayEvent> fixture() {
    using gbarecomp::InputReplayEvent;
    return {
        {100, kUp}, {140, kReleased}, {200, kUp}, {200, kA},
        {260, kDown}, {300, kLeft},   {300, kReleased},
    };
}

void test_lookup() {
    const auto ev = fixture();
    using gbarecomp::replay_keyinput_at;
    check("before-first-event released", replay_keyinput_at(ev, 0) == kReleased);
    check("before-first-event released (99)", replay_keyinput_at(ev, 99) == kReleased);
    check("hold Up at 100", replay_keyinput_at(ev, 100) == kUp);
    check("hold Up at 139", replay_keyinput_at(ev, 139) == kUp);
    check("release at 140", replay_keyinput_at(ev, 140) == kReleased);
    check("released at 199", replay_keyinput_at(ev, 199) == kReleased);
    check("same-frame conflict last-wins (200 -> A)",
          replay_keyinput_at(ev, 200) == kA);
    check("conflict holds at 259", replay_keyinput_at(ev, 259) == kA);
    check("Down at 260", replay_keyinput_at(ev, 260) == kDown);
    check("Down holds at 299", replay_keyinput_at(ev, 299) == kDown);
    check("conflict to released (300)", replay_keyinput_at(ev, 300) == kReleased);
    check("released holds past end", replay_keyinput_at(ev, 99999) == kReleased);
}

void test_backward_seek() {
    // Simulate: run forward to 280 (Down), restore to 150 (released), run to
    // 205 (A via the 200 conflict), rewind to 100 (Up). Each lookup depends
    // only on the queried frame.
    const auto ev = fixture();
    using gbarecomp::replay_keyinput_at;
    check("forward 280 -> Down", replay_keyinput_at(ev, 280) == kDown);
    check("restore 150 -> released", replay_keyinput_at(ev, 150) == kReleased);
    check("advance 205 -> A", replay_keyinput_at(ev, 205) == kA);
    check("rewind 100 -> Up", replay_keyinput_at(ev, 100) == kUp);
}

// Deliberately broken alternative 1: forward-only cursor. Correct only while
// frames advance monotonically; wrong after any backward restore.
struct ForwardCursor {
    const std::vector<gbarecomp::InputReplayEvent>& ev;
    std::size_t idx = 0;
    uint16_t cur = kReleased;
    uint16_t at(uint64_t frame) {
        while (idx < ev.size() && ev[idx].frame <= frame) {
            cur = ev[idx].keyinput;
            ++idx;
        }
        return cur;
    }
};

void test_forward_only_fails_fixture() {
    const auto ev = fixture();
    ForwardCursor c{ev};
    bool ok_forward = (c.at(280) == kDown);
    // Restore backward: the cursor cannot move back, so it still reports Down
    // at frame 150 where the correct answer is released.
    bool wrong_after_restore = (c.at(150) != kReleased);
    check("forward cursor correct while advancing", ok_forward);
    check("forward cursor WRONG after backward restore (control)",
          wrong_after_restore);
    if (!wrong_after_restore) {
        std::fprintf(stderr,
                     "CONTROL INVALID: fixture no longer discriminates "
                     "forward-only seeking\n");
        ++failures;
    }
}

// Deliberately broken alternative 2: first-duplicate-wins. A plausible
// implementation bug: lower_bound lands on the FIRST listed event at a
// conflict frame instead of the last. Correct on plain spans; wrong wherever
// two entries share a frame.
uint16_t first_wins_at(const std::vector<gbarecomp::InputReplayEvent>& ev,
                       uint64_t frame) {
    const auto it = std::lower_bound(
        ev.begin(), ev.end(), frame,
        [](const gbarecomp::InputReplayEvent& e, uint64_t f) {
            return e.frame < f;
        });
    if (it != ev.end() && it->frame == frame) return it->keyinput;
    if (it == ev.begin()) return kReleased;
    return (it - 1)->keyinput;
}

void test_first_wins_fails_fixture() {
    const auto ev = fixture();
    // First-wins agrees on plain spans...
    check("first-wins agrees at 139", first_wins_at(ev, 139) == kUp);
    // ...but reports Up at frame 200 (first listed) instead of A, and
    // resolves 300 to Left instead of released.
    const bool wrong_200 = (first_wins_at(ev, 200) != kA);
    const bool wrong_300 = (first_wins_at(ev, 300) != kReleased);
    check("first-wins WRONG at conflict 200 (control)", wrong_200);
    check("first-wins WRONG at conflict 300 (control)", wrong_300);
    if (!wrong_200 || !wrong_300) {
        std::fprintf(stderr,
                     "CONTROL INVALID: fixture no longer discriminates "
                     "first-duplicate-wins\n");
        ++failures;
    }
}

}  // namespace

int main() {
    test_lookup();
    test_backward_seek();
    test_forward_only_fails_fixture();
    test_first_wins_fails_fixture();
    std::printf("input_replay_tests: %s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
