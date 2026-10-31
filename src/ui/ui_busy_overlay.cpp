// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_busy_overlay.h"

#include "ui_effects.h"
#include "ui_update_queue.h"
#include "ui_utils.h"
#include "ui_widget_ref.h"

#include "format_utils.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdio>

namespace helix {

// ============================================================================
// STATIC STATE (namespace-level for internal use)
// ============================================================================
namespace {

helix::ui::WidgetRef g_overlay;      // Full-screen backdrop
lv_obj_t* g_spinner = nullptr;       // Centered spinner
lv_obj_t* g_label = nullptr;         // Progress text below spinner
lv_timer_t* g_grace_timer = nullptr; // Delayed show timer
bool g_pending_show = false;         // Show requested but grace period not elapsed
std::string g_pending_text;          // Text to show when overlay appears

// Backdrop opacity matches modal system (~70%)
constexpr uint8_t kOverlayBackdropOpacity = 180;

// Spacing between spinner and label
constexpr lv_coord_t kSpinnerLabelGap = 16;

// Forward declarations
void create_overlay_internal();
void destroy_overlay_internal();

// ============================================================================
// INTERNAL HELPERS (namespace-level)
// ============================================================================

void create_overlay_internal() {
    if (g_overlay) {
        spdlog::warn("[BusyOverlay] Overlay already exists - skipping creation");
        return;
    }

    // Double-check pending flag (guards against race with hide())
    if (!g_pending_show) {
        spdlog::debug("[BusyOverlay] Pending show cancelled before creation");
        return;
    }

    // Use top layer instead of active screen - survives screen changes
    // This prevents dangling pointer if user navigates while overlay is visible
    lv_obj_t* parent = lv_layer_top();

    // Create full-screen backdrop using shared utility
    g_overlay = helix::ui::create_fullscreen_backdrop(parent, kOverlayBackdropOpacity);
    if (!g_overlay) {
        spdlog::error("[BusyOverlay] Failed to create backdrop");
        g_pending_show = false;
        return;
    }

    // Create container for centered content (spinner + label)
    lv_obj_t* container = lv_obj_create(g_overlay);
    lv_obj_set_size(container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(container, kSpinnerLabelGap, LV_PART_MAIN);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // Create spinner using XML widget (with fallback to raw LVGL)
    g_spinner = static_cast<lv_obj_t*>(lv_xml_create(container, "spinner", nullptr));
    if (!g_spinner) {
        // Fallback: create spinner using raw LVGL API
        g_spinner = lv_spinner_create(container);
        if (g_spinner) {
            lv_spinner_set_anim_params(g_spinner, 1000, 200); // 1s rotation, 200deg arc
            // Match what the <spinner> XML widget would have produced on the
            // happy path, rather than freezing the small-breakpoint value here.
            int32_t spinner_size = theme_manager_get_spacing("spinner_lg");
            if (spinner_size <= 0)
                spinner_size = 48;
            lv_obj_set_size(g_spinner, spinner_size, spinner_size);
            spdlog::debug("[BusyOverlay] Using fallback spinner (XML not available)");
        } else {
            spdlog::error("[BusyOverlay] Failed to create spinner widget");
        }
    }

    // Create progress label
    g_label = lv_label_create(container);
    lv_obj_set_style_text_color(g_label, theme_manager_get_color("text"), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_label, theme_manager_get_font("font_small"), LV_PART_MAIN);
    lv_label_set_text(g_label, g_pending_text.c_str());

    // Bring to foreground
    lv_obj_move_foreground(g_overlay);

    g_pending_show = false;
    spdlog::debug("[BusyOverlay] Created overlay with text: '{}'", g_pending_text);
}

void destroy_overlay_internal() {
    if (g_overlay) {
        helix::ui::safe_delete_deferred(g_overlay);
        // g_spinner and g_label are children of g_overlay and were destroyed with it
        g_spinner = nullptr;
        g_label = nullptr;
        spdlog::trace("[BusyOverlay] Destroyed overlay");
    }
}

// Timer callback (must be after create_overlay_internal definition)
void grace_timer_cb(lv_timer_t* timer) {
    (void)timer;

    // Clear the timer reference - LVGL auto-deletes one-shot timers after callback
    // We only clear the reference here; do NOT call lv_timer_delete on one-shot timers
    g_grace_timer = nullptr;

    if (!lv_is_initialized()) {
        return; // Shutdown in progress
    }

    // Only create if still pending (hide() not called during grace period)
    if (g_pending_show) {
        create_overlay_internal();
    }
}

} // namespace

// ============================================================================
// PUBLIC API
// ============================================================================

void BusyOverlay::show(const std::string& initial_text, uint32_t grace_period_ms) {
    // Store text for when overlay actually appears
    g_pending_text = initial_text;

    // If already visible, just update text
    if (g_overlay) {
        if (g_label) {
            lv_label_set_text(g_label, initial_text.c_str());
        }
        spdlog::debug("[BusyOverlay] Already visible - updated text to: '{}'", initial_text);
        return;
    }

    // If already pending, update text but don't restart timer
    if (g_pending_show) {
        spdlog::debug("[BusyOverlay] Already pending - updated text to: '{}'", initial_text);
        return;
    }

    // Mark as pending and start grace timer
    g_pending_show = true;

    if (grace_period_ms == 0) {
        // No grace period - show immediately
        create_overlay_internal();
    } else {
        // Start grace timer
        g_grace_timer = lv_timer_create(grace_timer_cb, grace_period_ms, nullptr);
        lv_timer_set_repeat_count(g_grace_timer, 1); // One-shot
        spdlog::debug("[BusyOverlay] Started grace timer ({}ms) for: '{}'", grace_period_ms,
                      initial_text);
    }
}

void BusyOverlay::queue_progress(const std::string& operation, size_t done, size_t total) {
    const float pct =
        (total > 0) ? (100.0f * static_cast<float>(done) / static_cast<float>(total)) : 0.0f;
    helix::ui::queue_update("BusyOverlay::queue_progress",
                            [operation, pct]() { BusyOverlay::set_progress(operation, pct); });
}

void BusyOverlay::set_progress(const std::string& operation, float percent) {
    // Format: "Operation... XX%"
    char percent_buf[12];
    helix::format::format_percent_float(percent, 0, percent_buf, sizeof(percent_buf));
    char buf[128];
    snprintf(buf, sizeof(buf), "%s... %s", operation.c_str(), percent_buf);

    // Update pending text (in case overlay not yet visible)
    g_pending_text = buf;

    // Update label if visible
    if (g_overlay && g_label) {
        lv_label_set_text(g_label, buf);
    }
}

void BusyOverlay::hide() {
    // Cancel grace timer if pending
    if (g_grace_timer) {
        lv_timer_delete(g_grace_timer);
        g_grace_timer = nullptr;
        spdlog::debug("[BusyOverlay] Cancelled grace timer");
    }

    g_pending_show = false;
    g_pending_text.clear();

    // Destroy overlay if visible
    if (g_overlay) {
        destroy_overlay_internal();
    }
}

bool BusyOverlay::is_visible() {
    return g_overlay != nullptr;
}

bool BusyOverlay::is_pending() {
    return g_pending_show && !g_overlay;
}

} // namespace helix
