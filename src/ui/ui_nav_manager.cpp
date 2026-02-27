// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_nav_manager.h"

#include "ui_emergency_stop.h"
#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_keyboard_manager.h"
#include "ui_panel_base.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "moonraker_client.h" // For ConnectionState enum
#include "observer_factory.h"
#include "overlay_base.h"
#include "printer_state.h" // For KlippyState enum
#include "sound_manager.h"
#include "static_subject_registry.h"
#include "system/telemetry_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

using namespace helix;
using helix::ui::observe_int_sync;

#include <algorithm>
#include <cstdlib>

// ============================================================================
// SINGLETON INSTANCE
// ============================================================================

// Flag set by NavigationManager destructor to detect static destruction.
// Using namespace-scope static ensures it's initialized before main() and
// outlives all function-local statics, including the singleton itself.
namespace {
bool g_nav_manager_destroyed = false;
}

NavigationManager::~NavigationManager() {
    g_nav_manager_destroyed = true;
}

NavigationManager& NavigationManager::instance() {
    static NavigationManager inst;
    return inst;
}

bool NavigationManager::is_destroyed() {
    // Guard against Static Destruction Order Fiasco.
    // This flag is set by NavigationManager's destructor, so it accurately
    // reflects whether the singleton's internal data structures are valid.
    return g_nav_manager_destroyed;
}

// ============================================================================
// HELPER METHODS
// ============================================================================

const char* NavigationManager::panel_id_to_name(PanelId id) {
    static const char* names[] = {"home_panel",     "print_select_panel", "controls_panel",
                                  "filament_panel", "settings_panel",     "advanced_panel"};
    if (static_cast<int>(id) < UI_PANEL_COUNT) {
        return names[static_cast<int>(id)];
    }
    return "unknown_panel";
}

bool NavigationManager::panel_requires_connection(PanelId panel) {
    return panel == PanelId::Controls || panel == PanelId::Filament;
}

bool NavigationManager::is_printer_connected() const {
    auto* subject = get_printer_state().get_printer_connection_state_subject();
    return lv_subject_get_int(subject) == 2;
}

bool NavigationManager::is_klippy_ready() const {
    auto* subject = get_printer_state().get_klippy_state_subject();
    return lv_subject_get_int(subject) == 0; // KlippyState::READY
}

void NavigationManager::clear_overlay_stack() {
    // Hide all overlay panels immediately (no animation for connection loss)
    while (panel_stack_.size() > 1) {
        lv_obj_t* overlay = panel_stack_.back();
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        // Reset transform and opacity for potential reuse
        lv_obj_set_style_translate_x(overlay, 0, LV_PART_MAIN);
        lv_obj_set_style_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);

        // Clean up dynamic backdrop for this overlay (if one was created)
        auto backdrop_it = overlay_backdrops_.find(overlay);
        if (backdrop_it != overlay_backdrops_.end()) {
            lv_obj_del(backdrop_it->second);
            overlay_backdrops_.erase(backdrop_it);
        }

        panel_stack_.pop_back();
        spdlog::trace("[NavigationManager] Cleared overlay {} from stack", (void*)overlay);
    }

    // Clear zoom source rects for any cleared overlays
    zoom_source_rects_.clear();

    // Hide primary backdrop
    if (overlay_backdrop_) {
        set_backdrop_visible(false);
    }

    spdlog::trace("[NavigationManager] Overlay stack cleared (connection gating)");
}

// ============================================================================
// ANIMATION HELPERS
// ============================================================================

void NavigationManager::overlay_slide_out_complete_cb(lv_anim_t* anim) {
    if (NavigationManager::instance().is_shutting_down()) {
        return; // Shutdown in progress — widget may be freed
    }
    lv_obj_t* panel = static_cast<lv_obj_t*>(anim->var);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    // Reset all transform and opacity properties for potential reuse
    // (covers both slide and zoom animation properties)
    lv_obj_set_style_translate_x(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_translate_y(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(panel, 256, LV_PART_MAIN);
    lv_obj_set_style_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    spdlog::trace("[NavigationManager] Overlay slide+fade-out complete, panel {} hidden",
                  (void*)panel);

    // Defer close callback via lv_async_call so any object deletion happens AFTER the
    // current render cycle completes. Animation callbacks fire from inside
    // lv_timer_handler() → lv_display_refr_timer(), and deleting objects mid-layout
    // causes use-after-free in layout_update_core → lv_obj_scrollbar_invalidate.
    auto& mgr = NavigationManager::instance();
    auto it = mgr.overlay_close_callbacks_.find(panel);
    if (it != mgr.overlay_close_callbacks_.end()) {
        spdlog::trace("[NavigationManager] Deferring close callback for overlay {}", (void*)panel);
        // Move callback to heap — lv_async_call will invoke it on the next LVGL tick
        auto* deferred = new OverlayCloseCallback(std::move(it->second));
        mgr.overlay_close_callbacks_.erase(it);
        lv_async_call(
            [](void* data) {
                auto* cb = static_cast<OverlayCloseCallback*>(data);
                (*cb)();
                delete cb;
            },
            deferred);
    }

    // Lifecycle: Activate what's now visible after animation completes
    // Stack was already modified in go_back(), so check what's now at top
    if (mgr.panel_stack_.size() == 1) {
        // Back to main panel - activate it
        if (mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]) {
            spdlog::trace("[NavigationManager] Activating main panel {} after overlay closed",
                          static_cast<int>(mgr.active_panel_));
            mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]->on_activate();
        }
    } else if (mgr.panel_stack_.size() > 1) {
        // Back to previous overlay - activate it
        lv_obj_t* now_visible = mgr.panel_stack_.back();
        auto overlay_it = mgr.overlay_instances_.find(now_visible);
        if (overlay_it != mgr.overlay_instances_.end() && overlay_it->second) {
            spdlog::trace("[NavigationManager] Activating previous overlay {}",
                          overlay_it->second->get_name());
            overlay_it->second->on_activate();
        }
    }
}

void NavigationManager::overlay_animate_slide_in(lv_obj_t* panel) {
    int32_t panel_width = lv_obj_get_width(panel);
    if (panel_width <= 0) {
        panel_width = OVERLAY_SLIDE_OFFSET;
    }

    // Skip animation if disabled - show panel in final state
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_set_style_translate_x(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        spdlog::trace("[NavigationManager] Animations disabled - showing overlay instantly");
        return;
    }

    // Set initial state: off-screen and transparent
    lv_obj_set_style_translate_x(panel, panel_width, LV_PART_MAIN);
    lv_obj_set_style_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN);

    // Slide animation: translate from right to final position
    lv_anim_t slide_anim;
    lv_anim_init(&slide_anim);
    lv_anim_set_var(&slide_anim, panel);
    lv_anim_set_values(&slide_anim, panel_width, 0);
    lv_anim_set_duration(&slide_anim, OVERLAY_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&slide_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&slide_anim);

    // Fade animation: opacity from transparent to opaque (runs simultaneously)
    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, panel);
    lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade_anim, OVERLAY_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);

    spdlog::trace("[NavigationManager] Started slide+fade-in animation for panel {} (width={})",
                  (void*)panel, panel_width);
}

void NavigationManager::overlay_animate_slide_out(lv_obj_t* panel) {
    // Disable clicks immediately to prevent interaction during animation
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Skip animation if disabled - hide panel immediately and invoke callback
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        // Reset all transform and opacity properties for potential reuse
        lv_obj_set_style_translate_x(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_translate_y(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_transform_scale(panel, 256, LV_PART_MAIN);
        lv_obj_set_style_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        spdlog::trace("[NavigationManager] Animations disabled - hiding overlay instantly");

        // Invoke close callback if registered
        auto& mgr = NavigationManager::instance();
        auto it = mgr.overlay_close_callbacks_.find(panel);
        if (it != mgr.overlay_close_callbacks_.end()) {
            spdlog::trace("[NavigationManager] Invoking close callback for overlay {}",
                          (void*)panel);
            auto callback = std::move(it->second);
            mgr.overlay_close_callbacks_.erase(it);
            callback();
        }

        // Lifecycle: Activate what's now visible (same logic as animation callback)
        if (mgr.panel_stack_.size() == 1) {
            if (mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]) {
                spdlog::trace("[NavigationManager] Activating main panel {} after overlay closed",
                              static_cast<int>(mgr.active_panel_));
                mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]->on_activate();
            }
        } else if (mgr.panel_stack_.size() > 1) {
            lv_obj_t* now_visible = mgr.panel_stack_.back();
            auto overlay_it = mgr.overlay_instances_.find(now_visible);
            if (overlay_it != mgr.overlay_instances_.end() && overlay_it->second) {
                spdlog::trace("[NavigationManager] Activating previous overlay {}",
                              overlay_it->second->get_name());
                overlay_it->second->on_activate();
            }
        }
        return;
    }

    int32_t panel_width = lv_obj_get_width(panel);
    if (panel_width <= 0) {
        panel_width = OVERLAY_SLIDE_OFFSET;
    }

    // Slide animation: translate to off-screen right
    lv_anim_t slide_anim;
    lv_anim_init(&slide_anim);
    lv_anim_set_var(&slide_anim, panel);
    lv_anim_set_values(&slide_anim, 0, panel_width);
    lv_anim_set_duration(&slide_anim, OVERLAY_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&slide_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_set_completed_cb(&slide_anim, overlay_slide_out_complete_cb);
    lv_anim_start(&slide_anim);

    // Fade animation: opacity from opaque to transparent (runs simultaneously)
    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, panel);
    lv_anim_set_values(&fade_anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade_anim, OVERLAY_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);

    spdlog::trace("[NavigationManager] Started slide+fade-out animation for panel {} (width={})",
                  (void*)panel, panel_width);
}

void NavigationManager::overlay_animate_zoom_in(lv_obj_t* panel, lv_area_t source_rect) {
    // Skip animation if disabled
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_set_style_translate_x(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_translate_y(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_transform_scale(panel, 256, LV_PART_MAIN);
        lv_obj_set_style_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        spdlog::trace("[NavigationManager] Animations disabled - showing zoom overlay instantly");
        return;
    }

    // Calculate panel dimensions
    lv_obj_update_layout(panel);
    int32_t panel_w = lv_obj_get_width(panel);
    int32_t panel_h = lv_obj_get_height(panel);
    if (panel_w <= 0)
        panel_w = 480;
    if (panel_h <= 0)
        panel_h = 800;

    // Get panel screen position
    lv_area_t panel_coords;
    lv_obj_get_coords(panel, &panel_coords);

    // Calculate source rect center and panel center
    int32_t src_cx = (source_rect.x1 + source_rect.x2) / 2;
    int32_t src_cy = (source_rect.y1 + source_rect.y2) / 2;
    int32_t panel_cx = (panel_coords.x1 + panel_coords.x2) / 2;
    int32_t panel_cy = (panel_coords.y1 + panel_coords.y2) / 2;

    // Calculate starting translation (offset from panel center to source center)
    int32_t start_tx = src_cx - panel_cx;
    int32_t start_ty = src_cy - panel_cy;

    // Calculate starting scale based on card/panel size ratio
    // LVGL scale: 256 = 100%
    int32_t src_w = source_rect.x2 - source_rect.x1;
    int32_t start_scale = (src_w * 256) / panel_w;
    if (start_scale < 64)
        start_scale = 64; // Min 25% scale
    if (start_scale > 200)
        start_scale = 200; // Max ~78% scale

    spdlog::debug("[NavigationManager] zoom-in: panel={}x{} src=({},{}-{},{}) "
                  "start_tx={} start_ty={} start_scale={}",
                  panel_w, panel_h, source_rect.x1, source_rect.y1, source_rect.x2, source_rect.y2,
                  start_tx, start_ty, start_scale);

    // Set pivot to center for symmetric scaling
    lv_obj_set_style_transform_pivot_x(panel, panel_w / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(panel, panel_h / 2, LV_PART_MAIN);

    // Set initial state
    lv_obj_set_style_translate_x(panel, start_tx, LV_PART_MAIN);
    lv_obj_set_style_translate_y(panel, start_ty, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(panel, static_cast<int16_t>(start_scale), LV_PART_MAIN);
    lv_obj_set_style_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN);

    // Translate X animation
    lv_anim_t tx_anim;
    lv_anim_init(&tx_anim);
    lv_anim_set_var(&tx_anim, panel);
    lv_anim_set_values(&tx_anim, start_tx, 0);
    lv_anim_set_duration(&tx_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&tx_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&tx_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&tx_anim);

    // Translate Y animation
    lv_anim_t ty_anim;
    lv_anim_init(&ty_anim);
    lv_anim_set_var(&ty_anim, panel);
    lv_anim_set_values(&ty_anim, start_ty, 0);
    lv_anim_set_duration(&ty_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&ty_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&ty_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&ty_anim);

    // Scale animation
    lv_anim_t scale_anim;
    lv_anim_init(&scale_anim);
    lv_anim_set_var(&scale_anim, panel);
    lv_anim_set_values(&scale_anim, start_scale, 256);
    lv_anim_set_duration(&scale_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&scale_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&scale_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), static_cast<int16_t>(value),
                                         LV_PART_MAIN);
    });
    lv_anim_start(&scale_anim);

    // Opacity animation
    lv_anim_t opa_anim;
    lv_anim_init(&opa_anim);
    lv_anim_set_var(&opa_anim, panel);
    lv_anim_set_values(&opa_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&opa_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&opa_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&opa_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&opa_anim);

    spdlog::trace("[NavigationManager] Started zoom-in animation for panel {} (scale {}->256, "
                  "tx {}->0, ty {}->0)",
                  (void*)panel, start_scale, start_tx, start_ty);
}

void NavigationManager::overlay_animate_zoom_out(lv_obj_t* panel, lv_area_t source_rect) {
    // Disable clicks during animation
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Skip animation if disabled
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_translate_x(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_translate_y(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_transform_scale(panel, 256, LV_PART_MAIN);
        lv_obj_set_style_opa(panel, LV_OPA_COVER, LV_PART_MAIN);

        // Invoke close callback
        auto it = overlay_close_callbacks_.find(panel);
        if (it != overlay_close_callbacks_.end()) {
            auto callback = std::move(it->second);
            overlay_close_callbacks_.erase(it);
            callback();
        }

        // Lifecycle: Activate what's now visible
        if (panel_stack_.size() == 1) {
            if (panel_instances_[static_cast<int>(active_panel_)]) {
                panel_instances_[static_cast<int>(active_panel_)]->on_activate();
            }
        } else if (panel_stack_.size() > 1) {
            lv_obj_t* now_visible = panel_stack_.back();
            auto overlay_it = overlay_instances_.find(now_visible);
            if (overlay_it != overlay_instances_.end() && overlay_it->second) {
                overlay_it->second->on_activate();
            }
        }
        return;
    }

    // Calculate animation targets (reverse of zoom-in)
    lv_obj_update_layout(panel);
    int32_t panel_w = lv_obj_get_width(panel);
    int32_t panel_h = lv_obj_get_height(panel);
    if (panel_w <= 0)
        panel_w = 480;
    if (panel_h <= 0)
        panel_h = 800;

    lv_area_t panel_coords;
    lv_obj_get_coords(panel, &panel_coords);

    int32_t src_cx = (source_rect.x1 + source_rect.x2) / 2;
    int32_t src_cy = (source_rect.y1 + source_rect.y2) / 2;
    int32_t panel_cx = (panel_coords.x1 + panel_coords.x2) / 2;
    int32_t panel_cy = (panel_coords.y1 + panel_coords.y2) / 2;

    int32_t end_tx = src_cx - panel_cx;
    int32_t end_ty = src_cy - panel_cy;

    int32_t src_w = source_rect.x2 - source_rect.x1;
    int32_t end_scale = (src_w * 256) / panel_w;
    if (end_scale < 64)
        end_scale = 64;
    if (end_scale > 200)
        end_scale = 200;

    lv_obj_set_style_transform_pivot_x(panel, panel_w / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(panel, panel_h / 2, LV_PART_MAIN);

    // Translate X
    lv_anim_t tx_anim;
    lv_anim_init(&tx_anim);
    lv_anim_set_var(&tx_anim, panel);
    lv_anim_set_values(&tx_anim, 0, end_tx);
    lv_anim_set_duration(&tx_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&tx_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&tx_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&tx_anim);

    // Translate Y
    lv_anim_t ty_anim;
    lv_anim_init(&ty_anim);
    lv_anim_set_var(&ty_anim, panel);
    lv_anim_set_values(&ty_anim, 0, end_ty);
    lv_anim_set_duration(&ty_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&ty_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&ty_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&ty_anim);

    // Scale
    lv_anim_t scale_anim;
    lv_anim_init(&scale_anim);
    lv_anim_set_var(&scale_anim, panel);
    lv_anim_set_values(&scale_anim, 256, end_scale);
    lv_anim_set_duration(&scale_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&scale_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&scale_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), static_cast<int16_t>(value),
                                         LV_PART_MAIN);
    });
    lv_anim_start(&scale_anim);

    // Opacity — use the completed callback to handle post-animation cleanup
    lv_anim_t opa_anim;
    lv_anim_init(&opa_anim);
    lv_anim_set_var(&opa_anim, panel);
    lv_anim_set_values(&opa_anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&opa_anim, ZOOM_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&opa_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&opa_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    // Reuse the existing slide-out completion callback for post-animation cleanup
    lv_anim_set_completed_cb(&opa_anim, overlay_slide_out_complete_cb);
    lv_anim_start(&opa_anim);

    spdlog::trace("[NavigationManager] Started zoom-out animation for panel {} (scale 256->{}, "
                  "tx 0->{}, ty 0->{})",
                  (void*)panel, end_scale, end_tx, end_ty);
}

// ============================================================================
// OBSERVER HANDLERS (used by factory-created observers)
// ============================================================================

void NavigationManager::handle_active_panel_change(int32_t new_active_panel) {
    // Show/hide panels if widgets are set
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        if (panel_widgets_[i]) {
            if (i == new_active_panel) {
                lv_obj_remove_flag(panel_widgets_[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(panel_widgets_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void NavigationManager::handle_connection_state_change(int state) {
    bool was_connected =
        (previous_connection_state_ == static_cast<int>(ConnectionState::CONNECTED));
    bool is_connected = (state == static_cast<int>(ConnectionState::CONNECTED));

    // Only redirect if we were previously connected and are now disconnected
    if (was_connected && !is_connected && panel_requires_connection(active_panel_)) {
        spdlog::info("[NavigationManager] Connection lost on panel {} - navigating to home",
                     static_cast<int>(active_panel_));

        clear_overlay_stack();
        set_active(PanelId::Home);
    }

    previous_connection_state_ = state;
}

void NavigationManager::handle_klippy_state_change(int state) {
    bool was_ready = (previous_klippy_state_ == static_cast<int>(KlippyState::READY));
    bool is_ready = (state == static_cast<int>(KlippyState::READY));

    // Redirect to home if klippy enters non-READY state (SHUTDOWN/ERROR) while on restricted panel
    if (was_ready && !is_ready && panel_requires_connection(active_panel_)) {
        const char* state_name = (state == static_cast<int>(KlippyState::SHUTDOWN)) ? "SHUTDOWN"
                                 : (state == static_cast<int>(KlippyState::ERROR))  ? "ERROR"
                                                                                    : "non-READY";
        spdlog::info("[NavigationManager] Klippy {} on panel {} - navigating to home", state_name,
                     static_cast<int>(active_panel_));

        clear_overlay_stack();
        set_active(PanelId::Home);
    }

    previous_klippy_state_ = state;
}

// ============================================================================
// EVENT CALLBACKS
// ============================================================================

void NavigationManager::backdrop_click_event_cb(lv_event_t* e) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_obj_t* current = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

    // Only respond if click was directly on backdrop (not bubbled from child)
    if (target != current) {
        return;
    }

    auto& mgr = NavigationManager::instance();

    // Only process if there's an overlay to close (stack > 1 means overlays exist)
    if (mgr.panel_stack_.size() <= 1) {
        return;
    }

    // Get click position to check if it's in the navbar area
    lv_point_t click_point;
    lv_indev_get_point(lv_indev_active(), &click_point);

    // Check if click is in navbar area and find which button was clicked
    if (mgr.navbar_widget_) {
        int32_t navbar_width = lv_obj_get_width(mgr.navbar_widget_);

        if (click_point.x < navbar_width) {
            // Click is in navbar area - find which button and trigger navigation
            const char* button_names[] = {"nav_btn_home",     "nav_btn_print_select",
                                          "nav_btn_controls", "nav_btn_filament",
                                          "nav_btn_settings", "nav_btn_advanced"};

            for (int i = 0; i < UI_PANEL_COUNT; i++) {
                lv_obj_t* btn = lv_obj_find_by_name(mgr.navbar_widget_, button_names[i]);
                if (!btn) {
                    continue;
                }

                // Check if click point is inside this button
                lv_area_t btn_area;
                lv_obj_get_coords(btn, &btn_area);

                // Simple bounds check (point in rectangle)
                if (click_point.x >= btn_area.x1 && click_point.x <= btn_area.x2 &&
                    click_point.y >= btn_area.y1 && click_point.y <= btn_area.y2) {
                    spdlog::trace(
                        "[NavigationManager] Backdrop click forwarded to navbar button {}", i);
                    // Simulate the navbar button click by sending a clicked event
                    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
                    return;
                }
            }

            // Click was in navbar area but not on a button - just close overlay
            spdlog::trace("[NavigationManager] Backdrop clicked in navbar area (no button hit)");
        }
    }

    // Regular backdrop click - close topmost overlay
    spdlog::trace("[NavigationManager] Backdrop clicked, closing topmost overlay");
    mgr.go_back();
}

void NavigationManager::nav_button_clicked_cb(lv_event_t* event) {
    LVGL_SAFE_EVENT_CB_BEGIN("nav_button_clicked_cb");

    auto& mgr = NavigationManager::instance();
    lv_event_code_t code = lv_event_get_code(event);
    int panel_id = (int)(uintptr_t)lv_event_get_user_data(event);

    spdlog::trace("[NavigationManager] nav_button_clicked_cb fired: code={}, panel_id={}, "
                  "active_panel={}",
                  static_cast<int>(code), panel_id, static_cast<int>(mgr.active_panel_));

    if (code == LV_EVENT_CLICKED) {
        // Skip if already on this panel AND no overlays are open
        if (panel_id == static_cast<int>(mgr.active_panel_) && !mgr.has_open_overlays()) {
            spdlog::info("[NavigationManager] Skipping - already on panel {} with no overlays",
                         panel_id);
            return;
        }

        // Block navigation to connection-required panels when disconnected or klippy not ready
        if (panel_requires_connection(static_cast<PanelId>(panel_id))) {
            if (!mgr.is_printer_connected()) {
                spdlog::info("[NavigationManager] Navigation to panel {} blocked - not connected",
                             panel_id);
                return;
            }
            if (!mgr.is_klippy_ready()) {
                spdlog::info(
                    "[NavigationManager] Navigation to panel {} blocked - klippy not ready",
                    panel_id);
                return;
            }
        }

        // Queue for REFR_START - guarantees we never modify widgets during render phase
        spdlog::trace("[NavigationManager] Queuing switch to panel {}", panel_id);
        helix::ui::queue_update(
            [panel_id]() { NavigationManager::instance().switch_to_panel_impl(panel_id); });
    }

    LVGL_SAFE_EVENT_CB_END();
}

void NavigationManager::switch_to_panel_impl(int panel_id) {
    spdlog::trace("[NavigationManager] switch_to_panel_impl executing for panel {}", panel_id);

    // Hide ALL visible overlay panels
    lv_obj_t* screen = lv_screen_active();
    if (screen) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
            lv_obj_t* child = lv_obj_get_child(screen, static_cast<int32_t>(i));
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }

            if (child == app_layout_widget_) {
                continue;
            }

            bool is_main_panel = false;
            for (int j = 0; j < UI_PANEL_COUNT; j++) {
                if (panel_widgets_[j] == child) {
                    is_main_panel = true;
                    break;
                }
            }

            if (!is_main_panel) {
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
                // Reset transform and opacity for potential reuse
                lv_obj_set_style_translate_x(child, 0, LV_PART_MAIN);
                lv_obj_set_style_opa(child, LV_OPA_COVER, LV_PART_MAIN);
                spdlog::trace("[NavigationManager] Hiding overlay panel {} (nav button clicked)",
                              (void*)child);
            }
        }
    }

    // Hide all main panels
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        if (panel_widgets_[i]) {
            lv_obj_add_flag(panel_widgets_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Deactivate overlays, invoke close callbacks, and clean up backdrops
    // for any overlays being cleared (e.g., settings overlay needs on_deactivate to save)
    for (lv_obj_t* panel : panel_stack_) {
        // Call on_deactivate() if this overlay has a registered instance
        auto inst_it = overlay_instances_.find(panel);
        if (inst_it != overlay_instances_.end() && inst_it->second) {
            spdlog::trace("[NavigationManager] Calling on_deactivate() for overlay {} (navbar)",
                          (void*)panel);
            inst_it->second->on_deactivate();
        }

        // Invoke close callback if registered
        auto it = overlay_close_callbacks_.find(panel);
        if (it != overlay_close_callbacks_.end()) {
            auto callback = std::move(it->second);
            overlay_close_callbacks_.erase(it);
            spdlog::trace("[NavigationManager] Invoking close callback for panel {} (navbar)",
                          (void*)panel);
            callback();
        }

        // Clean up dynamic backdrop for this overlay (if one was created)
        auto backdrop_it = overlay_backdrops_.find(panel);
        if (backdrop_it != overlay_backdrops_.end()) {
            lv_obj_del(backdrop_it->second);
            overlay_backdrops_.erase(backdrop_it);
        }
    }

    // Clear panel stack
    panel_stack_.clear();
    spdlog::trace("[NavigationManager] Panel stack cleared (nav button clicked)");

    // Hide primary backdrop since all overlays are being cleared
    if (overlay_backdrop_) {
        set_backdrop_visible(false);
    }

    // Show the clicked panel
    lv_obj_t* new_panel = panel_widgets_[static_cast<int>(panel_id)];
    if (new_panel) {
        lv_obj_remove_flag(new_panel, LV_OBJ_FLAG_HIDDEN);
        panel_stack_.push_back(new_panel);
        spdlog::trace("[NavigationManager] Showing panel {} (stack depth: {})", (void*)new_panel,
                      panel_stack_.size());
    }

    spdlog::trace("[NavigationManager] Switched to panel {}", panel_id);
    set_active((PanelId)panel_id);
    SoundManager::instance().play("nav_forward");
}

// ============================================================================
// NAVIGATION MANAGER IMPLEMENTATION
// ============================================================================

void NavigationManager::init() {
    if (subjects_initialized_) {
        spdlog::warn("[NavigationManager] Subjects already initialized");
        return;
    }

    spdlog::trace("[NavigationManager] Initializing navigation reactive subjects...");

    UI_MANAGED_SUBJECT_INT(active_panel_subject_, static_cast<int>(PanelId::Home), "active_panel",
                           subjects_);

    // Overlay backdrop starts hidden
    UI_MANAGED_SUBJECT_INT(overlay_backdrop_visible_subject_, 0, "overlay_backdrop_visible",
                           subjects_);

    active_panel_observer_ = observe_int_sync<NavigationManager>(
        &active_panel_subject_, this,
        [](NavigationManager* mgr, int value) { mgr->handle_active_panel_change(value); });

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "NavigationManager", []() { NavigationManager::instance().deinit_subjects(); });

    spdlog::trace("[NavigationManager] Navigation subjects initialized successfully");
}

void NavigationManager::init_overlay_backdrop(lv_obj_t* screen) {
    if (!screen) {
        spdlog::error("[NavigationManager] NULL screen provided to init_overlay_backdrop");
        return;
    }

    if (overlay_backdrop_) {
        spdlog::warn("[NavigationManager] Overlay backdrop already initialized");
        return;
    }

    overlay_backdrop_ = static_cast<lv_obj_t*>(lv_xml_create(screen, "overlay_backdrop", nullptr));
    if (!overlay_backdrop_) {
        spdlog::error("[NavigationManager] Failed to create overlay_backdrop from XML");
        return;
    }

    // Wire up click handler to close topmost overlay when backdrop is clicked
    lv_obj_add_event_cb(overlay_backdrop_, backdrop_click_event_cb, LV_EVENT_CLICKED, nullptr);

    spdlog::trace("[NavigationManager] Overlay backdrop created from XML successfully");
}

void NavigationManager::set_app_layout(lv_obj_t* app_layout) {
    app_layout_widget_ = app_layout;
    spdlog::trace("[NavigationManager] App layout widget registered");
}

void NavigationManager::wire_events(lv_obj_t* navbar) {
    if (!navbar) {
        spdlog::error("[NavigationManager] NULL navbar provided to wire_events");
        return;
    }

    if (!subjects_initialized_) {
        spdlog::error("[NavigationManager] Subjects not initialized! Call init() first!");
        return;
    }

    // Store navbar reference for z-order management when showing overlays
    navbar_widget_ = navbar;

    lv_obj_remove_flag(navbar, LV_OBJ_FLAG_CLICKABLE);

    const char* button_names[] = {"nav_btn_home",     "nav_btn_print_select", "nav_btn_controls",
                                  "nav_btn_filament", "nav_btn_settings",     "nav_btn_advanced"};

    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        lv_obj_t* btn = lv_obj_find_by_name(navbar, button_names[i]);

        if (!btn) {
            spdlog::trace("[NavigationManager] Nav button {} not found (may be intentional)", i);
            continue;
        }

        lv_obj_add_event_cb(btn, nav_button_clicked_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

        // Remove focus ring — nav buttons use icon color swap for active state
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_group_remove_obj(btn);
    }

    // Register connection state observer for redirect on disconnect
    connection_state_observer_ = observe_int_sync<NavigationManager>(
        get_printer_state().get_printer_connection_state_subject(), this,
        [](NavigationManager* mgr, int value) { mgr->handle_connection_state_change(value); });

    // Register klippy state observer for redirect on SHUTDOWN/ERROR
    klippy_state_observer_ = observe_int_sync<NavigationManager>(
        get_printer_state().get_klippy_state_subject(), this,
        [](NavigationManager* mgr, int value) { mgr->handle_klippy_state_change(value); });

    spdlog::trace(
        "[NavigationManager] Navigation button events wired (with connection/klippy gating)");
}

void NavigationManager::wire_status_icons(lv_obj_t* navbar) {
    if (!navbar) {
        spdlog::error("[NavigationManager] NULL navbar provided to wire_status_icons");
        return;
    }

    const char* button_names[] = {"status_btn_printer", "status_btn_network",
                                  "status_notification_icon"};
    const char* icon_names[] = {"status_printer_icon", "status_network_icon",
                                "status_notification_icon"};
    const int status_icon_count = 3;

    for (int i = 0; i < status_icon_count; i++) {
        lv_obj_t* btn = lv_obj_find_by_name(navbar, button_names[i]);
        lv_obj_t* icon_widget = lv_obj_find_by_name(navbar, icon_names[i]);

        if (!btn || !icon_widget) {
            spdlog::warn("[NavigationManager] Status icon {}: btn={}, icon={} (may not exist yet)",
                         button_names[i], (void*)btn, (void*)icon_widget);
            continue;
        }

        lv_obj_add_flag(icon_widget, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(icon_widget, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        spdlog::trace("[NavigationManager] Status icon {} wired", button_names[i]);
    }
}

void NavigationManager::set_active(PanelId panel_id) {
    if (static_cast<int>(panel_id) >= UI_PANEL_COUNT) {
        spdlog::error("[NavigationManager] Invalid panel ID: {}", static_cast<int>(panel_id));
        return;
    }

    if (panel_id == active_panel_) {
        return;
    }

    PanelId old_panel = active_panel_;

    // Update panel stack
    // IMPORTANT: Only update the base panel in the stack, preserving any overlays.
    // This fixes the bug where closing an overlay from Controls would return to Home
    // because set_active() was clearing the entire stack unconditionally.
    if (panel_widgets_[static_cast<int>(panel_id)]) {
        if (panel_stack_.empty()) {
            // Stack is empty - just push the new panel
            panel_stack_.push_back(panel_widgets_[static_cast<int>(panel_id)]);
            spdlog::trace("[NavigationManager] Panel stack initialized with panel {}",
                          static_cast<int>(panel_id));
        } else if (panel_stack_.size() == 1) {
            // Only base panel in stack - replace it
            panel_stack_[0] = panel_widgets_[static_cast<int>(panel_id)];
            spdlog::trace("[NavigationManager] Panel stack base updated to panel {}",
                          static_cast<int>(panel_id));
        } else {
            // Overlays are present - update base panel but preserve overlays
            // This handles the case where connection changes while an overlay is open
            panel_stack_[0] = panel_widgets_[static_cast<int>(panel_id)];
            spdlog::trace("[NavigationManager] Panel stack base updated to panel {}, "
                          "preserving {} overlays",
                          static_cast<int>(panel_id), panel_stack_.size() - 1);
        }
    }

    // Call on_deactivate() BEFORE state update
    if (panel_instances_[static_cast<int>(old_panel)]) {
        spdlog::trace("[NavigationManager] Calling on_deactivate() for panel {}",
                      static_cast<int>(old_panel));
        panel_instances_[static_cast<int>(old_panel)]->on_deactivate();
    }

    // Update state
    lv_subject_set_int(&active_panel_subject_, static_cast<int>(panel_id));
    active_panel_ = panel_id;

    // Call on_activate() AFTER state update
    if (panel_instances_[static_cast<int>(panel_id)]) {
        spdlog::trace("[NavigationManager] Calling on_activate() for panel {}",
                      static_cast<int>(panel_id));
        panel_instances_[static_cast<int>(panel_id)]->on_activate();
    }
}

PanelId NavigationManager::get_active() const {
    return active_panel_;
}

void NavigationManager::set_panels(lv_obj_t** panels) {
    if (!panels) {
        spdlog::error("[NavigationManager] NULL panels array provided");
        return;
    }

    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        panel_widgets_[i] = panels[i];
    }

    // Hide all panels except active one
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        if (panel_widgets_[i]) {
            if (i == static_cast<int>(active_panel_)) {
                lv_obj_remove_flag(panel_widgets_[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(panel_widgets_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Initialize panel stack
    panel_stack_.clear();
    if (panel_widgets_[static_cast<int>(active_panel_)]) {
        panel_stack_.push_back(panel_widgets_[static_cast<int>(active_panel_)]);
        spdlog::trace("[NavigationManager] Panel stack initialized with active panel {}",
                      (void*)panel_widgets_[static_cast<int>(active_panel_)]);
    }

    spdlog::trace("[NavigationManager] Panel widgets registered for show/hide management");
}

void NavigationManager::register_panel_instance(PanelId id, PanelBase* panel) {
    if (static_cast<int>(id) >= UI_PANEL_COUNT) {
        spdlog::error("[NavigationManager] Invalid panel ID for registration: {}",
                      static_cast<int>(id));
        return;
    }
    panel_instances_[static_cast<int>(id)] = panel;
    spdlog::trace("[NavigationManager] Registered panel instance for ID {}", static_cast<int>(id));
}

void NavigationManager::activate_initial_panel() {
    if (panel_instances_[static_cast<int>(active_panel_)]) {
        spdlog::trace("[NavigationManager] Activating initial panel {}",
                      static_cast<int>(active_panel_));
        panel_instances_[static_cast<int>(active_panel_)]->on_activate();
    }
}

void NavigationManager::register_overlay_instance(lv_obj_t* widget, IPanelLifecycle* overlay) {
    if (!widget) {
        spdlog::error("[NavigationManager] Cannot register overlay with NULL widget");
        return;
    }
    overlay_instances_[widget] = overlay;
    if (overlay) {
        spdlog::trace("[NavigationManager] Registered overlay instance {} for widget {}",
                      overlay->get_name(), (void*)widget);
    } else {
        spdlog::trace("[NavigationManager] Registered overlay widget {} (no lifecycle)",
                      (void*)widget);
    }
}

void NavigationManager::unregister_overlay_instance(lv_obj_t* widget) {
    auto it = overlay_instances_.find(widget);
    if (it != overlay_instances_.end()) {
        spdlog::trace("[NavigationManager] Unregistered overlay instance for widget {}",
                      (void*)widget);
        overlay_instances_.erase(it);
    }
}

void NavigationManager::push_overlay(lv_obj_t* overlay_panel, bool hide_previous) {
    if (!overlay_panel) {
        spdlog::error("[NavigationManager] Cannot push NULL overlay panel");
        return;
    }

    // Always queue - this is the safest pattern for overlay operations
    // which can be triggered from various contexts (events, observers, etc.)
    helix::ui::queue_update([overlay_panel, hide_previous]() {
        auto& mgr = NavigationManager::instance();

        // Check for duplicate push
        if (std::find(mgr.panel_stack_.begin(), mgr.panel_stack_.end(), overlay_panel) !=
            mgr.panel_stack_.end()) {
            spdlog::warn("[NavigationManager] Overlay {} already in stack, ignoring duplicate push",
                         (void*)overlay_panel);
            return;
        }

        bool is_first_overlay = (mgr.panel_stack_.size() == 1);

        // Track overlay opens for telemetry panel_usage event
        TelemetryManager::instance().notify_overlay_opened();

        // Lifecycle: Deactivate what's currently visible before showing new overlay
        if (is_first_overlay) {
            // Deactivate main panel when first overlay covers it
            if (mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]) {
                spdlog::trace("[NavigationManager] Deactivating main panel {} for overlay",
                              static_cast<int>(mgr.active_panel_));
                mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]->on_deactivate();
            }
        } else {
            // Deactivate previous overlay if stacking
            lv_obj_t* prev_overlay = mgr.panel_stack_.back();
            auto it = mgr.overlay_instances_.find(prev_overlay);
            if (it != mgr.overlay_instances_.end() && it->second) {
                spdlog::trace("[NavigationManager] Deactivating previous overlay {}",
                              it->second->get_name());
                it->second->on_deactivate();
            }
        }

        // Optionally hide current top panel
        if (hide_previous && !mgr.panel_stack_.empty()) {
            lv_obj_t* current_top = mgr.panel_stack_.back();
            lv_obj_add_flag(current_top, LV_OBJ_FLAG_HIDDEN);
        }

        // Create backdrop
        lv_obj_t* screen = lv_obj_get_screen(overlay_panel);
        if (screen) {
            if (is_first_overlay && mgr.overlay_backdrop_) {
                mgr.set_backdrop_visible(true);
                lv_obj_move_foreground(mgr.overlay_backdrop_);
            }
            // Nested overlays do NOT get their own backdrop — the primary
            // backdrop already provides dimming for the entire overlay stack.
            // Secondary/deeper settings panels (e.g. theme editor inside
            // display settings) should not add additional darkening layers.
        }

        // Show overlay
        lv_obj_remove_flag(overlay_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(overlay_panel);
        mgr.panel_stack_.push_back(overlay_panel);
        mgr.overlay_animate_slide_in(overlay_panel);

        // Lifecycle: Activate new overlay
        auto it = mgr.overlay_instances_.find(overlay_panel);
        if (it == mgr.overlay_instances_.end()) {
            spdlog::warn("[NavigationManager] Overlay {} pushed without lifecycle registration",
                         (void*)overlay_panel);
        } else if (it->second) {
            spdlog::trace("[NavigationManager] Activating overlay {}", it->second->get_name());
            it->second->on_activate();
        }

        SoundManager::instance().play("nav_forward");
        spdlog::trace("[NavigationManager] Pushed overlay {} (stack: {})", (void*)overlay_panel,
                      mgr.panel_stack_.size());
    });
}

void NavigationManager::push_overlay_zoom_from(lv_obj_t* overlay_panel, lv_area_t source_rect) {
    if (!overlay_panel) {
        spdlog::error("[NavigationManager] Cannot push NULL overlay panel");
        return;
    }

    // Queue the push operation (same pattern as push_overlay)
    helix::ui::queue_update([overlay_panel, source_rect]() {
        auto& mgr = NavigationManager::instance();

        // Store source rect for reverse animation on go_back (must be on UI thread)
        mgr.zoom_source_rects_[overlay_panel] = source_rect;

        // Check for duplicate push
        if (std::find(mgr.panel_stack_.begin(), mgr.panel_stack_.end(), overlay_panel) !=
            mgr.panel_stack_.end()) {
            spdlog::warn("[NavigationManager] Overlay {} already in stack, ignoring duplicate push",
                         (void*)overlay_panel);
            return;
        }

        bool is_first_overlay = (mgr.panel_stack_.size() == 1);

        // Lifecycle: Deactivate what's currently visible
        if (is_first_overlay) {
            if (mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]) {
                mgr.panel_instances_[static_cast<int>(mgr.active_panel_)]->on_deactivate();
            }
        } else {
            lv_obj_t* prev_overlay = mgr.panel_stack_.back();
            auto it = mgr.overlay_instances_.find(prev_overlay);
            if (it != mgr.overlay_instances_.end() && it->second) {
                it->second->on_deactivate();
            }
        }

        // Hide current top panel
        if (!mgr.panel_stack_.empty()) {
            lv_obj_t* current_top = mgr.panel_stack_.back();
            lv_obj_add_flag(current_top, LV_OBJ_FLAG_HIDDEN);
        }

        // Create backdrop
        lv_obj_t* screen = lv_obj_get_screen(overlay_panel);
        if (screen) {
            if (is_first_overlay && mgr.overlay_backdrop_) {
                mgr.set_backdrop_visible(true);
                lv_obj_move_foreground(mgr.overlay_backdrop_);
            } else if (!is_first_overlay) {
                lv_obj_t* backdrop =
                    static_cast<lv_obj_t*>(lv_xml_create(screen, "overlay_backdrop", nullptr));
                if (backdrop) {
                    mgr.overlay_backdrops_[overlay_panel] = backdrop;
                    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_move_foreground(backdrop);
                    lv_obj_add_event_cb(backdrop, backdrop_click_event_cb, LV_EVENT_CLICKED,
                                        nullptr);
                }
            }
        }

        // Show overlay with zoom animation instead of slide
        lv_obj_remove_flag(overlay_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(overlay_panel);
        mgr.panel_stack_.push_back(overlay_panel);
        mgr.overlay_animate_zoom_in(overlay_panel, source_rect);

        // Lifecycle: Activate new overlay
        auto it = mgr.overlay_instances_.find(overlay_panel);
        if (it == mgr.overlay_instances_.end()) {
            spdlog::warn("[NavigationManager] Overlay {} pushed without lifecycle registration",
                         (void*)overlay_panel);
        } else if (it->second) {
            it->second->on_activate();
        }

        SoundManager::instance().play("nav_forward");
        spdlog::trace("[NavigationManager] Pushed overlay {} with zoom (stack: {})",
                      (void*)overlay_panel, mgr.panel_stack_.size());
    });
}

void NavigationManager::register_overlay_close_callback(lv_obj_t* overlay_panel,
                                                        OverlayCloseCallback callback) {
    if (!overlay_panel || !callback) {
        return;
    }
    overlay_close_callbacks_[overlay_panel] = std::move(callback);
    spdlog::trace("[NavigationManager] Registered close callback for overlay {}",
                  (void*)overlay_panel);
}

void NavigationManager::unregister_overlay_close_callback(lv_obj_t* overlay_panel) {
    auto it = overlay_close_callbacks_.find(overlay_panel);
    if (it != overlay_close_callbacks_.end()) {
        overlay_close_callbacks_.erase(it);
        spdlog::trace("[NavigationManager] Unregistered close callback for overlay {}",
                      (void*)overlay_panel);
    }
}

bool NavigationManager::go_back() {
    helix::ui::queue_update([]() {
        auto& mgr = NavigationManager::instance();
        spdlog::trace("[NavigationManager] go_back executing, stack depth: {}",
                      mgr.panel_stack_.size());

        // Dismiss keyboard before navigation to restore screen position
        if (KeyboardManager::instance().is_visible()) {
            KeyboardManager::instance().hide();
        }

        lv_obj_t* current_top = mgr.panel_stack_.empty() ? nullptr : mgr.panel_stack_.back();

        // Check if current top is an overlay
        bool is_overlay = false;
        if (current_top) {
            is_overlay = true;
            for (int j = 0; j < UI_PANEL_COUNT; j++) {
                if (mgr.panel_widgets_[j] == current_top) {
                    is_overlay = false;
                    break;
                }
            }
        }

        // Lifecycle: Deactivate the closing overlay before animation
        if (is_overlay && current_top) {
            // Remove overlay from focus group BEFORE closing to prevent LVGL from
            // auto-focusing the next element (which triggers scroll-on-focus)
            lv_group_t* group = lv_group_get_default();
            if (group) {
                lv_group_remove_obj(current_top);
            }

            auto it = mgr.overlay_instances_.find(current_top);
            if (it != mgr.overlay_instances_.end() && it->second) {
                spdlog::trace("[NavigationManager] Deactivating closing overlay {}",
                              it->second->get_name());
                it->second->on_deactivate();
            }
        }

        // Animate out if overlay (zoom-out for zoomed overlays, slide-out otherwise)
        if (is_overlay && current_top) {
            auto zoom_it = mgr.zoom_source_rects_.find(current_top);
            if (zoom_it != mgr.zoom_source_rects_.end()) {
                lv_area_t source_rect = zoom_it->second;
                mgr.zoom_source_rects_.erase(zoom_it);
                mgr.overlay_animate_zoom_out(current_top, source_rect);
            } else {
                mgr.overlay_animate_slide_out(current_top);
            }
            SoundManager::instance().play("nav_back");
        }

        // Determine the previous panel (what will be visible after pop)
        lv_obj_t* previous_panel = nullptr;
        if (mgr.panel_stack_.size() >= 2) {
            previous_panel = mgr.panel_stack_[mgr.panel_stack_.size() - 2];
        }

        // Hide stale overlays (but skip current_top, previous panel, and system widgets)
        lv_obj_t* screen = lv_screen_active();
        if (screen) {
            for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
                lv_obj_t* child = lv_obj_get_child(screen, static_cast<int32_t>(i));
                if (child == mgr.app_layout_widget_ || child == mgr.overlay_backdrop_ ||
                    child == current_top || child == previous_panel) {
                    continue;
                }
                bool is_main = false;
                for (int j = 0; j < UI_PANEL_COUNT; j++) {
                    if (mgr.panel_widgets_[j] == child) {
                        is_main = true;
                        break;
                    }
                }
                if (!is_main && !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
                    // [Perf] Skip style resets if already at defaults — each set_style
                    // call triggers a style invalidation, costing 4-20ms total on ARM.
                    bool needs_reset =
                        lv_obj_get_style_translate_x(child, LV_PART_MAIN) != 0 ||
                        lv_obj_get_style_translate_y(child, LV_PART_MAIN) != 0 ||
                        lv_obj_get_style_transform_scale_x(child, LV_PART_MAIN) != 256 ||
                        lv_obj_get_style_transform_scale_y(child, LV_PART_MAIN) != 256 ||
                        lv_obj_get_style_opa(child, LV_PART_MAIN) != LV_OPA_COVER;
                    if (needs_reset) {
                        lv_obj_set_style_translate_x(child, 0, LV_PART_MAIN);
                        lv_obj_set_style_translate_y(child, 0, LV_PART_MAIN);
                        lv_obj_set_style_transform_scale(child, 256, LV_PART_MAIN);
                        lv_obj_set_style_opa(child, LV_OPA_COVER, LV_PART_MAIN);
                    }
                }
            }
        }

        // Pop and clean up backdrop
        if (!mgr.panel_stack_.empty()) {
            lv_obj_t* popped = mgr.panel_stack_.back();
            mgr.panel_stack_.pop_back();
            auto it = mgr.overlay_backdrops_.find(popped);
            if (it != mgr.overlay_backdrops_.end()) {
                lv_obj_del(it->second);
                mgr.overlay_backdrops_.erase(it);
            }
        }

        // Hide backdrop if no more overlays
        if (mgr.panel_stack_.size() <= 1 && mgr.overlay_backdrop_) {
            mgr.set_backdrop_visible(false);
        }

        // Fallback to home if empty
        if (mgr.panel_stack_.empty()) {
            spdlog::trace("[NavigationManager] go_back stack empty, falling back to HOME");
            for (int i = 0; i < UI_PANEL_COUNT; i++) {
                if (mgr.panel_widgets_[i])
                    lv_obj_add_flag(mgr.panel_widgets_[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (mgr.panel_widgets_[static_cast<int>(PanelId::Home)]) {
                lv_obj_remove_flag(mgr.panel_widgets_[static_cast<int>(PanelId::Home)],
                                   LV_OBJ_FLAG_HIDDEN);
                mgr.panel_stack_.push_back(mgr.panel_widgets_[static_cast<int>(PanelId::Home)]);
                mgr.active_panel_ = PanelId::Home;
                lv_subject_set_int(&mgr.active_panel_subject_, static_cast<int>(PanelId::Home));
            }
            return;
        }

        // Show previous panel
        lv_obj_t* prev = mgr.panel_stack_.back();
        for (int i = 0; i < UI_PANEL_COUNT; i++) {
            if (mgr.panel_widgets_[i] == prev) {
                for (int j = 0; j < UI_PANEL_COUNT; j++) {
                    if (j != i && mgr.panel_widgets_[j])
                        lv_obj_add_flag(mgr.panel_widgets_[j], LV_OBJ_FLAG_HIDDEN);
                }
                mgr.active_panel_ = static_cast<PanelId>(i);
                lv_subject_set_int(&mgr.active_panel_subject_, i);
                break;
            }
        }
        lv_obj_remove_flag(prev, LV_OBJ_FLAG_HIDDEN);
    });
    return true;
}

bool NavigationManager::is_panel_in_stack(lv_obj_t* panel) const {
    if (!panel) {
        return false;
    }
    return std::find(panel_stack_.begin(), panel_stack_.end(), panel) != panel_stack_.end();
}

bool NavigationManager::has_open_overlays() const {
    return !overlay_instances_.empty() || panel_stack_.size() > 1;
}

void NavigationManager::shutdown() {
    spdlog::trace("[NavigationManager] Shutting down...");
    shutting_down_ = true;

    // Deactivate any overlays in the stack
    for (lv_obj_t* overlay_widget : panel_stack_) {
        auto it = overlay_instances_.find(overlay_widget);
        if (it != overlay_instances_.end() && it->second) {
            spdlog::trace("[NavigationManager] Deactivating overlay: {}", it->second->get_name());
            it->second->on_deactivate();
        }
    }

    // Clear overlay registry
    // Note: The actual panel objects are destroyed via StaticPanelRegistry,
    // we just clear our tracking references here
    overlay_instances_.clear();

    // Clear panel instances
    for (auto& panel : panel_instances_) {
        panel = nullptr;
    }

    // Clear panel stack and zoom state
    panel_stack_.clear();
    zoom_source_rects_.clear();

    spdlog::trace("[NavigationManager] Shutdown complete");
}

void NavigationManager::set_backdrop_visible(bool visible) {
    if (!subjects_initialized_) {
        spdlog::warn(
            "[NavigationManager] Subjects not initialized, cannot set backdrop visibility");
        return;
    }

    lv_subject_set_int(&overlay_backdrop_visible_subject_, visible ? 1 : 0);
    spdlog::trace("[NavigationManager] Overlay backdrop visibility set to: {}", visible);
}

void NavigationManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // Reset observer guards BEFORE deiniting subjects - they hold references
    // to subjects that will become invalid. Also handles observers attached
    // to external subjects (PrinterState) that may be reset separately.
    active_panel_observer_.reset();
    connection_state_observer_.reset();
    klippy_state_observer_.reset();

    subjects_.deinit_all();

    // Reset widget pointers - they become invalid when LVGL is reinitialized
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        panel_widgets_[i] = nullptr;
        panel_instances_[i] = nullptr;
    }
    overlay_instances_.clear();
    overlay_close_callbacks_.clear();
    overlay_backdrops_.clear();
    zoom_source_rects_.clear();
    panel_stack_.clear();
    app_layout_widget_ = nullptr;
    overlay_backdrop_ = nullptr;
    navbar_widget_ = nullptr;
    active_panel_ = PanelId::Home;
    previous_connection_state_ = -1;
    previous_klippy_state_ = -1;

    subjects_initialized_ = false;
    spdlog::trace("[NavigationManager] Subjects deinitialized");
}
