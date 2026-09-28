// Opt-in, bounded PCM evidence. record() allocates nothing and performs no I/O.
// Caller serializes push/pull records with the bridge mutex. finish() runs only
// after the device has stopped. The WAVs exclude downstream SDL/device effects.
//
// Record kinds:
//   'P' producer push  -- pre-bridge source samples, one record per push.
//   'C' device callback -- post-bridge output samples, one record per pull.
//   'M' action marker  -- no samples; a short label names a host-side action
//                         (save/load boundaries, pause/resume, fast-forward
//                         level edges, rewind trigger). Recorded in the SAME
//                         mutex-serialized steady_clock timeline as P/C, so a
//                         harness can read ring fill and bridge counters AT the
//                         action instead of estimating them from a push index.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace gbarecomp {
class AudioCapture {
    struct Event {
        int64_t ns;
        std::size_t offset, frames;
        uint64_t stretch, underrun, overflow;
        double fill;
        char kind;
        char label[24];  // 'M' only; empty for P/C
    };
    std::string prefix_;
    std::vector<int16_t> source_, output_;
    std::vector<Event> events_;
    std::size_t source_size_ = 0, output_size_ = 0, event_size_ = 0;
    int source_rate_ = 0, host_rate_ = 0;
    bool full_ = false;

    static void le32(FILE* f, uint32_t v) {
        unsigned char bytes[4] = {static_cast<unsigned char>(v),
            static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16),
            static_cast<unsigned char>(v >> 24)};
        std::fwrite(bytes, 1, 4, f);
    }
    static bool wav(const std::string& path, const std::vector<int16_t>& data,
                    std::size_t size, int rate) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fwrite("RIFF", 1, 4, f); le32(f, 36 + static_cast<uint32_t>(size * 2));
        std::fwrite("WAVEfmt ", 1, 8, f); le32(f, 16); le32(f, 0x00010001);
        le32(f, rate); le32(f, rate * 2); le32(f, 0x00100002);
        std::fwrite("data", 1, 4, f); le32(f, static_cast<uint32_t>(size * 2));
        // Explicit little endian WAV even on a big endian host.
        unsigned char chunk[8192];
        for (std::size_t i = 0; i < size;) {
            const auto n = std::min<std::size_t>(4096, size - i);
            for (std::size_t j = 0; j < n; ++j) {
                const uint16_t v = static_cast<uint16_t>(data[i + j]);
                chunk[2*j] = static_cast<unsigned char>(v);
                chunk[2*j+1] = static_cast<unsigned char>(v >> 8);
            }
            std::fwrite(chunk, 2, n, f); i += n;
        }
        const bool ok = !std::ferror(f);
        return std::fclose(f) == 0 && ok;
    }
public:
    // Start/finish occur on the window lifecycle thread, outside the event
    // watch/canary callbacks. This cheap check keeps ordinary uncaptured
    // gameplay from taking the audio mutex for diagnostic phase markers.
    bool active() const noexcept { return !prefix_.empty(); }

    bool start(const char* prefix, int source_rate, int host_rate, int seconds = 240) {
        if (!prefix || !*prefix) return false;
        if (source_rate <= 0 || host_rate <= 0 || source_rate > 768000 ||
            host_rate > 768000 || seconds < 1 || seconds > 240) return false;
        try {
            // A unique prefix prevents an accidental overwrite of old evidence.
            for (const char* suffix : {"-source.wav", "-output.wav", "-events.csv"})
                if (std::filesystem::exists(std::string(prefix) + suffix)) {
                    std::fprintf(stderr, "[audio-capture] prefix already exists; disabled\n");
                    return false;
                }
            source_.resize(static_cast<std::size_t>(source_rate) * seconds);
            output_.resize(static_cast<std::size_t>(host_rate) * seconds);
            events_.resize(static_cast<std::size_t>(seconds) * 1024);
            source_rate_ = source_rate; host_rate_ = host_rate; prefix_ = prefix;
            source_size_ = output_size_ = event_size_ = 0; full_ = false;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[audio-capture] disabled: %s\n", e.what());
            prefix_.clear();
        }
        return !prefix_.empty();
    }
    void record(char kind, const int16_t* data, std::size_t n,
                double fill, uint64_t stretch, uint64_t underrun, uint64_t overflow) {
        if (prefix_.empty() || full_) return;
        auto& buffer = kind == 'P' ? source_ : output_;
        auto& size = kind == 'P' ? source_size_ : output_size_;
        if (n > buffer.size() - size || event_size_ == events_.size()) {
            full_ = true; return;  // retain a complete, replayable prefix
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        events_[event_size_++] = {ns, size, n, stretch, underrun, overflow, fill, kind, {}};
        std::memcpy(buffer.data() + size, data, n * sizeof(int16_t)); size += n;
    }
    // Append a host-side action marker ('M', no samples). Read-only: the only
    // effect is the recorded event. Caller holds the bridge mutex, which is
    // what makes the marker ordering against P/C records trustworthy.
    void record_marker(const char* label, double fill, uint64_t stretch,
                       uint64_t underrun, uint64_t overflow) {
        if (prefix_.empty() || full_) return;
        if (event_size_ == events_.size()) { full_ = true; return; }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        Event& e = events_[event_size_++];
        e.ns = ns; e.offset = 0; e.frames = 0; e.fill = fill;
        e.stretch = stretch; e.underrun = underrun; e.overflow = overflow;
        e.kind = 'M'; e.label[0] = '\0';
        if (label) std::snprintf(e.label, sizeof(e.label), "%s", label);
    }
    bool finish() {
        if (prefix_.empty()) return false;
        bool ok = wav(prefix_ + "-source.wav", source_, source_size_, source_rate_);
        ok = wav(prefix_ + "-output.wav", output_, output_size_, host_rate_) && ok;
        FILE* f = std::fopen((prefix_ + "-events.csv").c_str(), "w");
        if (f) {
            std::fprintf(f, "kind,ns,offset,frames,fill_ms,stretch_frames,underrun_frames,overflow_frames,label\n");
            for (std::size_t i = 0; i < event_size_; ++i) {
                const auto& e = events_[i];
                std::fprintf(f, "%c,%lld,%zu,%zu,%.6f,%llu,%llu,%llu,%s\n", e.kind,
                    static_cast<long long>(e.ns), e.offset, e.frames, e.fill,
                    static_cast<unsigned long long>(e.stretch),
                    static_cast<unsigned long long>(e.underrun),
                    static_cast<unsigned long long>(e.overflow), e.label);
            }
            ok = !std::ferror(f) && ok;
            ok = (std::fclose(f) == 0) && ok;
        } else ok = false;
        std::fprintf(stderr, "[audio-capture] prefix=%s ok=%d truncated=%d source_frames=%zu output_frames=%zu events=%zu\n",
            prefix_.c_str(), ok, full_, source_size_, output_size_, event_size_);
        prefix_.clear();
        return ok && !full_;
    }
};
} // namespace gbarecomp
