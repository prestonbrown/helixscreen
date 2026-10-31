// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_widget_ref.h"

#include "lvgl/lvgl.h"

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
    /**
     * @brief Get singleton instance
     */
    static MemoryStatsOverlay& instance();

    /**
     * @brief Initialize overlay (creates XML component, starts update timer)
     * @param parent Parent screen to attach overlay to
     * @param initially_visible Whether to show overlay immediately
     */
    void init(lv_obj_t* parent, bool initially_visible = false);

    /**
     * @brief Toggle overlay visibility
     */
    void toggle();

    /**
     * @brief Show overlay
     */
    void show();

    /**
     * @brief Hide overlay
     */
    void hide();

    /**
     * @brief Check if overlay is visible
     */
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

  private:
    MemoryStatsOverlay() = default;
    ~MemoryStatsOverlay();

    // Non-copyable
    MemoryStatsOverlay(const MemoryStatsOverlay&) = delete;
    MemoryStatsOverlay& operator=(const MemoryStatsOverlay&) = delete;

    helix::ui::WidgetRef overlay_;
    lv_obj_t* rss_label_ = nullptr;
    lv_obj_t* hwm_label_ = nullptr;
    lv_obj_t* private_label_ = nullptr;
    lv_obj_t* delta_label_ = nullptr;
    lv_obj_t* pressure_label_ = nullptr;
    lv_timer_t* update_timer_ = nullptr;

    int64_t baseline_rss_kb_ = 0;
    bool initialized_ = false;
};
