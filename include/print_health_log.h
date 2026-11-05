// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace helix {

/// Cumulative counters and current memory readings at one moment.
struct PrintHealthSample {
    uint64_t process_cpu_ns = 0;
    uint64_t main_thread_cpu_ns = 0;
    uint64_t wall_ns = 0;
    uint64_t frames = 0;
    uint64_t flushed_px = 0;
    uint64_t screen_px = 0;
    size_t rss_kb = 0;
    size_t mem_available_kb = 0;
    size_t swap_free_kb = 0;
};

/// One log line for the interval between two samples. CPU is percent of one core, so a
/// value above 100 means several threads were busy at once. Pure.
std::string format_print_health(const PrintHealthSample& earlier, const PrintHealthSample& later);

/// Decides when a print health line is due: one per interval while a print runs, measured
/// from a baseline taken when the print is first seen, so each line covers only print time.
class PrintHealthLog {
  public:
    static constexpr uint32_t kIntervalMs = 60000;

    using SampleFn = std::function<PrintHealthSample()>;

    explicit PrintHealthLog(SampleFn sample) : sample_(std::move(sample)) {}

    /// The line to log now, if one is due. Call from the main loop; it samples only when
    /// the interval has passed or a print has just started.
    std::optional<std::string> tick(uint32_t now_ms, bool print_active);

  private:
    SampleFn sample_;
    std::optional<PrintHealthSample> baseline_;
    uint32_t baseline_ms_ = 0;
};

/// Counts frames and flushed pixels on @p display. Safe to call again for the same display.
void attach_print_health_counters(lv_display_t* display);

/// A sample of this process: its CPU clocks, the counters attach_print_health_counters()
/// keeps, and memory. Call from the main thread, whose CPU clock it reads.
PrintHealthSample read_print_health_sample(lv_display_t* display);

} // namespace helix
