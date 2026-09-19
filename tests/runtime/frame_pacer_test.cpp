#include "host_platform.h"
#include <cstdio>

int main() {
    using namespace std::chrono;
    using gbarecomp::FramePacer;
    const nanoseconds period(16742706);
    steady_clock::time_point deadline(period), legacy = deadline;
    for (int i = 0; i < 6000; ++i) {
        const auto delay = i % 10 == 0 ? milliseconds(2) : milliseconds(0);
        deadline = FramePacer::advance_deadline(deadline, deadline + delay, period);
        legacy = delay > microseconds(1500) ? legacy + delay + period : legacy + period;
    }
    if (deadline.time_since_epoch() != 6001 * period) return 1;
    auto woke = deadline + seconds(2);
    if (FramePacer::advance_deadline(deadline, woke, period) != woke + period) return 2;
    std::printf("PASS short-wake jitter: drift=0ms (old rule=%lldms); long stall resync\n",
                (long long)duration_cast<milliseconds>(legacy-deadline).count());
}
