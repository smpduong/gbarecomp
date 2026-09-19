#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"

// Non-periodic stereo signal: a pitch-aligned loop cannot accidentally make
// every entry/exit seam continuous as it can with a single 440 Hz test tone.
static void push_signal(rab_bridge& b, int count) {
    std::vector<int16_t> in(count * b.cfg.channels);
    for (int i = 0; i < count; ++i) {
        double t = (b.in_count + i) / b.cfg.source_rate;
        for (int c = 0; c < b.cfg.channels; ++c)
            in[i*b.cfg.channels+c] = static_cast<int16_t>(
                7000 * std::sin(2*RAB_PI*(437.3+c*91)*t) +
                5000 * std::sin(2*RAB_PI*(731.7+c*43)*t));
    }
    rab_push(&b, in.data(), count);
}

static bool uneven_batches() {
    rab_config c; rab_config_defaults(&c);
    c.channels = 1; c.source_rate = 65536.; c.host_rate = 65536.; c.target_ms = 40.;
    rab_bridge b{}; if (rab_init(&b, &c)) return false;
    // Exact guest cadence, but every pair is delivered as a 5ms/28ms split.
    // This models the recorded small/large chunks instead of uniform tones.
    const double frame_s = 280896. / 16777216.;
    unsigned p = 1, q = 0;
    uint64_t pushed = 0;
    int16_t output[512];
    while (p*frame_s < 20.) {
        if (p*frame_s <= q*512./c.host_rate) {
            uint64_t total = static_cast<uint64_t>(std::floor(p*frame_s*c.source_rate));
            if (p > 120 && p % 2) total -= 768;
            push_signal(b, static_cast<int>(total - pushed)); pushed = total; ++p;
        } else { rab_pull(&b, output, 512); ++q; }
    }
    const bool ok = b.stats.stretch_frames == 0 && b.stats.underrun_events == 0 &&
                    b.stats.overflow_drops == 0;
    std::printf("uneven-batch schedule: stretch=%llu underrun=%llu overflow=%llu\n",
        (unsigned long long)b.stats.stretch_frames, (unsigned long long)b.stats.underrun_events,
        (unsigned long long)b.stats.overflow_drops);
    rab_free(&b); return ok;
}

int main() {
    int failures = 0, worst_seam = 0;
    if (!uneven_batches()) ++failures;
    for (double rate : {44100., 48000., 65536., 96000.}) {
        for (int phase = 0; phase < 12; ++phase) {
            rab_config c; rab_config_defaults(&c);
            c.channels = 2; c.source_rate = 65536.; c.host_rate = rate;
            c.target_ms = 25.;
            rab_bridge b{};
            if (rab_init(&b, &c)) return 2;
            push_signal(b, 2500 + phase*31);
            int16_t sample[2]{}, previous[2]{};
            bool entered = false, resumed = false;
            for (int i = 0; i < 12000; ++i) {
                const bool was = b.concealing;
                rab_pull(&b, sample, 1);
                if (was != bool(b.concealing)) {
                    for (int ch = 0; ch < 2; ++ch)
                        worst_seam = std::max(worst_seam,
                            std::abs(int(sample[ch])-int(previous[ch])));
                    if (b.concealing) entered = true; else resumed = true;
                }
                std::copy_n(sample, 2, previous);
                if (entered && b.conceal_frames == 100)
                    push_signal(b, 2300); // enough to recover the target cushion
                if (resumed) break;
            }
            if (!entered || !resumed) ++failures;
            rab_free(&b);
        }
    }
    if (worst_seam > 100) ++failures;
    std::printf("entry/resume seam worst=%d S16 units (limit 100)\n", worst_seam);

    rab_config c; rab_config_defaults(&c);
    c.channels = 1; c.target_ms = 25.;
    rab_bridge b{}; if (rab_init(&b, &c)) return 2;
    push_signal(b, 2400);
    std::vector<int16_t> output(3000);
    rab_pull(&b, output.data(), 3000);
    const double stopped = b.out_pos;
    push_signal(b, 100); // 2ms of input must not restart an empty queue
    rab_pull(&b, output.data(), 100);
    if (b.out_pos != stopped) {
        std::puts("FAIL recovery consumed an underfilled queue"); ++failures;
    }
    rab_free(&b);
    if (rab_init(&b, &c)) return 2;
    push_signal(b, static_cast<int>(b.cap*2));
    const auto oldest = static_cast<int64_t>(b.in_count)-b.cap;
    if (static_cast<int64_t>(std::floor(b.out_pos))-b.half+1 < oldest) {
        std::puts("FAIL overflow overwrote samples still needed by the filter"); ++failures;
    }
    rab_free(&b);
    std::printf("continuity/recovery/history failures=%d\n", failures);
    return failures ? 1 : 0;
}
