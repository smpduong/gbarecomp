// Unit test for HealWorkQueue (heal_work_queue.h): the ownership rules the
// self-heal worker shutdown depends on.
//
//   * purge_preloads() drops ONLY load_only items, preserves survivor order,
//     and reports the dropped count.
//   * pop_front/empty/size behave as a FIFO.
//   * A queue holding only preloads purges to empty (the shutdown wedge
//     case: thousands of queued preload dlopens).
//   * A queue holding only real compiles is untouched by purge (in-flight +
//     queued real work still runs to completion under join).
//   * Mixed interleavings keep every real compile in order.
#include <cstdint>
#include <cstdio>

#include "heal_work_queue.h"

namespace {

int failures = 0;
void check(const char* label, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }
}

gbarecomp::OverlayWorkItem item(uint32_t pc, bool load_only) {
    gbarecomp::OverlayWorkItem w;
    w.pc = pc;
    w.thumb = true;
    w.load_only = load_only;
    return w;
}

void test_empty_queue() {
    gbarecomp::HealWorkQueue q;
    check("new queue empty", q.empty());
    check("new queue size 0", q.size() == 0);
    // Stop with an empty queue: purge is a no-op (shutdown fast-path).
    check("purge of empty drops 0", q.purge_preloads() == 0);
    check("still empty", q.empty());
}

void test_preload_only_purge() {
    gbarecomp::HealWorkQueue q;
    for (uint32_t i = 0; i < 1000; ++i) q.push(item(0x08000000u + i * 4u, true));
    check("1000 queued", q.size() == 1000);
    check("1000 load_only", q.load_only_queued() == 1000);
    check("purge drops 1000", q.purge_preloads() == 1000);
    check("empty after purge", q.empty());
}

void test_real_only_untouched() {
    gbarecomp::HealWorkQueue q;
    q.push(item(0x08000100u, false));
    q.push(item(0x08000200u, false));
    check("purge drops 0", q.purge_preloads() == 0);
    check("size still 2", q.size() == 2);
    check("FIFO order kept", q.pop_front().pc == 0x08000100u);
    check("FIFO order kept (2)", q.pop_front().pc == 0x08000200u);
    check("empty after pops", q.empty());
}

void test_mixed_interleaving() {
    gbarecomp::HealWorkQueue q;
    q.push(item(0x08000100u, true));
    q.push(item(0x08000200u, false));
    q.push(item(0x08000300u, true));
    q.push(item(0x08000400u, true));
    q.push(item(0x08000500u, false));
    q.push(item(0x08000600u, true));
    check("purge drops 4", q.purge_preloads() == 4);
    check("2 survivors", q.size() == 2);
    check("survivor order (real 1)", q.pop_front().pc == 0x08000200u);
    check("survivor order (real 2)", q.pop_front().pc == 0x08000500u);
    check("empty after pops", q.empty());
}

void test_inflight_item_not_in_queue() {
    // The worker pops BEFORE processing, so an in-flight item is never in
    // the queue: purge cannot strand in-flight work. Model: pop one real
    // item (now "in flight"), purge, and confirm only queued preloads drop.
    gbarecomp::HealWorkQueue q;
    q.push(item(0x08000100u, true));
    q.push(item(0x08000200u, false));
    q.push(item(0x08000300u, true));
    const auto inflight = q.pop_front();  // a preload goes in flight
    (void)inflight;
    const auto real = q.pop_front();  // a real compile goes in flight
    check("in-flight real pc", real.pc == 0x08000200u);
    check("purge drops last preload", q.purge_preloads() == 1);
    check("queue empty", q.empty());
}

}  // namespace

int main() {
    test_empty_queue();
    test_preload_only_purge();
    test_real_only_untouched();
    test_mixed_interleaving();
    test_inflight_item_not_in_queue();
    std::printf("heal_work_queue_tests: %s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
