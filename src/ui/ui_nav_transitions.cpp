// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_nav_transitions.h"

#include "ui_effects.h"
#include "ui_nav_manager.h"
#include "ui_nav_switch_trace.h"
#include "ui_utils.h"

#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <spdlog/spdlog.h>

namespace helix::nav {

lv_obj_t* make_loading_scrim() {
    lv_obj_t* pill = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(pill, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(pill, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(pill, 12, LV_PART_MAIN);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* lbl = lv_label_create(pill);
    lv_label_set_text(lbl, lv_tr("Loading..."));
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(pill);
    return pill;
}

void reset_overlay_transform(lv_obj_t* obj) {
    lv_obj_set_style_translate_x(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_translate_y(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(obj, 256, LV_PART_MAIN);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
}

#if defined(HELIX_PLATFORM_ESP32)
NavTransitionScrim::NavTransitionScrim(bool& active, bool needed)
    : active_(active), owns_(!active && needed) {
    if (owns_) {
        active_ = true;
        scrim_ = make_loading_scrim();
        lv_refr_now(lv_display_get_default());
        SwitchTrace::mark("scrim");
    }
}

NavTransitionScrim::~NavTransitionScrim() {
    if (owns_) {
        SwitchTrace::mark("tail");
        lv_refr_now(lv_display_get_default());
        SwitchTrace::mark("paint");
        helix::ui::safe_delete_deferred(scrim_);
        active_ = false;
    }
}
#endif

} // namespace helix::nav

using namespace helix;
using helix::nav::reset_overlay_transform;

void NavigationManager::overlay_slide_out_complete_cb(lv_anim_t* anim) {
    if (NavigationManager::instance().is_shutting_down()) {
        return; // Shutdown in progress — widget may be freed
    }
    // Deleting a widget cancels its animations, so anim->var is alive here.
    lv_obj_t* panel = static_cast<lv_obj_t*>(anim->var);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    reset_overlay_transform(panel);
    spdlog::trace("[NavigationManager] Overlay slide+fade-out complete, panel {} hidden",
                  (void*)panel);

    // Animation callbacks fire from inside lv_timer_handler() →
    // lv_display_refr_timer(), and deleting objects mid-layout is a
    // use-after-free in layout_update_core → lv_obj_scrollbar_invalidate, so the
    // close callback waits for the next tick.
    auto& mgr = NavigationManager::instance();
    mgr.retire_overlay(panel);

    // Lifecycle: activate what's now visible. go_back() consumes the latch
    // itself before it returns, so this is the fallback for any close path that
    // armed it without reaching that point — a no-op once consumed.
    mgr.activate_restored_target();
}

// Overlays enter from and exit to the right edge in every orientation.
static void overlay_translate_x(void* obj, int32_t v) {
    lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), v, LV_PART_MAIN);
}

static int32_t overlay_slide_offset(lv_obj_t* panel, int32_t fallback) {
    const int32_t width = lv_obj_get_width(panel);
    return width != 0 ? width : fallback;
}

void NavigationManager::overlay_animate_slide_in(lv_obj_t* panel) {
    const int32_t offset = overlay_slide_offset(panel, kOverlaySlideOffset);

    // Final state when animations are off, or on a tier where a near-full-screen
    // slide plus fade costs more than the frame budget
    if (!DisplaySettingsManager::instance().get_animations_enabled() ||
        !helix::ui::full_style_effects_active()) {
        lv_obj_set_style_translate_x(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        spdlog::trace("[NavigationManager] Animations disabled - showing overlay instantly");
        return;
    }

    // Set initial state: off-screen and transparent
    lv_obj_set_style_translate_x(panel, offset, LV_PART_MAIN);
    lv_obj_set_style_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_anim_t slide_anim;
    lv_anim_init(&slide_anim);
    lv_anim_set_var(&slide_anim, panel);
    lv_anim_set_values(&slide_anim, offset, 0);
    lv_anim_set_duration(&slide_anim, kOverlayAnimDurationMs);
    lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&slide_anim, overlay_translate_x);
    lv_anim_start(&slide_anim);

    // Fade animation: opacity from transparent to opaque (runs simultaneously)
    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, panel);
    lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade_anim, kOverlayAnimDurationMs);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);

    spdlog::trace("[NavigationManager] Started slide+fade-in for panel {} (offset={})",
                  (void*)panel, offset);
}

void NavigationManager::overlay_animate_slide_out(lv_obj_t* panel) {
    // Disable clicks immediately to prevent interaction during animation
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Same rule as overlay_animate_slide_in: hide immediately and retire
    if (!DisplaySettingsManager::instance().get_animations_enabled() ||
        !helix::ui::full_style_effects_active()) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        reset_overlay_transform(panel);
        spdlog::trace("[NavigationManager] Animations disabled - hiding overlay instantly");

        retire_overlay(panel);

        // Deliberately NO activation here. This runs from inside go_back(),
        // which un-hides the restored panel *after* this returns; an
        // on_activate() that navigates (PrintSelectPanel's Print-Last flow
        // calls set_active(Home)) would be silently undone by that un-hide.
        // go_back() owns the restored panel's activation via
        // activate_restored_target(), fired once, below its un-hide.
        return;
    }

    const int32_t offset = overlay_slide_offset(panel, kOverlaySlideOffset);

    lv_anim_t slide_anim;
    lv_anim_init(&slide_anim);
    lv_anim_set_var(&slide_anim, panel);
    lv_anim_set_values(&slide_anim, 0, offset);
    lv_anim_set_duration(&slide_anim, kOverlayAnimDurationMs);
    lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&slide_anim, overlay_translate_x);
    lv_anim_set_completed_cb(&slide_anim, overlay_slide_out_complete_cb);
    lv_anim_start(&slide_anim);

    // Fade animation: opacity from opaque to transparent (runs simultaneously)
    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, panel);
    lv_anim_set_values(&fade_anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade_anim, kOverlayAnimDurationMs);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);

    spdlog::trace("[NavigationManager] Started slide+fade-out for panel {} (offset={})",
                  (void*)panel, offset);
}
