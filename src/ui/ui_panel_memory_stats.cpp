// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_memory_stats.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "memory_monitor.h"
#include "memory_utils.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <utility>

// Update interval in milliseconds
static constexpr uint32_t UPDATE_INTERVAL_MS = 2000;

// Timer callback for periodic updates
static void memory_stats_timer_cb(lv_timer_t* timer) {
    auto* overlay = static_cast<MemoryStatsOverlay*>(lv_timer_get_user_data(timer));
    if (overlay) {
        overlay->update();
    }
}

MemoryStatsOverlay& MemoryStatsOverlay::instance() {
    static MemoryStatsOverlay instance;
    return instance;
}

MemoryStatsOverlay::~MemoryStatsOverlay() {
    // Clean up timer - must be deleted explicitly before LVGL shutdown
    // Check lv_is_initialized() to avoid crash during static destruction
    if (lv_is_initialized()) {
        if (update_timer_) {
            lv_timer_delete(update_timer_);
            update_timer_ = nullptr;
        }
    }
}

void MemoryStatsOverlay::init(lv_obj_t* /*parent*/, bool initially_visible) {
    if (initialized_) {
        spdlog::debug("[MemoryStats] Already initialized");
        return;
    }

    // Create overlay on top layer to ensure it's always visible above everything
    lv_obj_t* top_layer = lv_layer_top();
    if (!top_layer) {
        spdlog::error("[MemoryStats] Cannot get top layer");
        return;
    }

    // Create overlay from XML on the top layer
    overlay_ = static_cast<lv_obj_t*>(lv_xml_create(top_layer, "memory_stats_overlay", nullptr));
    if (!overlay_) {
        spdlog::error("[MemoryStats] Failed to create overlay from XML");
        return;
    }

    // Find label widgets
    rss_label_ = helix::ui::find_required(overlay_, "rss_value", "MemoryStats");
    hwm_label_ = helix::ui::find_required(overlay_, "hwm_value", "MemoryStats");
    private_label_ = helix::ui::find_required(overlay_, "private_value", "MemoryStats");
    delta_label_ = helix::ui::find_required(overlay_, "delta_value", "MemoryStats");
    pressure_label_ = helix::ui::find_required(overlay_, "pressure_value", "MemoryStats");

    // Capture baseline RSS
    int64_t rss_kb = 0, hwm_kb = 0;
    if (helix::read_memory_stats(rss_kb, hwm_kb)) {
        baseline_rss_kb_ = rss_kb;
    }

    // Create update timer
    update_timer_ = lv_timer_create(memory_stats_timer_cb, UPDATE_INTERVAL_MS, this);

    // Initial visibility
    if (initially_visible) {
        show();
    } else {
        hide();
    }

    initialized_ = true;

    // Register shutdown with StaticPanelRegistry to ensure timer is stopped before lv_deinit()
    StaticPanelRegistry::instance().register_destroy("MemoryStatsOverlay",
                                                     []() { instance().shutdown(); });

    spdlog::debug("[MemoryStats] Overlay initialized (baseline={}KB)", baseline_rss_kb_);
}

void MemoryStatsOverlay::toggle() {
    if (!overlay_)
        return;

    if (is_visible()) {
        hide();
    } else {
        show();
    }
}

void MemoryStatsOverlay::show() {
    if (!overlay_)
        return;

    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay_);
    update(); // Immediate update when shown
    spdlog::debug("[MemoryStats] Overlay shown");
}

void MemoryStatsOverlay::hide() {
    if (!overlay_)
        return;

    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    spdlog::debug("[MemoryStats] Overlay hidden");
}

bool MemoryStatsOverlay::is_visible() const {
    if (!overlay_)
        return false;
    return !lv_obj_has_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
}

void MemoryStatsOverlay::shutdown() {
    if (!initialized_) {
        return;
    }

    spdlog::debug("[MemoryStats] Shutting down");

    // Stop the timer first to prevent callbacks during cleanup.
    // Note: With proper shutdown order (StaticPanelRegistry before lv_deinit),
    // lv_is_initialized() should always be true here. The check is defense-in-depth.
    if (update_timer_ && lv_is_initialized()) {
        lv_timer_delete(update_timer_);
        update_timer_ = nullptr;
    }

    // Clear all LVGL object pointers (objects will be destroyed by lv_deinit)
    overlay_ = nullptr;
    rss_label_ = nullptr;
    hwm_label_ = nullptr;
    private_label_ = nullptr;
    delta_label_ = nullptr;
    pressure_label_ = nullptr;

    initialized_ = false;
}

void MemoryStatsOverlay::update() {
    // Guard against shutdown race: timer may fire after LVGL objects destroyed
    // but before shutdown() clears our pointers. This can happen when:
    // 1. SDL_WINDOWEVENT_CLOSE triggers lv_display_delete() inside lv_timer_handler()
    // 2. Display deletion destroys all UI objects
    // 3. Same lv_timer_handler() call continues and runs this timer
    // The lv_obj_is_valid() check catches this race condition.
    if (!lv_is_initialized() || !overlay_ || !lv_obj_is_valid(overlay_) || !is_visible())
        return;

    int64_t rss_kb = 0, hwm_kb = 0, private_kb = 0;

    if (helix::read_memory_stats(rss_kb, hwm_kb)) {
        helix::read_private_dirty(private_kb);

        int64_t delta_kb = rss_kb - baseline_rss_kb_;

        // Format as MB with one decimal place
        auto format_mb = [](int64_t kb) -> std::pair<int, int> {
            int mb = static_cast<int>(kb / 1024);
            int decimal = static_cast<int>((kb % 1024) * 10 / 1024);
            return {mb, decimal};
        };

        // Update labels with nicely formatted MB values
        if (rss_label_) {
            auto [mb, dec] = format_mb(rss_kb);
            lv_label_set_text_fmt(rss_label_, "%d.%d", mb, dec);
        }
        if (hwm_label_) {
            auto [mb, dec] = format_mb(hwm_kb);
            lv_label_set_text_fmt(hwm_label_, "%d.%d", mb, dec);
        }
        if (private_label_) {
            if (private_kb > 0) {
                auto [mb, dec] = format_mb(private_kb);
                lv_label_set_text_fmt(private_label_, "%d.%d", mb, dec);
            } else {
                lv_label_set_text(private_label_, "--");
            }
        }
        if (delta_label_) {
            auto [mb, dec] = format_mb(delta_kb >= 0 ? delta_kb : -delta_kb);
            lv_label_set_text_fmt(delta_label_, "%s%d.%d", delta_kb >= 0 ? "+" : "-", mb, dec);
            // Color based on growth: green = stable, yellow = growing, red = high growth
            if (delta_kb < 500) {
                lv_obj_set_style_text_color(delta_label_, theme_manager_get_color("success"),
                                            LV_PART_MAIN);
            } else if (delta_kb < 2000) {
                lv_obj_set_style_text_color(delta_label_, theme_manager_get_color("warning"),
                                            LV_PART_MAIN);
            } else {
                lv_obj_set_style_text_color(delta_label_, theme_manager_get_color("danger"),
                                            LV_PART_MAIN);
            }
        }
    } else {
        // Not available
        if (rss_label_)
            lv_label_set_text(rss_label_, "N/A");
        if (hwm_label_)
            lv_label_set_text(hwm_label_, "N/A");
        if (private_label_)
            lv_label_set_text(private_label_, "N/A");
        if (delta_label_)
            lv_label_set_text(delta_label_, "N/A");
    }

    // Pressure level is available regardless of /proc stats
    if (pressure_label_) {
        auto level = helix::MemoryMonitor::instance().pressure_level();
        const char* level_str = nullptr;
        const char* color_token = nullptr;
        switch (level) {
        case helix::MemoryPressureLevel::none:
            level_str = "OK";
            color_token = "success";
            break;
        case helix::MemoryPressureLevel::elevated:
            level_str = "ELEVATED";
            color_token = "warning";
            break;
        case helix::MemoryPressureLevel::warning:
            level_str = "WARNING";
            color_token = "warning";
            break;
        case helix::MemoryPressureLevel::critical:
            level_str = "CRITICAL";
            color_token = "danger";
            break;
        }
        lv_label_set_text_fmt(pressure_label_, "%s", level_str);
        lv_obj_set_style_text_color(pressure_label_, theme_manager_get_color(color_token),
                                    LV_PART_MAIN);
    }
}
