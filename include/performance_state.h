// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "performance_source.h"
#include "subject_managed_panel.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace helix {
namespace perf {

/// Size of the per-metric ring buffer (60 samples × ~1 Hz = ~60 s of history).
constexpr std::size_t HISTORY_SAMPLES = 60;

/// Singleton — sibling to PrinterState/AmsState/ToolState.
class PerformanceState {
  public:
    static PerformanceState& instance();

    /// Register subjects and self-register cleanup. Idempotent.
    /// Called from Application::init_subsystems() after IMoonrakerAPI is up.
    void init_subjects();

    /// Tear down subjects. Called from StaticSubjectRegistry on shutdown.
    void deinit_subjects();

    /// Install a data source and start it. Replaces any prior source.
    /// Owns the source. Safe to call multiple times (re-init for reconnect).
    void set_source(std::unique_ptr<IPerformanceSource> source);

    /// Read a snapshot of a ring buffer (for sparkline paint). Returns the
    /// buffer in chronological order (oldest first). Returns empty when the
    /// named buffer doesn't exist.
    std::vector<float> read_history(const std::string& name) const;

    /// Lifetime token for the perf subjects. Observers of any perf subject
    /// (e.g. HelixSparkline on perf_history_tick) MUST pass this to observe_*()
    /// so their ObserverGuard releases safely instead of calling
    /// lv_observer_remove() on a freed subject. These subjects are NOT a
    /// never-freed singleton: deinit_subjects()/init_subjects() destroy and
    /// recreate them on reconnect, printer switch, and between tests. Null until
    /// init_subjects() has run.
    const SubjectLifetime& subjects_lifetime() const {
        return subjects_lifetime_;
    }

  private:
    friend class PerformanceStateTestAccess;

    PerformanceState() = default;
    PerformanceState(const PerformanceState&) = delete;
    PerformanceState& operator=(const PerformanceState&) = delete;

    // ---- Main-thread sample application ----
    void apply_sample(const PerfSample& sample);
    void update_about_summary(const PerfSample& s);
    void update_mcu_subjects(const std::vector<McuStat>& mcus);
    void push_history(const std::string& key, float value);
    static std::string mcu_safe_name(const std::string& raw);

    // ---- Subjects (static set) ----
    SubjectManager subjects_;
    // Shared lifetime handed to observers of the perf subjects. Reset (and its
    // bool flipped false) in deinit_subjects() BEFORE the subjects are freed so
    // ObserverGuard::reset() detects the dead subject. See subjects_lifetime().
    SubjectLifetime subjects_lifetime_;
    lv_subject_t s_host_cpu_pct_{};
    lv_subject_t s_host_cpu_pct_present_{};
    lv_subject_t s_host_cpu_temp_c10_{};
    lv_subject_t s_host_cpu_temp_present_{};
    lv_subject_t s_host_mem_free_mb_{};
    lv_subject_t s_host_mem_pct_used_{};
    lv_subject_t s_host_mem_present_{};
    lv_subject_t s_host_throttle_state_{};
    lv_subject_t s_host_throttle_text_{};
    lv_subject_t s_mcu_names_{};
    lv_subject_t s_about_summary_{};
    lv_subject_t s_available_{};
    lv_subject_t s_history_tick_{};
    lv_subject_t s_host_cpu_pct_text_{};
    lv_subject_t s_host_mem_free_text_{};

    char buf_throttle_text_[96]{};
    char buf_mcu_names_[256]{};
    char buf_about_summary_[64]{};
    char buf_cpu_text_[48]{};
    char buf_mem_text_[48]{};

    // ---- Dynamic per-MCU subjects ----
    struct McuSubjects {
        lv_subject_t load_pct{};
        lv_subject_t retrans{};
        lv_subject_t present{};
        lv_subject_t text{};
        char buf_text[64]{};

        ~McuSubjects() {
            lv_subject_deinit(&load_pct);
            lv_subject_deinit(&retrans);
            lv_subject_deinit(&present);
            lv_subject_deinit(&text);
        }
    };
    std::unordered_map<std::string, std::unique_ptr<McuSubjects>> mcu_subjects_;

    // ---- Ring buffers ----
    struct Ring {
        std::array<float, HISTORY_SAMPLES> data{};
        std::size_t head = 0; ///< index of NEXT write (rolls)
        std::size_t fill = 0; ///< samples written (clamped to HISTORY_SAMPLES)
    };
    mutable std::mutex history_mu_;
    std::unordered_map<std::string, Ring> history_;

    // ---- Source ----
    std::unique_ptr<IPerformanceSource> source_;
    AsyncLifetimeGuard lifetime_;
    bool initialized_ = false;
};

} // namespace perf
} // namespace helix
