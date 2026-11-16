// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>

namespace helix::ui {

/// CPU time the whole process has used and the steady clock, read together.
struct CpuSample {
    uint64_t cpu_ns = 0;
    uint64_t wall_ns = 0;
};

/// Source of CpuSamples; tests pass a scripted one.
using CpuClockFn = std::function<CpuSample()>;

/// Source of the steady clock alone, in nanoseconds. Cheap where the CPU clock is a
/// system call, so a caller sampling on an interval checks this one first.
using WallClockFn = std::function<uint64_t()>;

/// Reads CLOCK_PROCESS_CPUTIME_ID, which counts every thread of the process, LVGL's draw
/// thread included, and the steady clock.
CpuSample read_process_cpu_clock();

/// The steady clock in nanoseconds: CpuSample::wall_ns without the CPU read.
uint64_t read_steady_clock_ns();

} // namespace helix::ui
