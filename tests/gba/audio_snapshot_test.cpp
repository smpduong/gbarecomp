// Fixture test: GbaAudio snapshot round-trip with all PSG voices live.
//
// Programs square (ch1/ch2), wave-RAM (ch3) and noise (ch4) voices with
// steady envelopes and disabled lengths, runs 0.5 s, snapshots, runs 0.5 s
// more (segment A), restores into a fresh object, runs 0.5 s (segment B).
// Two modes, both strict (no WILL_FAIL — a crash, setup failure, or wrong
// result all exit nonzero distinctly):
//   --check=preserved : serialized channels (ch1/ch2) restore BIT-EXACTLY.
//   --check=omitted   : passes only by demonstrating the specific known
//     gap — ch1/ch2 bit-exact, ch3/ch4 silent, early first difference.
//     Flip omitted to a full-equivalence requirement only with a versioned
//     serializer change plus compatibility plan; never by weakening it.
//
// Assertions are SAMPLE-LEVEL, not energy-level: the per-voice capture ring
// (CapSample::ch[voice], the raw PSG contribution) is compared element by
// element over a fixed window behind each stream head. A negative control
// perturbs one sample and requires the comparator to reject it, so an
// accidentally vacuous comparison fails loudly. Segment lengths are the TRUE
// drained counts (no resize()/zero-pad); a short or padded stream fails.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "gba_audio.h"
#include "snapshot.h"

namespace {
// No generic check helper: each mode returns explicit pass/fail below,
// so setup failures and crashes stay distinguishable from verdicts.

// 0.5 s at the 32768 Hz bridge rate.
constexpr uint32_t kHalfSecSamples = 16384u;
// Window of capture-ring samples compared per voice (same distance behind
// each stream head, so a phase-correct restore lines up exactly).
constexpr std::size_t kVoiceWindow = 4096u;

void program_voices(gba::GbaAudio& audio) {
    using gba::GbaAudio;
    // Master on, full volume, all channels to both outputs.
    audio.write_io8(0x84, 0x80);  // NR52 master enable
    audio.write_io8(0x80, 0x77);  // NR50
    audio.write_io8(0x81, 0xFF);  // NR51
    // Sound1: 50% square, steady vol 15, 440 Hz, length disabled.
    // (GBA byte map: duty/length 0x62, envelope 0x63, freq low 0x64,
    // freq high + control 0x65.)
    audio.write_io8(0x62, 0x80);  // NR11 duty 50%
    audio.write_io8(0x63, 0xF0);  // NR12 steady
    audio.write_io8(0x64, 0xD6);  // NR13 freq low (N=0x6D6)
    audio.write_io8(0x65, 0x86);  // NR14 trigger, no length
    // Sound2: 25% square, steady vol 12, 660 Hz, length disabled.
    audio.write_io8(0x68, 0x40);  // NR21 duty 25%
    audio.write_io8(0x69, 0xC0);  // NR22 steady
    audio.write_io8(0x6C, 0x5B);  // NR23 freq low (N=0x449)
    audio.write_io8(0x6D, 0x84);  // NR24 trigger, no length
    // Sound3: wave RAM ascending nibbles, DAC on, full volume.
    for (int i = 0; i < 16; ++i)
        audio.write_io8(0x90 + i, static_cast<uint8_t>((i << 4) | i));
    audio.write_io8(0x70, 0x80);  // NR30 DAC on
    audio.write_io8(0x72, 0x00);  // NR31 length (unused: disabled below)
    audio.write_io8(0x73, 0x20);  // NR32 volume 100%
    audio.write_io8(0x74, 0x00);  // NR33 freq low
    audio.write_io8(0x75, 0x87);  // NR34 trigger (N=0x700), no length
    // Sound4: steady vol 15 noise, length disabled.
    // (GBA byte map: length 0x78, envelope 0x79, divisor 0x7C,
    // control 0x7D.)
    audio.write_io8(0x78, 0x00);  // NR41
    audio.write_io8(0x79, 0xF0);  // NR42 steady
    audio.write_io8(0x7C, 0x00);  // NR43 ratio 0, 15-bit
    audio.write_io8(0x7D, 0x80);  // NR44 trigger, no length
}

// Advance `seconds`, draining the mixed playback ring. The returned vector
// holds the TRUE drained count: it is never resized or zero-padded, so a
// shortfall or surplus is visible to the caller (an earlier revision called
// resize(want), which masked exactly the defect this test must catch).
std::vector<int16_t> run_seconds(gba::GbaAudio& audio, double seconds,
                                 std::size_t expected) {
    std::vector<int16_t> out;
    out.reserve(expected + 64);
    std::vector<int16_t> tmp(4096);
    const uint32_t frame_cycles = 280896u;
    uint32_t advanced = 0;
    const auto total =
        static_cast<uint32_t>(seconds * gba::GbaAudio::kSystemHz);
    while (advanced < total) {
        const uint32_t step =
            total - advanced > frame_cycles ? frame_cycles : total - advanced;
        audio.tick(step);
        advanced += step;
        while (out.size() < expected) {
            const std::size_t n =
                audio.drain_samples(tmp.data(), tmp.size());
            if (n == 0) break;
            out.insert(out.end(), tmp.begin(), tmp.begin() + n);
        }
    }
    return out;
}

// Last `window` raw per-voice samples (CapSample::ch[voice]) ending at the
// live capture head. Non-mutating; the capture ring is never drained.
std::vector<int16_t> capture_voice(const gba::GbaAudio& audio, int voice,
                                   std::size_t window) {
    const uint64_t head = audio.samples_generated();
    const uint64_t start = head > window ? head - window : 0;
    const std::size_t count = static_cast<std::size_t>(head - start);
    std::vector<gba::GbaAudio::CapSample> buf(count);
    uint64_t first = 0;
    const std::size_t got = audio.query_capture(start, count, buf.data(),
                                                first);
    std::vector<int16_t> out(got);
    for (std::size_t i = 0; i < got; ++i) out[i] = buf[i].ch[voice];
    return out;
}

// First index where two windows differ; equals the compared length when the
// windows are identical (or when only the lengths differ).
std::size_t first_diff(const std::vector<int16_t>& a,
                       const std::vector<int16_t>& b) {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return i;
    return n;
}

double energy(const std::vector<int16_t>& v) {
    double e = 0.0;
    for (int16_t s : v) e += static_cast<double>(s) * s;
    return v.empty() ? 0.0 : e / v.size();
}

bool all_zero(const std::vector<int16_t>& v) {
    for (int16_t s : v) if (s != 0) return false;
    return !v.empty();
}

}  // namespace

int main(int argc, char** argv) {
    // Modes (both must exit 0 when their condition holds; setup failures
    // and crashes exit nonzero and can never masquerade as demonstrated):
    //   --check=preserved : serialized channels (ch1/ch2) restore bit-exactly.
    //   --check=omitted   : omitted channels (ch3/ch4/wave) are lost
    //                       post-restore while ch1/ch2 stay bit-exact, with an
    //                       early first difference in the mixed stream.
    std::string mode = "preserved";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const std::string prefix = "--check=";
        if (arg.compare(0, prefix.size(), prefix) == 0)
            mode = arg.substr(prefix.size());
    }
    if (mode != "preserved" && mode != "omitted") {
        std::printf("FAIL: unknown --check mode '%s'\n", mode.c_str());
        return 2;
    }
    gba::GbaAudio audio;
    program_voices(audio);
    (void)run_seconds(audio, 0.5, kHalfSecSamples);  // settle past transients

    gbarecomp::debug::SnapshotWriter w;
    audio.serialize(w);
    const std::vector<uint8_t> blob = w.take_buffer();

    const std::vector<int16_t> seg_a = run_seconds(audio, 0.5, kHalfSecSamples);
    const uint64_t gen_a = audio.samples_generated();

    gba::GbaAudio restored;
    gbarecomp::debug::SnapshotReader r(blob.data(), blob.size());
    restored.deserialize(r);
    if (!r.ok()) {
        std::printf("FAIL: snapshot round-trips without truncation\n");
        return 2;
    }
    const std::vector<int16_t> seg_b =
        run_seconds(restored, 0.5, kHalfSecSamples);
    const uint64_t gen_b = restored.samples_generated();

    // Real length check (no resize/zero-pad): short or padded output fails.
    if (seg_a.size() != kHalfSecSamples || seg_b.size() != kHalfSecSamples) {
        std::printf("FAIL: drained lengths %zu/%zu, want %u "
                    "(short or padded output, not resized to fit)\n",
                    seg_a.size(), seg_b.size(), kHalfSecSamples);
        return 2;
    }
    // samples_generated_ is serialized; the restored clock must continue
    // where the snapshot left off, or the comparison windows are misaligned.
    if (gen_a != gen_b) {
        std::printf("FAIL: sample clock not restored: %llu vs %llu\n",
                    static_cast<unsigned long long>(gen_a),
                    static_cast<unsigned long long>(gen_b));
        return 1;
    }

    // Per-voice sample-level comparison (bit-exact), the actual claim.
    std::vector<int16_t> va[4], vb[4];
    bool voices_live = true;
    for (int v = 0; v < 4; ++v) {
        va[v] = capture_voice(audio, v, kVoiceWindow);
        vb[v] = capture_voice(restored, v, kVoiceWindow);
        if (va[v].empty() || va[v].size() != kVoiceWindow ||
            vb[v].size() != kVoiceWindow) {
            std::printf("FAIL: voice ch%d windows %zu/%zu, want %zu\n", v + 1,
                        va[v].size(), vb[v].size(), kVoiceWindow);
            return 2;
        }
        std::printf("voice ch%d window=%zu exact=%d pre_energy=%.1f "
                    "post_energy=%.1f\n",
                    v + 1, va[v].size(), (int)(first_diff(va[v], vb[v]) ==
                                               va[v].size()),
                    energy(va[v]), energy(vb[v]));
        if (energy(va[v]) == 0.0) voices_live = false;
    }
    if (!voices_live) {
        std::printf("FAIL: a voice was not live pre-save (fixture setup)\n");
        return 2;
    }

    // Negative control: the comparator must reject a one-sample perturbation
    // and must confirm a self-comparison. If this ever passes vacuously the
    // exactness assertions below are meaningless.
    {
        std::vector<int16_t> perturbed = va[0];
        perturbed[perturbed.size() / 2] =
            static_cast<int16_t>(perturbed[perturbed.size() / 2] ^ 1);
        const bool rejects = first_diff(perturbed, va[0]) < va[0].size();
        const bool accepts = first_diff(va[0], va[0]) == va[0].size();
        if (!rejects || !accepts) {
            std::printf("FAIL: comparator negative control "
                        "(rejects=%d accepts=%d)\n", (int)rejects,
                        (int)accepts);
            return 2;
        }
    }

    const bool ch12_exact = first_diff(va[0], vb[0]) == va[0].size() &&
                            first_diff(va[1], vb[1]) == va[1].size();
    if (mode == "preserved") {
        std::printf("preserved ch1/ch2 sample arrays %s\n",
                    ch12_exact ? "bit-exact" : "MISMATCH");
        return ch12_exact ? 0 : 1;
    }

    const bool ch34_lost = all_zero(vb[2]) && all_zero(vb[3]);
    std::size_t mixed_first = seg_a.size();
    for (std::size_t i = 0; i < seg_a.size(); ++i) {
        if (seg_a[i] != seg_b[i]) {
            mixed_first = i;
            break;
        }
    }
    std::printf("post-restore mixed first differing sample: %s\n",
                mixed_first == seg_a.size()
                    ? "(none)"
                    : std::to_string(mixed_first).c_str());
    std::printf("post-restore ch3 window all-zero=%d ch4 window all-zero=%d\n",
                (int)all_zero(vb[2]), (int)all_zero(vb[3]));
    const bool demonstrated = ch12_exact && ch34_lost && mixed_first < 64;
    std::printf("omitted-gap %s (ch1/ch2 exact=%d ch3/ch4 silent=%d early=%d)\n",
                demonstrated ? "DEMONSTRATED" : "NOT reproduced",
                (int)ch12_exact, (int)ch34_lost, (int)(mixed_first < 64));
    return demonstrated ? 0 : 1;
}
