// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_cpu_clock.h"

#include <chrono>
#include <ctime>

namespace helix::ui {

CpuSample read_process_cpu_clock() {
    CpuSample sample;
    timespec cpu{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) == 0) {
        sample.cpu_ns =
            static_cast<uint64_t>(cpu.tv_sec) * 1000000000ULL + static_cast<uint64_t>(cpu.tv_nsec);
    }
    sample.wall_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
    return sample;
}

} // namespace helix::ui
