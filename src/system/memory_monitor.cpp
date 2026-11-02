// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "memory_monitor.h"

#include "ui_update_queue.h"

#include "exception_policy.h"
#include "helix_thread.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef __linux__
#include <lvgl.h>
#endif

#ifdef __linux__
#include <string>
#endif

namespace helix {

static constexpr auto kRateLimitInterval = std::chrono::minutes(5);

MemoryThresholds MemoryThresholds::for_device(const MemoryInfo& info) {
    MemoryThresholds t;

    // RSS thresholds are a SHARE OF TOTAL RAM with a per-band floor, in every
    // band. A flat number cannot serve a band 2x wide: the constrained band's
    // 30MB was chosen for the AD5M (~110MB, so 27% of RAM), but the same band
    // holds the 209MB Creality K1, where 30MB is 14% — and the app's own steady
    // state there is 27-30MB. That put the trigger AT steady state and the 90%
    // clear threshold BELOW it, so the warning latched on and could never clear:
    // a memory warning every 5 minutes forever, on a box with 130MB free, each
    // one dispatching the pressure responders for a measured +0kB.
    //
    // std::max means this only ever RAISES a threshold relative to the old flat
    // values, so it cannot introduce a warning on any device that was quiet.
    if (info.is_constrained_device()) {
        // <256MB (AD5M ~110MB, K1/K1C ~209-214MB): tight budgets
        t.warn_rss_kb = std::max<size_t>(30 * 1024, info.total_kb * 30 / 100);
        t.critical_rss_kb = std::max<size_t>(40 * 1024, info.total_kb * 50 / 100);
        t.warn_available_kb = 15 * 1024;
        t.critical_available_kb = 8 * 1024;
        t.growth_5min_kb = 1 * 1024;
    } else if (info.is_normal_device()) {
        // 256-448MB. Warn at 40%: a ~415MB Pi driving a printer holds ~130MB
        // RSS mid-print with ~165MB still free, which 30% (124MB) called a
        // warning. 40% of the band's top meets the next band's 180MB floor.
        t.warn_rss_kb = std::max<size_t>(120 * 1024, info.total_kb * 40 / 100);
        t.critical_rss_kb = std::max<size_t>(180 * 1024, info.total_kb * 50 / 100);
        t.warn_available_kb = 32 * 1024;
        t.critical_available_kb = 16 * 1024;
        t.growth_5min_kb = 3 * 1024;
    } else {
        // >448MB (Pi 4/5, desktop). On a 4GB Pi we are usually the largest
        // process and can legitimately use most of it for the 3D viewer; on a
        // desktop we want even more headroom.
        t.warn_rss_kb = std::max<size_t>(180 * 1024, info.total_kb * 30 / 100);
        t.critical_rss_kb = std::max<size_t>(230 * 1024, info.total_kb * 50 / 100);
        t.warn_available_kb = 48 * 1024;
        t.critical_available_kb = 24 * 1024;
        t.growth_5min_kb = 5 * 1024;
    }

    // Hysteresis: clear thresholds at 90% of trigger for RSS,
    // 110% of trigger for available (available must rise above clear to dismiss)
    t.clear_warn_rss_kb = t.warn_rss_kb * 90 / 100;
    t.clear_critical_rss_kb = t.critical_rss_kb * 90 / 100;
    t.clear_warn_available_kb = t.warn_available_kb * 110 / 100;
    t.clear_critical_available_kb = t.critical_available_kb * 110 / 100;

    return t;
}

const char* pressure_level_to_string(MemoryPressureLevel level) {
    switch (level) {
    case MemoryPressureLevel::none:
        return "none";
    case MemoryPressureLevel::elevated:
        return "elevated";
    case MemoryPressureLevel::warning:
        return "warning";
    case MemoryPressureLevel::critical:
        return "critical";
    }
    return "unknown";
}

MemoryMonitor& MemoryMonitor::instance() {
    static MemoryMonitor instance;
    return instance;
}

MemoryMonitor::~MemoryMonitor() {
    stop();
}

void MemoryMonitor::start(int interval_ms) {
    if (running_.load()) {
        return;
    }

    interval_ms_.store(interval_ms);
    running_.store(true);

    // Compute thresholds based on device tier
    auto sys_info = get_system_memory_info();
    thresholds_ = MemoryThresholds::for_device(sys_info);

    const char* tier = sys_info.is_constrained_device() ? "constrained"
                       : sys_info.is_normal_device()    ? "normal"
                                                        : "good";
    spdlog::info("[MemoryMonitor] Thresholds: warn_rss={}MB critical_rss={}MB "
                 "warn_avail={}MB critical_avail={}MB growth_5min={}MB (device tier: {}, "
                 "total={}MB)",
                 thresholds_.warn_rss_kb / 1024, thresholds_.critical_rss_kb / 1024,
                 thresholds_.warn_available_kb / 1024, thresholds_.critical_available_kb / 1024,
                 thresholds_.growth_5min_kb / 1024, tier, sys_info.total_mb());

    // EAGAIN under thread exhaustion throws std::system_error ([L083]).
    if (!helix::contain_exceptions("[MemoryMonitor] Spawning the monitor thread", [&] {
            monitor_thread_ = helix::make_thread([this]() { monitor_loop(); });
        })) {
        running_.store(false);
        return;
    }
    spdlog::debug("[MemoryMonitor] Started (interval={}ms)", interval_ms);
}

void MemoryMonitor::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }

    spdlog::debug("[MemoryMonitor] Stopped");
}

void MemoryMonitor::set_warning_callback(WarningCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    warning_callback_ = std::move(cb);
}

void MemoryMonitor::set_hang_callback(HangCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    hang_callback_ = std::move(cb);
}

void MemoryMonitor::set_hang_threshold_ms(uint32_t ms) {
    hang_threshold_ms_.store(ms, std::memory_order_relaxed);
    spdlog::debug("[MemoryMonitor] Main-loop hang threshold set to {}ms{}", ms,
                  ms == 0 ? " (detection disabled)" : "");
}

uint32_t MemoryMonitor::hang_threshold_ms() const {
    return hang_threshold_ms_.load(std::memory_order_relaxed);
}

void MemoryMonitor::check_main_loop_liveness() {
    // Unarmed means the main loop has not run a single iteration yet, so there
    // is no baseline to call stale. Startup is slow enough on the weaker boards
    // (~8s of XML parsing on AD5M) that measuring before then would only ever
    // produce false positives.
    if (!MainLoopHeartbeat::armed()) {
        return;
    }

    hang_detector_.set_threshold_ms(hang_threshold_ms_.load(std::memory_order_relaxed));

    const auto now_ms =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());

    const uint32_t stalled_ms = hang_detector_.sample(MainLoopHeartbeat::count(), now_ms);
    if (stalled_ms == 0) {
        return;
    }

    // Deliberately not spdlog::debug: by definition nobody is watching a screen
    // that has stopped responding, so this has to survive at the default level
    // to be in the log the user eventually ships us.
    spdlog::error("[MemoryMonitor] UI main loop has not ticked in {}ms — the touchscreen is "
                  "unresponsive (heartbeat stuck at {})",
                  stalled_ms, MainLoopHeartbeat::count());

    HangCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = hang_callback_;
    }
    // Invoked outside the lock: the callback reports telemetry and may one day
    // abort the process, and neither wants this mutex held.
    if (cb) {
        cb(stalled_ms);
    }
}

MemoryMonitor::PressureResponderId
MemoryMonitor::add_pressure_responder(std::function<void(MemoryPressureLevel)> cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    auto id = next_responder_id_.fetch_add(1);
    pressure_responders_.emplace_back(id, std::move(cb));
    spdlog::debug("[MemoryMonitor] Registered pressure responder (id={})", id);
    return id;
}

void MemoryMonitor::remove_pressure_responder(PressureResponderId id) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    auto it = std::remove_if(pressure_responders_.begin(), pressure_responders_.end(),
                             [id](const auto& pair) { return pair.first == id; });
    if (it != pressure_responders_.end()) {
        pressure_responders_.erase(it, pressure_responders_.end());
        spdlog::debug("[MemoryMonitor] Removed pressure responder (id={})", id);
    }
}

MemoryStats MemoryMonitor::get_current_stats() {
    MemoryStats stats;

#ifdef __linux__
    helix::text_io::LineReader status("/proc/self/status");
    if (!status) {
        return stats;
    }

    std::string line;
    while (status.next(line)) {
        if (line.compare(0, 7, "VmSize:") == 0) {
            sscanf(line.c_str(), "VmSize: %zu", &stats.vm_size_kb);
        } else if (line.compare(0, 6, "VmRSS:") == 0) {
            sscanf(line.c_str(), "VmRSS: %zu", &stats.vm_rss_kb);
        } else if (line.compare(0, 7, "VmData:") == 0) {
            sscanf(line.c_str(), "VmData: %zu", &stats.vm_data_kb);
        } else if (line.compare(0, 7, "VmSwap:") == 0) {
            sscanf(line.c_str(), "VmSwap: %zu", &stats.vm_swap_kb);
        } else if (line.compare(0, 7, "VmPeak:") == 0) {
            sscanf(line.c_str(), "VmPeak: %zu", &stats.vm_peak_kb);
        } else if (line.compare(0, 6, "VmHWM:") == 0) {
            sscanf(line.c_str(), "VmHWM: %zu", &stats.vm_hwm_kb);
        }
    }
#endif

    return stats;
}

void MemoryMonitor::log_now(const char* context, spdlog::level::level_enum level) {
    MemoryStats stats = get_current_stats();

    if (context) {
        spdlog::log(level,
                    "[MemoryMonitor] [{}] RSS={}kB VmSize={}kB VmData={}kB Swap={}kB (Peak: "
                    "RSS={}kB Vm={}kB)",
                    context, stats.vm_rss_kb, stats.vm_size_kb, stats.vm_data_kb, stats.vm_swap_kb,
                    stats.vm_hwm_kb, stats.vm_peak_kb);
    } else {
        spdlog::log(level,
                    "[MemoryMonitor] RSS={}kB VmSize={}kB VmData={}kB Swap={}kB (Peak: RSS={}kB "
                    "Vm={}kB)",
                    stats.vm_rss_kb, stats.vm_size_kb, stats.vm_data_kb, stats.vm_swap_kb,
                    stats.vm_hwm_kb, stats.vm_peak_kb);
    }
}

MemoryPressureAssessment assess_memory_pressure(const MemoryStats& stats,
                                                const MemoryThresholds& thresholds,
                                                MemoryPressureLevel current_level,
                                                const MemoryInfo& sys_info, int64_t growth_kb,
                                                uint32_t sustained_warning_secs) {
    using Level = MemoryPressureLevel;
    using Cause = MemoryPressureCause;

    // RSS thresholds — check escalations first, then hysteresis holds
    MemoryPressureAssessment rss;
    if (stats.vm_rss_kb >= thresholds.critical_rss_kb) {
        rss = {Level::critical, Cause::rss_critical};
    } else if (current_level >= Level::critical &&
               stats.vm_rss_kb >= thresholds.clear_critical_rss_kb) {
        rss = {Level::critical, Cause::hysteresis};
    } else if (stats.vm_rss_kb >= thresholds.warn_rss_kb) {
        rss = {Level::warning, Cause::rss_warning};
    } else if (current_level >= Level::warning && stats.vm_rss_kb >= thresholds.clear_warn_rss_kb) {
        rss = {Level::warning, Cause::hysteresis};
    }

    // System available memory. Lower available_kb = worse, so clear thresholds
    // are ABOVE trigger thresholds.
    MemoryPressureAssessment avail;
    if (sys_info.available_kb > 0) {
        if (sys_info.available_kb <= thresholds.critical_available_kb) {
            avail = {Level::critical, Cause::available_critical};
        } else if (current_level >= Level::critical &&
                   sys_info.available_kb <= thresholds.clear_critical_available_kb) {
            avail = {Level::critical, Cause::hysteresis};
        } else if (sys_info.available_kb <= thresholds.warn_available_kb) {
            avail = {Level::warning, Cause::available_warning};
        } else if (current_level >= Level::warning &&
                   sys_info.available_kb <= thresholds.clear_warn_available_kb) {
            avail = {Level::warning, Cause::hysteresis};
        }
    }

    // The worse band wins; on a tie the RSS band names the cause.
    MemoryPressureAssessment result = avail.level > rss.level ? avail : rss;

    // Growth rate (may trigger elevated) — but ONLY when the device is already
    // approaching its limits. A fast one-time climb is benign when there is
    // ample headroom and must not warn: the 3D G-code viewer loading at print
    // start adds ~13MB over 5 minutes, which on a 512MB-class AD5X (RSS ~48MB,
    // hundreds of MB free) is well under every absolute band.
    //
    // Genuinely tight devices (AD5M 128MB, RSS baseline ~20MB vs warn_rss 30MB)
    // sit above warn_rss/2 essentially all the time, so growth still escalates
    // there — exactly where rapid growth actually matters.
    const bool near_rss_limit = stats.vm_rss_kb >= thresholds.warn_rss_kb / 2;
    const bool low_available =
        sys_info.available_kb > 0 && sys_info.available_kb <= thresholds.warn_available_kb * 2;
    if ((near_rss_limit || low_available) &&
        growth_kb > static_cast<int64_t>(thresholds.growth_5min_kb) &&
        result.level < Level::elevated) {
        result = {Level::elevated, Cause::growth};
    }

    // Free memory that sits in the warning band and will not clear is the
    // steady state, not a spike. Escalate it so the responders gated at
    // critical are reachable on a device whose idle free memory never rises
    // out of the warning band. RSS alone never escalates: a large footprint
    // with ample free memory is not pressure, however long it lasts.
    if (result.level == Level::warning && avail.level == Level::warning &&
        sustained_warning_secs >= thresholds.sustained_warning_secs_to_critical) {
        result = {Level::critical, Cause::sustained_low_available};
    }

    return result;
}

MemoryPressureLevel compute_pressure_level(const MemoryStats& stats,
                                           const MemoryThresholds& thresholds,
                                           MemoryPressureLevel current_level,
                                           const MemoryInfo& sys_info, int64_t growth_kb,
                                           uint32_t sustained_warning_secs) {
    return assess_memory_pressure(stats, thresholds, current_level, sys_info, growth_kb,
                                  sustained_warning_secs)
        .level;
}

std::string describe_memory_pressure(const MemoryPressureAssessment& assessment,
                                     const MemoryStats& stats, const MemoryThresholds& thresholds,
                                     const MemoryInfo& sys_info, int64_t growth_kb) {
    switch (assessment.cause) {
    case MemoryPressureCause::rss_critical:
        return fmt::format("RSS {}MB exceeds critical threshold {}MB", stats.vm_rss_kb / 1024,
                           thresholds.critical_rss_kb / 1024);
    case MemoryPressureCause::rss_warning:
        return fmt::format("RSS {}MB exceeds warning threshold {}MB", stats.vm_rss_kb / 1024,
                           thresholds.warn_rss_kb / 1024);
    case MemoryPressureCause::available_critical:
        return fmt::format("System available {}MB below critical threshold {}MB",
                           sys_info.available_mb(), thresholds.critical_available_kb / 1024);
    case MemoryPressureCause::available_warning:
        return fmt::format("System available {}MB below warning threshold {}MB",
                           sys_info.available_mb(), thresholds.warn_available_kb / 1024);
    case MemoryPressureCause::growth:
        return fmt::format("RSS growth {:+}KB over 5 minutes exceeds threshold {}KB", growth_kb,
                           thresholds.growth_5min_kb);
    case MemoryPressureCause::sustained_low_available:
        return fmt::format("System available {}MB held below warning threshold {}MB",
                           sys_info.available_mb(), thresholds.warn_available_kb / 1024);
    case MemoryPressureCause::hysteresis:
        return fmt::format("Pressure level {} held by hysteresis (RSS {}MB, available {}MB)",
                           pressure_level_to_string(assessment.level), stats.vm_rss_kb / 1024,
                           sys_info.available_mb());
    case MemoryPressureCause::none:
        break;
    }
    return "No memory pressure";
}

bool should_report_memory_pressure(MemoryReportState& state, MemoryPressureLevel level,
                                   size_t rss_kb, size_t growth_step_kb, bool interval_elapsed) {
    if (level == MemoryPressureLevel::none || level < state.level) {
        state.level = level;
        state.rss_kb = rss_kb;
        return false;
    }
    const bool rose = level > state.level;
    const bool grew = growth_step_kb > 0 && rss_kb >= state.rss_kb + growth_step_kb;
    if (!(rose || grew) || !interval_elapsed) {
        return false;
    }
    state.level = level;
    state.rss_kb = rss_kb;
    return true;
}

void MemoryMonitor::evaluate_thresholds(const MemoryStats& stats) {
    // No meaningful stats on non-Linux — skip evaluation
    if (stats.vm_rss_kb == 0 && stats.vm_size_kb == 0) {
        return;
    }

    auto sys_info = get_system_memory_info();

    // Compute smoothed growth from circular buffer
    int64_t growth_kb = 0;
    if (rss_history_count_ >= kRssHistorySize) {
        // Average of 3 oldest samples
        size_t avg_oldest = 0;
        for (size_t i = 0; i < 3; i++) {
            avg_oldest += rss_history_[(rss_history_index_ + i) % kRssHistorySize];
        }
        avg_oldest /= 3;

        // Average of 3 newest samples (the 3 before rss_history_index_)
        size_t avg_newest = 0;
        for (size_t i = 1; i <= 3; i++) {
            avg_newest +=
                rss_history_[(rss_history_index_ + kRssHistorySize - i) % kRssHistorySize];
        }
        avg_newest /= 3;

        growth_kb = static_cast<int64_t>(avg_newest) - static_cast<int64_t>(avg_oldest);
    }

    auto current = pressure_level_.load();

    const auto now_tp = std::chrono::steady_clock::now();
    if (current < MemoryPressureLevel::warning) {
        warning_since_ = now_tp;
    }
    const auto sustained =
        std::chrono::duration_cast<std::chrono::seconds>(now_tp - warning_since_);

    const MemoryPressureAssessment assessment = assess_memory_pressure(
        stats, thresholds_, current, sys_info, growth_kb, static_cast<uint32_t>(sustained.count()));
    const MemoryPressureLevel level = assessment.level;

    pressure_level_.store(level);

    const auto now = std::chrono::steady_clock::now();
    const int level_idx = static_cast<int>(level);

    // Reports go out on a change worth knowing about, never on a timer while
    // nothing moves: a level held for hours is one event, not one per tick.
    const bool report = should_report_memory_pressure(
        report_state_, level, stats.vm_rss_kb, thresholds_.growth_5min_kb,
        now - last_report_time_[level_idx] >= kRateLimitInterval);
    if (report) {
        last_report_time_[level_idx] = now;
    }

    // Responders re-run on a cadence while pressure holds: the caches they
    // drop refill.
    bool dispatch = false;
    if (level != MemoryPressureLevel::none &&
        now - last_warning_time_[level_idx] >= kRateLimitInterval) {
        last_warning_time_[level_idx] = now;
        dispatch = true;
    }

    if (!report && !dispatch) {
        return;
    }

    const std::string reason =
        describe_memory_pressure(assessment, stats, thresholds_, sys_info, growth_kb);

    // Always include our own memory footprint so logs can distinguish
    // "we are the cause" from "we are a victim of system pressure".
    const std::string our_footprint =
        fmt::format("our RSS={}MB VmSize={}MB VmSwap={}MB growth_5min={:+}KB sys_avail={}MB",
                    stats.vm_rss_kb / 1024, stats.vm_size_kb / 1024, stats.vm_swap_kb / 1024,
                    growth_kb, sys_info.available_mb());

    if (report) {
        switch (level) {
        case MemoryPressureLevel::elevated:
            spdlog::info("[MemoryMonitor] ELEVATED: {} ({})", reason, our_footprint);
            break;
        case MemoryPressureLevel::warning:
            spdlog::warn("[MemoryMonitor] WARNING: {} ({})", reason, our_footprint);
            break;
        case MemoryPressureLevel::critical:
            spdlog::error("[MemoryMonitor] CRITICAL: {} ({})", reason, our_footprint);
            break;
        default:
            break;
        }
    } else {
        spdlog::debug("[MemoryMonitor] {} unchanged: {} ({})", pressure_level_to_string(level),
                      reason, our_footprint);
    }

    fire_warning(level, reason, stats, sys_info, growth_kb, report, dispatch);
}

void MemoryMonitor::fire_warning(MemoryPressureLevel level, const std::string& reason,
                                 const MemoryStats& stats, const MemoryInfo& sys_info,
                                 int64_t growth_kb, bool report, bool dispatch) {
    // Copy callbacks under lock, then invoke outside to minimize hold time
    WarningCallback warning_cb;
    std::vector<std::pair<PressureResponderId, std::function<void(MemoryPressureLevel)>>>
        responders;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        warning_cb = warning_callback_;
        responders = pressure_responders_;
    }

    if (report) {
        // Build the event and read smaps outside the lock (smaps_rollup can be slow)
        MemoryWarningEvent event;
        event.level = level;
        event.reason = reason;
        event.stats = stats;
        event.system_info = sys_info;
        event.growth_5min_kb = growth_kb;
        const bool smaps_ok = read_smaps_rollup(event.smaps);

        // On warning+, log the smaps breakdown so we can tell what kind of memory
        // we're holding (heap/anon vs mapped files vs shared).
        if (smaps_ok && level >= MemoryPressureLevel::warning) {
            spdlog::warn("[MemoryMonitor] smaps: rss={}kB pss={}kB private_dirty={}kB "
                         "private_clean={}kB shared_clean={}kB shared_dirty={}kB swap={}kB",
                         event.smaps.rss_kb, event.smaps.pss_kb, event.smaps.private_dirty_kb,
                         event.smaps.private_clean_kb, event.smaps.shared_clean_kb,
                         event.smaps.shared_dirty_kb, event.smaps.swap_kb);
        }

        if (warning_cb) {
            warning_cb(event);
        }
    }

    if (!dispatch) {
        return;
    }

    // Snapshot RSS around the responder loop so we can log how much memory
    // the responders actually freed. Individual responders already log what
    // THEY did — this gives us the whole-app delta.
    //
    // Sample RSS *immediately before* the loop (not from the outer `stats`
    // captured at monitor_loop start) so the warning-log, smaps read, and
    // mutex-copy time don't inflate the "before" number.
    MemoryStats before_stats = get_current_stats();
    size_t responders_fired = 0;
    size_t responders_threw = 0;

    for (const auto& [id, responder] : responders) {
        const auto& respond = responder; // a lambda cannot capture a structured binding in C++17
        if (helix::contain_exceptions(fmt::format("[MemoryMonitor] Pressure responder {}", id),
                                      [&] { respond(level); })) {
            ++responders_fired;
        } else {
            ++responders_threw;
        }
    }

    // Synchronous breadcrumb: we know the call count immediately. If the UI
    // thread is wedged (often the cause of pressure), the deferred completion
    // log below never runs, and this is what shows up in the tail.
    if (responders_fired > 0 || responders_threw > 0) {
        spdlog::warn("[MemoryMonitor] Pressure response dispatched: {} fired, {} threw",
                     responders_fired, responders_threw);
    } else if (level >= MemoryPressureLevel::warning) {
        spdlog::warn("[MemoryMonitor] No pressure responders registered — nothing to free");
    }

    // Deferred completion log: responders (print-status tree, LVGL image
    // cache) hop to the UI thread via queue_update, and a destroyed widget
    // tree is actually freed by lv_obj_delete_async — a period-0 one-shot
    // timer. A sync get_current_stats() right after the loop would
    // systematically undercount: the RSS delta would read "+0kB" even when
    // multi-hundred-KB reclaim succeeded. So would a single queue hop: the
    // responders' callbacks run in this same UpdateQueue drain, before the
    // lv_timer_handler pass that fires the delete timers. A period-based
    // delay would not fix it either — readiness is timing, and a drain that
    // spends longer than the period in callbacks makes the timer come due in
    // the very pass the deletes fire. So the completion hops UpdateQueue
    // twice: process_pending() swaps the queue before running callbacks, so
    // work queued from inside a drain lands in the NEXT drain — one full
    // handler pass after the delete timers of any responder that defers its
    // delete within one hop. A responder that needed a second hop for its
    // delete would queue that hop ahead of this sample, so its tree frees
    // after it. The inner callback only samples and logs (no LVGL calls), so
    // nothing else can reorder it.
    if (responders_fired > 0) {
#ifdef __linux__
        const size_t before_rss_kb = before_stats.vm_rss_kb;
        const size_t fired = responders_fired;
        helix::ui::queue_update("MemoryMonitor::fire_warning", [before_rss_kb, fired]() {
            helix::ui::queue_update("MemoryMonitor::fire_warning", [before_rss_kb, fired]() {
                MemoryStats after = MemoryMonitor::get_current_stats();
                int64_t delta_kb =
                    static_cast<int64_t>(after.vm_rss_kb) - static_cast<int64_t>(before_rss_kb);
                spdlog::warn("[MemoryMonitor] Pressure response complete: "
                             "{} responder(s), "
                             "RSS {}MB -> {}MB ({:+}kB)",
                             fired, before_rss_kb / 1024, after.vm_rss_kb / 1024, delta_kb);
            });
        });
#endif
    }
}

void MemoryMonitor::monitor_loop() {
    log_now("start");

    MemoryStats prev_stats = get_current_stats();

    while (running_.load()) {
        // Sleep in small chunks so we can respond to stop() quickly
        int remaining_ms = interval_ms_.load();
        while (remaining_ms > 0 && running_.load()) {
            int sleep_ms = std::min(remaining_ms, 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            remaining_ms -= sleep_ms;
        }

        if (!running_.load()) {
            break;
        }

        MemoryStats stats = get_current_stats();

        // Calculate deltas
        int64_t rss_delta =
            static_cast<int64_t>(stats.vm_rss_kb) - static_cast<int64_t>(prev_stats.vm_rss_kb);
        int64_t vm_delta =
            static_cast<int64_t>(stats.vm_size_kb) - static_cast<int64_t>(prev_stats.vm_size_kb);

        // Log with delta if significant change (>100kB)
        if (std::abs(rss_delta) > 100 || std::abs(vm_delta) > 100) {
            spdlog::trace(
                "[MemoryMonitor] RSS={}kB ({:+}kB) VmSize={}kB ({:+}kB) VmData={}kB Swap={}kB",
                stats.vm_rss_kb, rss_delta, stats.vm_size_kb, vm_delta, stats.vm_data_kb,
                stats.vm_swap_kb);
        } else {
            spdlog::trace("[MemoryMonitor] RSS={}kB VmSize={}kB VmData={}kB Swap={}kB",
                          stats.vm_rss_kb, stats.vm_size_kb, stats.vm_data_kb, stats.vm_swap_kb);
        }

        // Evaluate pressure thresholds
        evaluate_thresholds(stats);

        // Liveness runs on the same cadence. It is checked here rather than from
        // its own thread because only a background thread can observe a wedged
        // UI thread at all, and this one already wakes on the right interval.
        check_main_loop_liveness();

        // Deep sample every 6th tick (30s at 5s interval): record RSS for growth tracking
        ++deep_sample_counter_;
        if (deep_sample_counter_ >= 6) {
            deep_sample_counter_ = 0;

            rss_history_[rss_history_index_] = stats.vm_rss_kb;
            rss_history_index_ = (rss_history_index_ + 1) % kRssHistorySize;
            if (rss_history_count_ < kRssHistorySize) {
                ++rss_history_count_;
            }
        }

        prev_stats = stats;
    }

    log_now("stop");
}

} // namespace helix
