// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "memory_monitor.h"

#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef __linux__
#include <lvgl.h>
#endif

#ifdef __linux__
#include <fstream>
#include <sstream>
#include <string>
#endif

namespace helix {

static constexpr auto RATE_LIMIT_INTERVAL = std::chrono::minutes(5);

MemoryThresholds MemoryThresholds::for_device(const MemoryInfo& info) {
    MemoryThresholds t;

    if (info.is_constrained_device()) {
        // <256MB (AD5M ~110MB, K1C ~214MB): tight budgets
        // App baseline RSS is ~20MB — warn well above baseline to avoid false positives
        t.warn_rss_kb = 30 * 1024;
        t.critical_rss_kb = 40 * 1024;
        t.warn_available_kb = 15 * 1024;
        t.critical_available_kb = 8 * 1024;
        t.growth_5min_kb = 1 * 1024;
    } else if (info.is_normal_device()) {
        // 256-512MB
        t.warn_rss_kb = 120 * 1024;
        t.critical_rss_kb = 180 * 1024;
        t.warn_available_kb = 32 * 1024;
        t.critical_available_kb = 16 * 1024;
        t.growth_5min_kb = 3 * 1024;
    } else {
        // >512MB (Pi 4/5, desktop) — scale RSS thresholds as a fraction of total RAM.
        // On a 4GB Pi we are usually the largest process and can legitimately use
        // most of it for the 3D viewer; on a desktop we want even more headroom.
        // Floors keep us reasonable on small "good" devices (e.g. 512MB-1GB).
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

    // Wrap — EAGAIN under thread exhaustion throws std::system_error ([L083]).
    try {
        monitor_thread_ = std::thread([this]() { monitor_loop(); });
        spdlog::debug("[MemoryMonitor] Started (interval={}ms)", interval_ms);
    } catch (const std::system_error& e) {
        spdlog::error("[MemoryMonitor] Failed to spawn monitor thread: {}", e.what());
        running_.store(false);
    }
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
    std::ifstream status("/proc/self/status");
    if (!status.is_open()) {
        return stats;
    }

    std::string line;
    while (std::getline(status, line)) {
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

MemoryPressureLevel compute_pressure_level(const MemoryStats& stats,
                                           const MemoryThresholds& thresholds,
                                           MemoryPressureLevel current_level,
                                           const MemoryInfo& sys_info, int64_t growth_kb) {
    MemoryPressureLevel level = MemoryPressureLevel::none;

    // RSS thresholds — check escalations first, then hysteresis holds
    if (stats.vm_rss_kb >= thresholds.critical_rss_kb) {
        level = MemoryPressureLevel::critical;
    } else if (current_level >= MemoryPressureLevel::critical &&
               stats.vm_rss_kb >= thresholds.clear_critical_rss_kb) {
        level = MemoryPressureLevel::critical; // Hold critical — hasn't cleared
    } else if (stats.vm_rss_kb >= thresholds.warn_rss_kb) {
        level = MemoryPressureLevel::warning;
    } else if (current_level >= MemoryPressureLevel::warning &&
               stats.vm_rss_kb >= thresholds.clear_warn_rss_kb) {
        level = MemoryPressureLevel::warning; // Hold warning — hasn't cleared
    }

    // System available memory (may escalate level)
    // Note: lower available_kb = worse, so clear thresholds are ABOVE trigger thresholds
    if (sys_info.available_kb > 0) {
        if (sys_info.available_kb <= thresholds.critical_available_kb &&
            level < MemoryPressureLevel::critical) {
            level = MemoryPressureLevel::critical;
        } else if (current_level >= MemoryPressureLevel::critical &&
                   sys_info.available_kb <= thresholds.clear_critical_available_kb &&
                   level < MemoryPressureLevel::critical) {
            level = MemoryPressureLevel::critical; // Hold critical
        } else if (sys_info.available_kb <= thresholds.warn_available_kb &&
                   level < MemoryPressureLevel::warning) {
            level = MemoryPressureLevel::warning;
        } else if (current_level >= MemoryPressureLevel::warning &&
                   sys_info.available_kb <= thresholds.clear_warn_available_kb &&
                   level < MemoryPressureLevel::warning) {
            level = MemoryPressureLevel::warning; // Hold warning
        }
    }

    // Growth rate (may trigger elevated) — but ONLY when the device is already
    // approaching its limits. A fast one-time climb is benign when there is
    // ample headroom and must not warn: the 3D G-code viewer loading at print
    // start adds ~13MB over 5 minutes, which on a 512MB-class AD5X (RSS ~48MB,
    // hundreds of MB free) is well under every absolute band. Before this gate,
    // every AD5X print start emitted a spurious `elevated` telemetry warning.
    //
    // Genuinely tight devices (AD5M 128MB, RSS baseline ~20MB vs warn_rss 30MB)
    // sit above warn_rss/2 essentially all the time, so growth still escalates
    // there — exactly where rapid growth actually matters.
    const bool near_rss_limit = stats.vm_rss_kb >= thresholds.warn_rss_kb / 2;
    const bool low_available =
        sys_info.available_kb > 0 && sys_info.available_kb <= thresholds.warn_available_kb * 2;
    if ((near_rss_limit || low_available) &&
        growth_kb > static_cast<int64_t>(thresholds.growth_5min_kb)) {
        if (level < MemoryPressureLevel::elevated) {
            level = MemoryPressureLevel::elevated;
        }
    }

    return level;
}

void MemoryMonitor::evaluate_thresholds(const MemoryStats& stats) {
    // No meaningful stats on non-Linux — skip evaluation
    if (stats.vm_rss_kb == 0 && stats.vm_size_kb == 0) {
        return;
    }

    auto sys_info = get_system_memory_info();

    // Compute smoothed growth from circular buffer
    int64_t growth_kb = 0;
    if (rss_history_count_ >= RSS_HISTORY_SIZE) {
        // Average of 3 oldest samples
        size_t avg_oldest = 0;
        for (size_t i = 0; i < 3; i++) {
            avg_oldest += rss_history_[(rss_history_index_ + i) % RSS_HISTORY_SIZE];
        }
        avg_oldest /= 3;

        // Average of 3 newest samples (the 3 before rss_history_index_)
        size_t avg_newest = 0;
        for (size_t i = 1; i <= 3; i++) {
            avg_newest +=
                rss_history_[(rss_history_index_ + RSS_HISTORY_SIZE - i) % RSS_HISTORY_SIZE];
        }
        avg_newest /= 3;

        growth_kb = static_cast<int64_t>(avg_newest) - static_cast<int64_t>(avg_oldest);
    }

    auto current = pressure_level_.load();
    MemoryPressureLevel level =
        compute_pressure_level(stats, thresholds_, current, sys_info, growth_kb);

    // Update atomic level
    pressure_level_.store(level);

    // Rate-limited log and fire callback on non-none levels
    if (level != MemoryPressureLevel::none) {
        auto now = std::chrono::steady_clock::now();
        int level_idx = static_cast<int>(level);
        if (now - last_warning_time_[level_idx] >= RATE_LIMIT_INTERVAL) {
            last_warning_time_[level_idx] = now;

            std::string reason;
            if (stats.vm_rss_kb >= thresholds_.critical_rss_kb) {
                reason = fmt::format("RSS {}MB exceeds critical threshold {}MB",
                                     stats.vm_rss_kb / 1024, thresholds_.critical_rss_kb / 1024);
            } else if (stats.vm_rss_kb >= thresholds_.warn_rss_kb) {
                reason = fmt::format("RSS {}MB exceeds warning threshold {}MB",
                                     stats.vm_rss_kb / 1024, thresholds_.warn_rss_kb / 1024);
            } else if (sys_info.available_kb > 0 &&
                       sys_info.available_kb <= thresholds_.critical_available_kb) {
                reason =
                    fmt::format("System available {}MB below critical threshold {}MB",
                                sys_info.available_mb(), thresholds_.critical_available_kb / 1024);
            } else if (sys_info.available_kb > 0 &&
                       sys_info.available_kb <= thresholds_.warn_available_kb) {
                reason = fmt::format("System available {}MB below warning threshold {}MB",
                                     sys_info.available_mb(), thresholds_.warn_available_kb / 1024);
            } else if (growth_kb > static_cast<int64_t>(thresholds_.growth_5min_kb)) {
                reason = fmt::format("RSS growth {:+}KB over 5 minutes exceeds threshold {}KB",
                                     growth_kb, thresholds_.growth_5min_kb);
            } else {
                reason = fmt::format("Pressure level {} held by hysteresis",
                                     pressure_level_to_string(level));
            }

            // Always include our own memory footprint so logs can distinguish
            // "we are the cause" from "we are a victim of system pressure".
            std::string our_footprint = fmt::format(
                "our RSS={}MB VmSize={}MB VmSwap={}MB growth_5min={:+}KB sys_avail={}MB",
                stats.vm_rss_kb / 1024, stats.vm_size_kb / 1024, stats.vm_swap_kb / 1024, growth_kb,
                sys_info.available_mb());

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

            fire_warning(level, reason, stats, sys_info, growth_kb);
        }
    }
}

void MemoryMonitor::fire_warning(MemoryPressureLevel level, const std::string& reason,
                                 const MemoryStats& stats, const MemoryInfo& sys_info,
                                 int64_t growth_kb) {
    // Build event and read smaps BEFORE acquiring the lock (smaps_rollup can be slow)
    MemoryWarningEvent event;
    event.level = level;
    event.reason = reason;
    event.stats = stats;
    event.system_info = sys_info;
    event.growth_5min_kb = growth_kb;
    bool smaps_ok = read_smaps_rollup(event.smaps);

    // On warning+, log the smaps breakdown so we can tell what kind of memory
    // we're holding (heap/anon vs mapped files vs shared). This is the
    // diagnostic we keep wishing we had when reading crash-bundle log tails.
    if (smaps_ok && level >= MemoryPressureLevel::warning) {
        spdlog::warn("[MemoryMonitor] smaps: rss={}kB pss={}kB private_dirty={}kB "
                     "private_clean={}kB shared_clean={}kB shared_dirty={}kB swap={}kB",
                     event.smaps.rss_kb, event.smaps.pss_kb, event.smaps.private_dirty_kb,
                     event.smaps.private_clean_kb, event.smaps.shared_clean_kb,
                     event.smaps.shared_dirty_kb, event.smaps.swap_kb);
    }

    // Copy callbacks under lock, then invoke outside to minimize hold time
    WarningCallback warning_cb;
    std::vector<std::pair<PressureResponderId, std::function<void(MemoryPressureLevel)>>>
        responders;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        warning_cb = warning_callback_;
        responders = pressure_responders_;
    }

    if (warning_cb) {
        warning_cb(event);
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
        try {
            responder(level);
            ++responders_fired;
        } catch (const std::exception& e) {
            spdlog::error("[MemoryMonitor] Pressure responder {} threw: {}", id, e.what());
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

    // Deferred completion log: both new responders (PrintStatusPanel tree,
    // LVGL image cache) hop to the UI thread via queue_update, and a
    // destroyed widget tree is actually freed by lv_obj_delete_async on the
    // next LVGL async-handler pass. A sync get_current_stats() right after
    // the loop would systematically undercount — the RSS delta would read
    // "+0kB" even when multi-hundred-KB reclaim succeeded. Instead, hop
    // through queue_update (runs in the next UpdateQueue tick, after our
    // deferred reclaim work) and lv_async_call (runs AFTER process_pending's
    // async-delete drain in the same lv_timer_handler pass), so the
    // post-sample reflects sync responders, cache drops, AND widget deletes.
    if (responders_fired > 0) {
#ifdef __linux__
        struct CompletionCtx {
            size_t before_rss_kb;
            size_t fired;
        };
        auto* ctx = new CompletionCtx{before_stats.vm_rss_kb, responders_fired};
        helix::ui::queue_update([ctx]() {
            lv_async_call(
                [](void* ud) {
                    auto* c = static_cast<CompletionCtx*>(ud);
                    MemoryStats after = MemoryMonitor::get_current_stats();
                    int64_t delta_kb = static_cast<int64_t>(after.vm_rss_kb) -
                                       static_cast<int64_t>(c->before_rss_kb);
                    spdlog::warn("[MemoryMonitor] Pressure response complete: {} responder(s), "
                                 "RSS {}MB -> {}MB ({:+}kB)",
                                 c->fired, c->before_rss_kb / 1024, after.vm_rss_kb / 1024,
                                 delta_kb);
                    delete c;
                },
                ctx);
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
            rss_history_index_ = (rss_history_index_ + 1) % RSS_HISTORY_SIZE;
            if (rss_history_count_ < RSS_HISTORY_SIZE) {
                ++rss_history_count_;
            }
        }

        prev_stats = stats;
    }

    log_now("stop");
}

} // namespace helix
