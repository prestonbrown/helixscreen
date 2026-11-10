// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_memory_stats.h"

#include "core/static_panel_registry.h"
#include "core/static_subject_registry.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "memory_monitor.h"
#include "memory_utils.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <utility>

// Update interval in milliseconds
static constexpr uint32_t kUpdateIntervalMs = 2000;

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

void MemoryStatsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    UI_MANAGED_SUBJECT_STRING(rss_text_, rss_buf_, "--", "memory_stats_rss_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(hwm_text_, hwm_buf_, "--", "memory_stats_hwm_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(private_text_, private_buf_, "--", "memory_stats_private_text",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(delta_text_, delta_buf_, "--", "memory_stats_delta_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(pressure_text_, pressure_buf_, "--", "memory_stats_pressure_text",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(delta_tone_, static_cast<int>(Tone::Ok), "memory_stats_delta_tone",
                           subjects_);
    UI_MANAGED_SUBJECT_INT(pressure_tone_, static_cast<int>(Tone::Ok), "memory_stats_pressure_tone",
                           subjects_);
    subjects_initialized_ = true;
    StaticSubjectRegistry::instance().register_deinit("MemoryStatsOverlay", []() {
        auto& self = instance();
        self.subjects_.deinit_all();
        self.subjects_initialized_ = false;
    });
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

    // The overlay's labels bind these, so they exist before it does.
    init_subjects();

    // Create overlay from XML on the top layer
    overlay_ = static_cast<lv_obj_t*>(lv_xml_create(top_layer, "memory_stats_overlay", nullptr));
    if (!overlay_) {
        spdlog::error("[MemoryStats] Failed to create overlay from XML");
        return;
    }

    // Capture baseline RSS
    int64_t rss_kb = 0, hwm_kb = 0;
    if (helix::read_memory_stats(rss_kb, hwm_kb)) {
        baseline_rss_kb_ = rss_kb;
    }

    update_timer_ = lv_timer_create(memory_stats_timer_cb, kUpdateIntervalMs, this);

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

    // The overlay itself is destroyed by lv_deinit
    overlay_ = nullptr;

    initialized_ = false;
}

void MemoryStatsOverlay::update() {
    // Guard against shutdown race: timer may fire after LVGL objects destroyed
    // but before shutdown() clears our pointers. This can happen when:
    // 1. SDL_WINDOWEVENT_CLOSE triggers lv_display_delete() inside lv_timer_handler()
    // 2. Display deletion destroys all UI objects
    // 3. Same lv_timer_handler() call continues and runs this timer
    // overlay_ reads nullptr once the display deletion has destroyed it.
    if (!lv_is_initialized() || !subjects_initialized_ || !overlay_ || !is_visible())
        return;

    int64_t rss_kb = 0, hwm_kb = 0, private_kb = 0;
    char buf[16];

    // Format as MB with one decimal place
    auto set_mb = [&buf](lv_subject_t& subject, const char* sign, int64_t kb) {
        int mb = static_cast<int>(kb / 1024);
        int decimal = static_cast<int>((kb % 1024) * 10 / 1024);
        snprintf(buf, sizeof(buf), "%s%d.%d", sign, mb, decimal);
        lv_subject_copy_string(&subject, buf);
    };

    if (helix::read_memory_stats(rss_kb, hwm_kb)) {
        helix::read_private_dirty(private_kb);

        int64_t delta_kb = rss_kb - baseline_rss_kb_;

        set_mb(rss_text_, "", rss_kb);
        set_mb(hwm_text_, "", hwm_kb);
        if (private_kb > 0) {
            set_mb(private_text_, "", private_kb);
        } else {
            lv_subject_copy_string(&private_text_, "--");
        }
        set_mb(delta_text_, delta_kb >= 0 ? "+" : "-", delta_kb >= 0 ? delta_kb : -delta_kb);
        // Growth tone: stable, growing, high growth
        const Tone tone = delta_kb < 500 ? Tone::Ok : (delta_kb < 2000 ? Tone::Warn : Tone::Bad);
        lv_subject_set_int(&delta_tone_, static_cast<int>(tone));
    } else {
        // Not available
        lv_subject_copy_string(&rss_text_, "N/A");
        lv_subject_copy_string(&hwm_text_, "N/A");
        lv_subject_copy_string(&private_text_, "N/A");
        lv_subject_copy_string(&delta_text_, "N/A");
    }

    // Pressure level is available regardless of /proc stats
    const char* level_str = "OK";
    Tone tone = Tone::Ok;
    switch (helix::MemoryMonitor::instance().pressure_level()) {
    case helix::MemoryPressureLevel::none:
        break;
    case helix::MemoryPressureLevel::elevated:
        level_str = "ELEVATED";
        tone = Tone::Warn;
        break;
    case helix::MemoryPressureLevel::warning:
        level_str = "WARNING";
        tone = Tone::Warn;
        break;
    case helix::MemoryPressureLevel::critical:
        level_str = "CRITICAL";
        tone = Tone::Bad;
        break;
    }
    lv_subject_copy_string(&pressure_text_, level_str);
    lv_subject_set_int(&pressure_tone_, static_cast<int>(tone));
}
