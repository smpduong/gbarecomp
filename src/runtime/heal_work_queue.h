// heal_work_queue.h — ownership-explicit queue for the self-heal worker.
//
// The worker thread processes two kinds of OverlayWorkItem:
//   * real compiles (load_only=false): requested by a dispatch miss; the
//     interpreter bridges until the shard installs. Dropping one only loses
//     this session's healing for that PC (the miss re-requests next run).
//   * preload loads (load_only=true): optional warm-cache background loads.
//     Dropping them is always safe — a later real miss compiles fresh and a
//     purged preload never poisoned s_failed/s_inflight (preload never
//     enters either set).
//
// Shutdown purging (overlay_loader_shutdown) drops ONLY load_only items:
// the observed multi-minute shutdown wedge was the worker sequentially
// dlopen()ing thousands of queued preloads after stop was requested.
// In-flight work always runs to completion under join (ownership-safe: no
// detach, no state destroyed under the worker); queued real compiles are
// few and bounded. See overlay_loader.cpp.

#pragma once

#include <cstddef>
#include <deque>

#include "overlay_compile.h"  // OverlayWorkItem (plain data, header-only deps)

namespace gbarecomp {

class HealWorkQueue {
  public:
    void push(OverlayWorkItem w) { queue_.push_back(std::move(w)); }

    bool empty() const { return queue_.empty(); }
    std::size_t size() const { return queue_.size(); }

    std::size_t load_only_queued() const {
        std::size_t n = 0;
        for (const auto& w : queue_)
            if (w.load_only) ++n;
        return n;
    }

    // Take the front item. Caller must hold the queue mutex and have
    // checked !empty().
    OverlayWorkItem pop_front() {
        OverlayWorkItem w = std::move(queue_.front());
        queue_.pop_front();
        return w;
    }

    // Drop every pending preload (load_only) item, preserving the relative
    // order of the survivors. Returns the number dropped. Real compiles are
    // never touched.
    std::size_t purge_preloads() {
        std::size_t dropped = 0;
        std::deque<OverlayWorkItem> kept;
        for (auto& w : queue_) {
            if (w.load_only) {
                ++dropped;
                continue;
            }
            kept.push_back(std::move(w));
        }
        queue_.swap(kept);
        return dropped;
    }

  private:
    std::deque<OverlayWorkItem> queue_;
};

}  // namespace gbarecomp
