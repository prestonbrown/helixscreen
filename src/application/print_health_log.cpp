// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_health_log.h"

#include "helix_thread.h"
#include "memory_monitor.h"
#include "memory_utils.h"
#include "screensaver_cpu_clock.h"

#include <spdlog/fmt/fmt.h>

namespace helix {

namespace {

// Display events and the sampler both run on the main thread.
uint64_t s_frames = 0;
uint64_t s_flushed_px = 0;
lv_display_t* s_attached_display = nullptr;

// The refresh timer ticks whether or not anything changed; a render means a drawn frame.
void on_render_start(lv_event_t*) {
    ++s_frames;
}

void on_flush_start(lv_event_t* e) {
    const auto* area = static_cast<const lv_area_t*>(lv_event_get_param(e));
    if (area) {
        s_flushed_px += static_cast<uint64_t>(lv_area_get_size(area));
    }
}

uint64_t delta(uint64_t earlier, uint64_t later) {
    return later > earlier ? later - earlier : 0;
}

} // namespace

std::string format_print_health(const PrintHealthSample& earlier, const PrintHealthSample& later) {
    const double seconds = static_cast<double>(delta(earlier.wall_ns, later.wall_ns)) / 1e9;
    const auto per_second = [seconds](double count) { return seconds > 0 ? count / seconds : 0.0; };
    const auto cpu_percent = [&per_second](uint64_t ns) {
        return per_second(static_cast<double>(ns) / 1e9) * 100.0;
    };
    const double screens = later.screen_px > 0
                               ? static_cast<double>(delta(earlier.flushed_px, later.flushed_px)) /
                                     static_cast<double>(later.screen_px)
                               : 0.0;
    return fmt::format("cpu={:.1f}% main={:.1f}% frames={:.1f}/s flushed={:.2f} screens/s rss={}MB "
                       "mem_avail={}MB swap_free={}MB",
                       cpu_percent(delta(earlier.process_cpu_ns, later.process_cpu_ns)),
                       cpu_percent(delta(earlier.main_thread_cpu_ns, later.main_thread_cpu_ns)),
                       per_second(static_cast<double>(delta(earlier.frames, later.frames))),
                       per_second(screens), later.rss_kb / 1024, later.mem_available_kb / 1024,
                       later.swap_free_kb / 1024);
}

std::optional<std::string> PrintHealthLog::tick(uint32_t now_ms, bool print_active) {
    if (!print_active) {
        baseline_.reset();
        return std::nullopt;
    }
    if (!baseline_) {
        baseline_ = sample_();
        baseline_ms_ = now_ms;
        return std::nullopt;
    }
    if (now_ms - baseline_ms_ < kIntervalMs) {
        return std::nullopt;
    }
    PrintHealthSample current = sample_();
    std::string line = format_print_health(*baseline_, current);
    baseline_ = current;
    baseline_ms_ = now_ms;
    return line;
}

void attach_print_health_counters(lv_display_t* display) {
    if (!display || display == s_attached_display) {
        return;
    }
    lv_display_add_event_cb(display, on_render_start, LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(display, on_flush_start, LV_EVENT_FLUSH_START, nullptr);
    s_attached_display = display;
}

PrintHealthSample read_print_health_sample(lv_display_t* display) {
    PrintHealthSample sample;
    const ui::CpuSample cpu = ui::read_process_cpu_clock();
    sample.process_cpu_ns = cpu.cpu_ns;
    sample.wall_ns = cpu.wall_ns;
    sample.main_thread_cpu_ns = static_cast<uint64_t>(thread_cpu_ns());
    sample.frames = s_frames;
    sample.flushed_px = s_flushed_px;
    if (display) {
        sample.screen_px = static_cast<uint64_t>(lv_display_get_horizontal_resolution(display)) *
                           static_cast<uint64_t>(lv_display_get_vertical_resolution(display));
    }
    sample.rss_kb = MemoryMonitor::get_current_stats().vm_rss_kb;
    const MemoryInfo memory = get_system_memory_info();
    sample.mem_available_kb = memory.available_kb;
    sample.swap_free_kb = memory.swap_free_kb;
    return sample;
}

} // namespace helix
