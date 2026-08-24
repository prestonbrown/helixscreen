// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_touch.h"

#include "ui_nav_manager.h"

#include "input_settings_manager.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::settings {

static std::unique_ptr<TouchSettingsOverlay> g_touch_settings_overlay;

TouchSettingsOverlay& get_touch_settings_overlay() {
    if (!g_touch_settings_overlay) {
        g_touch_settings_overlay = std::make_unique<TouchSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "TouchSettingsOverlay", []() { g_touch_settings_overlay.reset(); });
    }
    return *g_touch_settings_overlay;
}

TouchSettingsOverlay::TouchSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

TouchSettingsOverlay::~TouchSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

void TouchSettingsOverlay::init_subjects() {
    // All bound subjects (settings_debug_touches, settings_scroll_guard,
    // settings_jitter_threshold, settings_scroll_limit) are owned by
    // InputSettingsManager and registered globally at startup.
    subjects_initialized_ = true;
}

void TouchSettingsOverlay::register_callbacks() {
    // All row callbacks are registered globally by SettingsPanel so the
    // top-level Touch Calibration entry can share them.
}

lv_obj_t* TouchSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        return overlay_root_;
    }
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_touch_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);
    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void TouchSettingsOverlay::show(lv_obj_t* parent_screen) {
    parent_screen_ = parent_screen;
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }
    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }
    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

void TouchSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    init_input_sliders();
}

void TouchSettingsOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
}

// Sliders capture their value at XML construction from the static `value`
// prop, so push the persisted value on every activate. Toggle rows bind to
// subjects directly and need no manual sync.
void TouchSettingsOverlay::init_input_sliders() {
    if (!overlay_root_) {
        return;
    }

    auto& input = helix::InputSettingsManager::instance();

    auto sync_slider = [this](const char* row_name, int value) {
        lv_obj_t* row = lv_obj_find_by_name(overlay_root_, row_name);
        if (!row) {
            return;
        }
        if (lv_obj_t* slider = lv_obj_find_by_name(row, "slider")) {
            lv_slider_set_value(slider, value, LV_ANIM_OFF);
        }
        if (lv_obj_t* value_label = lv_obj_find_by_name(row, "value_label")) {
            lv_label_set_text_fmt(value_label, "%d", value);
        }
    };

    sync_slider("row_jitter_threshold", input.get_jitter_threshold());
    sync_slider("row_scroll_limit", input.get_scroll_limit());
    sync_slider("row_long_press_time", input.get_long_press_time());
}

} // namespace helix::settings
