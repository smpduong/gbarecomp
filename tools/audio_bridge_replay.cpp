// Replay ordered AudioCapture events without a device or ROM.
// Usage: audio_bridge_replay CAPTURE_PREFIX OUTPUT_PREFIX [--target-ms N] [--require-exact]
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
#define RECOMP_AUDIO_DRC_IMPL
#ifdef RAB_REPLAY_HEADER
#include RAB_REPLAY_HEADER
#else
#include "recomp_audio_drc.h"
#endif
#include "audio_capture.h"

struct Wav { int rate; std::vector<int16_t> samples; };
static Wav read_wav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    unsigned char h[44]{};
    if (!f.read(reinterpret_cast<char*>(h), 44)) throw std::runtime_error("missing WAV header");
    auto u32 = [&](int i) { return uint32_t(h[i]) | uint32_t(h[i+1])<<8 |
                                  uint32_t(h[i+2])<<16 | uint32_t(h[i+3])<<24; };
    // Deliberately accept only the fixed 44-byte mono PCM format we record.
    if (std::memcmp(h, "RIFF", 4) || std::memcmp(h+8, "WAVEfmt ", 8) ||
        std::memcmp(h+36, "data", 4) || u32(16) != 16 || u32(20) != 0x10001 ||
        u32(32) != 0x100002 || !u32(24) || u32(24) > 768000 ||
        u32(40) % 2 || u32(40) > 128*1024*1024)
        throw std::runtime_error("unsupported capture WAV");
    Wav w{static_cast<int>(u32(24)), std::vector<int16_t>(u32(40)/2)};
    for (auto& s : w.samples) {
        unsigned char bytes[2];
        if (!f.read(reinterpret_cast<char*>(bytes), 2)) throw std::runtime_error("short WAV");
        s = static_cast<int16_t>(uint16_t(bytes[0]) | uint16_t(bytes[1])<<8);
    }
    return w;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: audio_bridge_replay capture-prefix output-prefix [--target-ms N] [--require-exact]\n";
        return 2;
    }
    try {
        double target_ms = 25.;
        bool require_exact = false;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--require-exact") require_exact = true;
            else if (std::string(argv[i]) == "--target-ms" && i+1 < argc)
                target_ms = std::stod(argv[++i]);
            else throw std::runtime_error("unknown/missing option");
        }
        const std::string prefix = argv[1];
        auto source = read_wav(prefix + "-source.wav");
        auto expected = read_wav(prefix + "-output.wav");
        rab_config c; rab_config_defaults(&c);
        c.channels = 1; c.source_rate = source.rate; c.host_rate = expected.rate;
        c.target_ms = target_ms; c.preroll_ms = 0.;
        rab_bridge b{};
        if (rab_init(&b, &c)) throw std::runtime_error("bridge initialization failed");
        gbarecomp::AudioCapture capture;
        if (!capture.start(argv[2], source.rate, expected.rate))
            throw std::runtime_error("cannot start capture (use a fresh output prefix)");
        std::ifstream events(prefix + "-events.csv");
        std::string line; std::getline(events, line);
        // Accept both the original 8-column header and the current one with
        // the trailing `label` column ('M' marker records only).
        const std::string header_base =
            "kind,ns,offset,frames,fill_ms,stretch_frames,underrun_frames,overflow_frames";
        if (line != header_base && line != header_base + ",label")
            throw std::runtime_error("invalid event header");
        uint64_t mismatches = 0, pushes = 0, pulls = 0;
        std::vector<int16_t> out;
        while (std::getline(events, line)) {
            std::replace(line.begin(), line.end(), ',', ' ');
            std::istringstream row(line);
            char kind; int64_t ns; std::size_t offset, frames;
            double fill; uint64_t stretch, under, over;
            if (!(row >> kind >> ns >> offset >> frames >> fill >> stretch >> under >> over))
                throw std::runtime_error("invalid capture event");
            std::string label;
            std::getline(row, label);  // optional trailing label column
            while (!label.empty() && label.front() == ' ') label.erase(label.begin());
            if (kind == 'M') {
                // Action marker: no samples to replay, just re-record it.
                capture.record_marker(label.c_str(), fill, stretch, under, over);
                continue;
            }
            if (!frames || frames > 65536) throw std::runtime_error("invalid capture event");
            const int16_t* data = nullptr;
            if (kind == 'P') {
                if (offset != pushes || offset > source.samples.size() ||
                    frames > source.samples.size()-offset) throw std::runtime_error("source offset mismatch");
                data = source.samples.data()+offset;
                rab_push(&b, data, static_cast<int>(frames)); pushes += frames;
            } else if (kind == 'C') {
                if (offset != pulls || offset > expected.samples.size() ||
                    frames > expected.samples.size()-offset) throw std::runtime_error("output offset mismatch");
                out.resize(frames);
                rab_pull(&b, out.data(), static_cast<int>(frames)); pulls += frames;
                for (std::size_t i = 0; i < frames; ++i)
                    mismatches += out[i] != expected.samples[offset+i];
                data = out.data();
            } else throw std::runtime_error("unknown event kind");
            capture.record(kind, data, frames, rab_fill_ms(&b), b.stats.stretch_frames,
                           b.stats.underrun_events, b.stats.overflow_drops);
        }
        if (!capture.finish()) throw std::runtime_error("capture write failed or storage limit exceeded");
        if (pushes != source.samples.size() || pulls != expected.samples.size())
            throw std::runtime_error("incomplete event coverage");
        std::printf("{\"source_frames\":%llu,\"output_frames\":%llu,\"mismatched_samples\":%llu,"
                    "\"stretch_events\":%llu,\"stretch_frames\":%llu,\"underrun_frames\":%llu,\"overflow_frames\":%llu}\n",
            (unsigned long long)pushes, (unsigned long long)pulls, (unsigned long long)mismatches,
            (unsigned long long)b.stats.stretch_events, (unsigned long long)b.stats.stretch_frames,
            (unsigned long long)b.stats.underrun_events, (unsigned long long)b.stats.overflow_drops);
        rab_free(&b);
        return require_exact && mismatches ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
