// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_busy_overlay.h"

#include "ui_effects.h"
#include "ui_utils.h"
#include "ui_widget_ref.h"

#include "core/static_subject_registry.h"
#include "core/subject.h"
#include "core/update_queue.h"
#include "format_utils.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <spdlog/spdlog.h>

#include <cstdio>

namespace helix {

// ============================================================================
// STATIC STATE (namespace-level for internal use)
// ============================================================================
namespace {

helix::ui::WidgetRef g_overlay;      // Full-screen backdrop
lv_timer_t* g_grace_timer = nullptr; // Delayed show timer
bool g_pending_show = false;         // Show requested but grace period not elapsed

// Backdrop opacity matches modal system (~70%)
constexpr uint8_t kOverlayBackdropOpacity = 180;

/// busy_overlay_text: the line under the spinner, bound by busy_overlay.xml.
/// Holds the text while the grace period runs, so the overlay appears with it.
struct BusyTextSubject {
    helix::Subject subject{};
    char buf[256] = {};
    SubjectManager subjects;
    bool ready = false;
};

BusyTextSubject& text_state() {
    static BusyTextSubject s;
    return s;
}

void deinit_text_subject() {
    auto& s = text_state();
    if (!s.ready) {
        return;
    }
    s.subjects.deinit_all();
    s.ready = false;
}

lv_subject_t* busy_text_subject() {
    auto& s = text_state();
    if (!s.ready) {
        UI_MANAGED_SUBJECT_STRING(s.subject, s.buf, "", "busy_overlay_text", s.subjects);
        s.ready = true;
        StaticSubjectRegistry::instance().register_deinit("BusyOverlay", deinit_text_subject,
                                                          StaticSubjectRegistry::Lifetime::Process);
    }
    return &s.subject;
}

void set_text(const char* text) {
    lv_subject_set_string(busy_text_subject(), text);
}

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

    // Blurred or dimmed snapshot of the screen: built from a render, so it stays C++
    g_overlay = helix::ui::create_fullscreen_backdrop(parent, kOverlayBackdropOpacity);
    if (!g_overlay) {
        spdlog::error("[BusyOverlay] Failed to create backdrop");
        g_pending_show = false;
        return;
    }

    busy_text_subject();
    if (!lv_xml_create(g_overlay, "busy_overlay", nullptr)) {
        spdlog::error("[BusyOverlay] Failed to create busy_overlay component");
    }

    // Bring to foreground
    lv_obj_move_to_index(g_overlay, -1);

    g_pending_show = false;
    spdlog::debug("[BusyOverlay] Created overlay with text: '{}'",
                  lv_subject_get_string(busy_text_subject()));
}

void destroy_overlay_internal() {
    if (g_overlay) {
        helix::ui::safe_delete_deferred(g_overlay);
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
    // Bound by the label, and held for the overlay if it is still pending
    set_text(initial_text.c_str());

    // If already visible, the label already shows the new text
    if (g_overlay) {
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

    set_text(buf);
}

void BusyOverlay::hide() {
    if (g_grace_timer) {
        lv_timer_delete(g_grace_timer);
        g_grace_timer = nullptr;
        spdlog::debug("[BusyOverlay] Cancelled grace timer");
    }

    // The text stays: the backdrop is deleted on a later tick, and the next
    // show() sets its own.
    g_pending_show = false;

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
