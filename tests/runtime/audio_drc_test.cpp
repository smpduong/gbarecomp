#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"

static int bounded_concealment_test() {
    rab_config cfg;
    rab_config_defaults(&cfg);
    cfg.channels = 1;
    cfg.source_rate = 48000.0;
    cfg.host_rate = 48000.0;
    cfg.target_ms = 10.0;
    cfg.ring_ms = 100.0;
    cfg.preroll_ms = 10.0;
    cfg.stretch_limit_ms = 20.0;

    rab_bridge bridge{};
    if (rab_init(&bridge, &cfg) != 0) {
        std::fprintf(stderr, "rab_init failed\n");
        return 1;
    }

    std::vector<int16_t> input(2400);
    for (std::size_t i = 0; i < input.size(); ++i) {
        constexpr double kPi = 3.14159265358979323846;
        input[i] = static_cast<int16_t>(
            std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) /
                     cfg.source_rate) *
            12000.0);
    }
    rab_push(&bridge, input.data(), static_cast<int>(input.size()));

    std::vector<int16_t> output(6000);
    rab_pull(&bridge, output.data(), static_cast<int>(output.size()));

    rab_stats stats{};
    rab_get_stats(&bridge, &stats);
    const uint64_t limit_frames =
        static_cast<uint64_t>(cfg.stretch_limit_ms * cfg.host_rate / 1000.0);
    if (stats.stretch_frames == 0 || stats.stretch_frames > limit_frames) {
        std::fprintf(stderr,
                     "stall concealment exceeded limit: stretch=%llu limit=%llu\n",
                     static_cast<unsigned long long>(stats.stretch_frames),
                     static_cast<unsigned long long>(limit_frames));
        rab_free(&bridge);
        return 2;
    }

    const auto tail_begin = output.end() - 256;
    const int16_t tail_peak = *std::max_element(
        tail_begin, output.end(),
        [](int16_t a, int16_t b) { return std::abs(a) < std::abs(b); });
    if (std::abs(static_cast<int>(tail_peak)) > 1) {
        std::fprintf(stderr, "stalled output did not fade to silence: peak=%d\n",
                     static_cast<int>(tail_peak));
        rab_free(&bridge);
        return 3;
    }

    rab_free(&bridge);
    return 0;
}

namespace {

constexpr double kSourceRate = 65536.0;
constexpr double kGameRate = 59.7275;
constexpr int kCallbackFrames = 512;

struct TestBridge {
    rab_bridge bridge{};
    rab_config cfg{};
    bool valid = false;

    explicit TestBridge(double host_rate) {
        rab_config_defaults(&cfg);
        cfg.channels = 2;
        cfg.source_rate = kSourceRate;
        cfg.host_rate = host_rate;
        cfg.target_ms = 25.0;
        cfg.preroll_ms = 0.0;
        cfg.ring_ms = 300.0;
        valid = rab_init(&bridge, &cfg) == 0;
    }
    ~TestBridge() { if (valid) rab_free(&bridge); }
    TestBridge(const TestBridge&) = delete;
    TestBridge& operator=(const TestBridge&) = delete;
};

bool check(bool condition, const char* test, double rate,
           const char* message) {
    if (!condition)
        std::fprintf(stderr, "%s (host %.0f Hz): %s\n", test, rate, message);
    return condition;
}

bool monotonic(const rab_stats& before, const rab_stats& after) {
    return after.pushed_frames >= before.pushed_frames &&
           after.pulled_frames >= before.pulled_frames &&
           after.underrun_events >= before.underrun_events &&
           after.overflow_drops >= before.overflow_drops &&
           after.stretch_frames >= before.stretch_frames &&
           after.stretch_events >= before.stretch_events;
}

void push_tone(rab_bridge* bridge, int frames) {
    std::vector<int16_t> input(static_cast<std::size_t>(frames) * 2);
    for (int f = 0; f < frames; ++f) {
        const double t = static_cast<double>(bridge->stats.pushed_frames + f) /
                         bridge->cfg.source_rate;
        const auto sample = static_cast<int16_t>(
            std::sin(2.0 * RAB_PI * 440.0 * t) * 10000.0);
        input[2 * f] = sample;
        input[2 * f + 1] = static_cast<int16_t>(-sample);
    }
    rab_push(bridge, input.data(), frames);
}

int peak(const std::vector<int16_t>& output) {
    int result = 0;
    for (int16_t sample : output)
        result = std::max(result, std::abs(static_cast<int>(sample)));
    return result;
}

// A deterministic wall-clock schedule: one producer event per GBA frame and
// one consumer event per 512 host frames. Fractional source frames accumulate
// instead of rounding every game frame (which would introduce artificial drift).
bool cadence_test(double host_rate, bool producer_stall) {
    const char* name = producer_stall ? "producer stall/recovery" : "steady cadence";
    TestBridge fixture(host_rate);
    if (!check(fixture.valid, name, host_rate, "initialization failed")) return false;
    auto& bridge = fixture.bridge;
    const auto& cfg = fixture.cfg;
    const double frame_s = 1.0 / kGameRate;
    const double callback_s = kCallbackFrames / host_rate;
    const double duration_s = producer_stall ? 5.0 : 10.0;
    const double stall_start = 2.0;
    const double stall_end = 2.25;
    std::vector<int16_t> output(kCallbackFrames * cfg.channels);
    uint64_t producer_event = 1, consumer_event = 0;
    uint64_t expected_pushed = 0, expected_pulled = 0;
    rab_stats previous{};
    double first_real_s = -1.0, first_recovered_s = -1.0;
    double maximum_fill_ms = 0.0;
    bool silent_during_stall = false;
    double previous_correction = 0.0;
    rab_stats recovered_stats{};
    bool recovery_checkpoint = false;

    while (true) {
        const double producer_s = producer_event * frame_s;
        const double consumer_s = consumer_event * callback_s;
        const double now_s = std::min(producer_s, consumer_s);
        if (now_s >= duration_s) break;
        if (producer_s <= consumer_s) {
            const int frames = static_cast<int>(
                std::floor(producer_event * kSourceRate / kGameRate) -
                std::floor((producer_event - 1) * kSourceRate / kGameRate));
            if (!(producer_stall && now_s >= stall_start && now_s < stall_end)) {
                push_tone(&bridge, frames);
                expected_pushed += static_cast<uint64_t>(frames);
            }
            ++producer_event;
        } else {
            rab_pull(&bridge, output.data(), kCallbackFrames);
            expected_pulled += kCallbackFrames;
            ++consumer_event;
            const int output_peak = peak(output);
            if (output_peak > 100 && first_real_s < 0.0) first_real_s = now_s;
            if (producer_stall && now_s > stall_start + 0.15 && now_s < stall_end &&
                output_peak <= 1) silent_during_stall = true;
            if (producer_stall && now_s >= stall_end && output_peak > 100 &&
                first_recovered_s < 0.0) first_recovered_s = now_s;
            for (std::size_t i = 0; i < output.size(); i += 2) {
                if (!check(output[i] == -output[i + 1], name, host_rate,
                           "stereo channels lost their opposite polarity")) return false;
            }
            if (!check(output_peak <= 11000, name, host_rate,
                       "unexpected amplification/clipping")) return false;
            // The configured slew rate is per second, not per callback.
            const double allowed_delta = cfg.slew_pp_per_s / 100.0 * callback_s;
            if (!check(std::abs(bridge.corr - previous_correction) <= allowed_delta + 1e-12,
                       name, host_rate, "correction slew exceeded elapsed-time limit"))
                return false;
            previous_correction = bridge.corr;
        }
        rab_stats stats{};
        rab_get_stats(&bridge, &stats);
        if (!check(monotonic(previous, stats), name, host_rate,
                   "a cumulative counter moved backward") ||
            !check(stats.pushed_frames == expected_pushed &&
                   stats.pulled_frames == expected_pulled, name, host_rate,
                   "pushed/pulled counters disagree with scheduled frames") ||
            !check(std::isfinite(stats.last_correction) &&
                   std::abs(stats.last_correction) <= cfg.max_correction + 1e-12,
                   name, host_rate, "correction exceeded configured bound") ||
            !check(std::isfinite(rab_fill_ms(&bridge)) && rab_fill_ms(&bridge) >= 0.0,
                   name, host_rate, "invalid ring occupancy")) return false;
        previous = stats;
        maximum_fill_ms = std::max(maximum_fill_ms, rab_fill_ms(&bridge));
        if (producer_stall && now_s > stall_end + 0.5 && !recovery_checkpoint) {
            recovered_stats = stats;
            recovery_checkpoint = true;
        }
    }

    rab_stats stats{};
    rab_get_stats(&bridge, &stats);
    // Prime at 25 ms, so two ~16.74 ms source batches must suffice. Allow one
    // callback for scheduling and 3 ms for the configured fade to real output.
    if (!check(first_real_s >= frame_s &&
               first_real_s <= 2.0 * frame_s + callback_s + 0.003,
               name, host_rate, "startup did not emit real audio within two frames") ||
        !check(stats.overflow_drops == 0, name, host_rate,
               "normal cadence unexpectedly overflowed the ring") ||
        !check(maximum_fill_ms <= cfg.target_ms + 1000.0 * (frame_s + callback_s) + 5.0,
               name, host_rate, "normal cadence accumulated excessive backlog")) return false;
    if (producer_stall) {
        if (!check(silent_during_stall, name, host_rate,
                   "long stall failed to reach silence after bounded concealment") ||
            !check(stats.stretch_frames > 0 && stats.stretch_events > 0,
                   name, host_rate, "stall did not exercise concealment") ||
            !check(first_recovered_s >= stall_end &&
                   first_recovered_s <= stall_end + 2.0 * frame_s + callback_s + 0.003,
                   name, host_rate, "real audio failed to resume promptly") ||
            !check(recovery_checkpoint &&
                   stats.underrun_events == recovered_stats.underrun_events &&
                   stats.stretch_events == recovered_stats.stretch_events,
                   name, host_rate, "stable playback continued underrunning after recovery"))
            return false;
    } else if (!check(stats.underrun_events == 0 && stats.stretch_frames == 0,
                      name, host_rate, "steady cadence unexpectedly ran dry")) return false;
    std::printf("PASS %s host=%.0f first_audio=%.2fms max_fill=%.2fms\n",
                name, host_rate, first_real_s * 1000.0, maximum_fill_ms);
    return true;
}

bool controller_direction_test() {
    for (bool high : {false, true}) {
        TestBridge fixture(65536.0);
        if (!fixture.valid) return false;
        auto& bridge = fixture.bridge;
        push_tone(&bridge, high ? 4000 : 400);
        std::vector<int16_t> output(512 * 2);
        double previous_correction = 0.0;
        for (int callback = 0; callback < 150; ++callback) {
            const uint64_t before_in = bridge.in_count;
            const double before_out = bridge.out_pos;
            rab_pull(&bridge, output.data(), 512);
            if (high) {
                // Replenish consumed source frames; preserve a consistently
                // high queue while the controller reaches its positive limit.
                const int refill = static_cast<int>(std::llround(bridge.out_pos - before_out));
                push_tone(&bridge, refill);
                if (!check(bridge.in_count >= before_in, "controller direction", 65536.0,
                           "producer cursor moved backward")) return false;
            }
            if (!check(high ? bridge.corr >= previous_correction :
                              bridge.corr <= previous_correction,
                       "controller direction", 65536.0,
                       "correction moved in the wrong direction")) return false;
            previous_correction = bridge.corr;
        }
        const double expected = high ? fixture.cfg.max_correction :
                                       -fixture.cfg.max_correction;
        if (!check(std::abs(bridge.corr - expected) < 1e-9,
                   "controller direction", 65536.0,
                   "sustained fill error did not reach the correct bounded correction") ||
            !check(high ? bridge.cur_step > 1.0 : bridge.cur_step < 1.0,
                   "controller direction", 65536.0,
                   "resampler consumes in the wrong direction")) return false;
    }
    std::puts("PASS controller direction and correction bounds");
    return true;
}

bool callback_duration_test() {
    for (double host_rate : {44100.0, 48000.0, 65536.0, 96000.0}) {
        for (int callback_frames : {0, 128, 256, 512, 1024, 2048}) {
            TestBridge fixture(host_rate);
            if (!fixture.valid) return false;
            auto& bridge = fixture.bridge;
            push_tone(&bridge, 4000); // sustained positive error, well below capacity
            std::vector<int16_t> output(std::max(callback_frames, 1) * 2);
            rab_pull(&bridge, output.data(), callback_frames);
            const double expected_slew = fixture.cfg.slew_pp_per_s / 100.0 *
                                         callback_frames / host_rate;
            if (!check(std::abs(bridge.corr - expected_slew) < 1e-12,
                       "callback duration", host_rate,
                       "initial correction does not match callback duration") ||
                !check(bridge.stats.pulled_frames == static_cast<uint64_t>(callback_frames),
                       "callback duration", host_rate,
                       "variable-size callback frame counter is incorrect")) return false;
        }
    }
    std::puts("PASS callback-duration slew at 0/128/256/512/1024/2048 frames");
    return true;
}

bool overflow_test() {
    TestBridge fixture(48000.0);
    if (!fixture.valid) return false;
    auto& bridge = fixture.bridge;
    const int frames = static_cast<int>(bridge.cap * 2);
    push_tone(&bridge, frames);
    rab_stats before{};
    rab_get_stats(&bridge, &before);
    if (!check(before.pushed_frames == static_cast<uint64_t>(frames) &&
               before.overflow_drops > 0 &&
               (bridge.in_count - bridge.out_pos) <= bridge.cap,
               "overflow boundary", 48000.0, "overflow did not preserve bounded storage"))
        return false;
    std::vector<int16_t> output(512 * 2);
    rab_pull(&bridge, output.data(), 512);
    rab_stats after{};
    rab_get_stats(&bridge, &after);
    if (!check(monotonic(before, after) && after.pulled_frames == 512 && peak(output) > 100,
               "overflow boundary", 48000.0, "playback/counters failed after overflow"))
        return false;
    std::puts("PASS overflow boundary and resumed output");
    return true;
}

} // namespace

int main() {
    if (const int result = bounded_concealment_test()) return result;
    std::puts("PASS original bounded concealment and fade-to-silence regression");
    for (double host_rate : {44100.0, 48000.0, 65536.0, 96000.0}) {
        if (!cadence_test(host_rate, false) || !cadence_test(host_rate, true)) return 4;
    }
    if (!controller_direction_test()) return 5;
    if (!callback_duration_test()) return 7;
    if (!overflow_test()) return 6;
    return 0;
}
