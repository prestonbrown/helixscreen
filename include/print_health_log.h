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
    uint64_t render_ns = 0;
    uint64_t slow_frames = 0;
    uint64_t flushed_px = 0;
    uint64_t screen_px = 0;
    size_t rss_kb = 0;
    size_t peak_rss_kb = 0;
    size_t mem_available_kb = 0;
    size_t swap_free_kb = 0;
};

/// Render time of the frames a display draws, from LV_EVENT_RENDER_START to
/// LV_EVENT_RENDER_READY: drawing and flushing every invalidated area. A refresh with
/// nothing invalid sends neither event, so an idle screen costs no clock read. Pure:
/// times are passed in, in nanoseconds on one monotonic clock.
class FrameCostCounter {
  public:
    /// A frame that renders for longer than the default refresh period makes the next
    /// refresh start late.
    static constexpr uint64_t kSlowRenderNs = uint64_t{LV_DEF_REFR_PERIOD} * 1'000'000ULL;

    void render_start(uint64_t now_ns) {
        start_ns_ = now_ns;
        in_frame_ = true;
    }
    void render_ready(uint64_t now_ns) {
        if (!in_frame_) {
            return;
        }
        in_frame_ = false;
        const uint64_t ns = now_ns > start_ns_ ? now_ns - start_ns_ : 0;
        ++frames_;
        render_ns_ += ns;
        if (ns > kSlowRenderNs) {
            ++slow_frames_;
        }
    }
    uint64_t frames() const {
        return frames_;
    }
    uint64_t render_ns() const {
        return render_ns_;
    }
    uint64_t slow_frames() const {
        return slow_frames_;
    }

  private:
    uint64_t start_ns_ = 0;
    bool in_frame_ = false;
    uint64_t frames_ = 0;
    uint64_t render_ns_ = 0;
    uint64_t slow_frames_ = 0;
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

/// Counts frames, their render time and flushed pixels on @p display. Safe to call again for the
/// same display.
void attach_print_health_counters(lv_display_t* display);

/// A sample of this process: its CPU clocks, the counters attach_print_health_counters()
/// keeps, and memory. Call from the main thread, whose CPU clock it reads.
PrintHealthSample read_print_health_sample(lv_display_t* display);

} // namespace helix
