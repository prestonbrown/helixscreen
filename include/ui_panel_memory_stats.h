// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_widget_ref.h"

#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

/**
 * @brief Memory Stats Overlay - Development tool for monitoring memory usage
 *
 * Shows a small floating overlay with live memory statistics:
 * - RSS (Resident Set Size): Current physical memory usage
 * - HWM (High Water Mark): Peak memory usage
 * - Private: Private dirty pages (heap + modified pages)
 * - Delta: Change from baseline at startup
 *
 * Toggle visibility with M key or --show-memory flag.
 * Only reads /proc/self/status on Linux; shows placeholder on macOS.
 */
class MemoryStatsOverlay {
  public:
    static MemoryStatsOverlay& instance();

    /**
     * @brief Initialize overlay (creates XML component, starts update timer)
     * @param parent Parent screen to attach overlay to
     * @param initially_visible Whether to show overlay immediately
     */
    void init(lv_obj_t* parent, bool initially_visible = false);

    void toggle();
    void show();
    void hide();
    bool is_visible() const;

    /**
     * @brief Shutdown overlay (stops timer, clears pointers)
     * Must be called before lv_deinit() to prevent stale pointer crashes.
     */
    void shutdown();

    /**
     * @brief Update memory stats display (called by timer)
     */
    void update();

    /// Text tone of a delta or pressure value: the index its bind_styles select.
    enum class Tone : int { Ok = 0, Warn = 1, Bad = 2 };

  private:
    MemoryStatsOverlay() = default;
    ~MemoryStatsOverlay();

    MemoryStatsOverlay(const MemoryStatsOverlay&) = delete;
    MemoryStatsOverlay& operator=(const MemoryStatsOverlay&) = delete;

    void init_subjects();

    helix::ui::WidgetRef overlay_;
    lv_timer_t* update_timer_ = nullptr;

    SubjectManager subjects_;
    lv_subject_t rss_text_{};
    lv_subject_t hwm_text_{};
    lv_subject_t private_text_{};
    lv_subject_t delta_text_{};
    lv_subject_t pressure_text_{};
    lv_subject_t delta_tone_{};
    lv_subject_t pressure_tone_{};
    char rss_buf_[16] = {};
    char hwm_buf_[16] = {};
    char private_buf_[16] = {};
    char delta_buf_[16] = {};
    char pressure_buf_[16] = {};
    bool subjects_initialized_ = false;

    int64_t baseline_rss_kb_ = 0;
    bool initialized_ = false;
};
