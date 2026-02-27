// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "led_widget.h"

#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "led/led_controller.h"
#include "led/ui_led_control_overlay.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

void register_led_widget() {
    register_widget_factory("led", []() {
        auto& ps = get_printer_state();
        auto* api = PanelWidgetManager::instance().shared_resource<MoonrakerAPI>();
        return std::make_unique<LedWidget>(ps, api);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "light_toggle_cb", LedWidget::light_toggle_cb);
    lv_xml_register_event_cb(nullptr, "light_long_press_cb", LedWidget::light_long_press_cb);
}

LedWidget::LedWidget(PrinterState& printer_state, MoonrakerAPI* api)
    : printer_state_(printer_state), api_(api) {}

LedWidget::~LedWidget() {
    detach();
}

void LedWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    *alive_ = true;

    if (!widget_obj_) {
        return;
    }

    lv_obj_set_user_data(widget_obj_, this);

    // Set user_data on the light_button (where event_cb is registered in XML)
    // so the callback can recover this widget instance via lv_obj_get_user_data()
    auto* light_button = lv_obj_find_by_name(widget_obj_, "light_button");
    if (light_button) {
        lv_obj_set_user_data(light_button, this);
    }

    // Find light icon for dynamic brightness/color updates
    light_icon_ = lv_obj_find_by_name(widget_obj_, "light_icon");
    if (light_icon_) {
        spdlog::debug("[LedWidget] Found light_icon for dynamic brightness/color");
        update_light_icon();
    }

    // Observe led_config_version to rebind when LED discovery or settings change.
    // This fires immediately on add (triggering bind_led), so no separate init call needed.
    std::weak_ptr<bool> weak_alive = alive_;
    auto& led_ctrl = helix::led::LedController::instance();
    led_version_observer_ =
        helix::ui::observe_int_sync<LedWidget>(led_ctrl.get_led_config_version_subject(), this,
                                               [weak_alive](LedWidget* self, int /*version*/) {
                                                   if (weak_alive.expired())
                                                       return;
                                                   self->bind_led();
                                               });

    spdlog::debug("[LedWidget] Attached");
}

void LedWidget::detach() {
    *alive_ = false;

    // Nullify widget pointers BEFORE resetting observers
    if (widget_obj_) {
        auto* light_button = lv_obj_find_by_name(widget_obj_, "light_button");
        if (light_button) {
            lv_obj_set_user_data(light_button, nullptr);
        }
        lv_obj_set_user_data(widget_obj_, nullptr);
    }
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    light_icon_ = nullptr;
    led_control_panel_ = nullptr;

    led_version_observer_.reset();
    led_state_observer_.reset();
    led_brightness_observer_.reset();

    spdlog::debug("[LedWidget] Detached");
}

void LedWidget::bind_led() {
    // Reset existing per-LED observers before rebinding
    led_state_observer_.reset();
    led_brightness_observer_.reset();

    auto& led_ctrl = helix::led::LedController::instance();
    const auto& strips = led_ctrl.selected_strips();
    if (!strips.empty()) {
        printer_state_.set_tracked_led(strips.front());

        // Create state/brightness observers
        std::weak_ptr<bool> weak_alive = alive_;
        led_state_observer_ = helix::ui::observe_int_sync<LedWidget>(
            printer_state_.get_led_state_subject(), this, [weak_alive](LedWidget* self, int state) {
                if (weak_alive.expired())
                    return;
                self->on_led_state_changed(state);
            });
        led_brightness_observer_ = helix::ui::observe_int_sync<LedWidget>(
            printer_state_.get_led_brightness_subject(), this,
            [weak_alive](LedWidget* self, int /*brightness*/) {
                if (weak_alive.expired())
                    return;
                self->update_light_icon();
            });

        spdlog::info("[LedWidget] Bound to LED: {}", strips.front());
    } else {
        printer_state_.set_tracked_led("");
        spdlog::debug("[LedWidget] LED binding cleared (no strips selected)");
    }

    update_light_icon();
}

void LedWidget::handle_light_toggle() {
    // Suppress click that follows a long-press gesture
    if (light_long_pressed_) {
        light_long_pressed_ = false;
        spdlog::debug("[LedWidget] Light click suppressed (follows long-press)");
        return;
    }

    spdlog::info("[LedWidget] Light button clicked");

    auto& led_ctrl = helix::led::LedController::instance();
    const auto& strips = led_ctrl.selected_strips();
    if (strips.empty()) {
        spdlog::warn("[LedWidget] Light toggle called but no LED configured");
        return;
    }

    led_ctrl.light_toggle();

    if (led_ctrl.light_state_trackable()) {
        light_on_ = led_ctrl.light_is_on();
        update_light_icon();
    } else {
        flash_light_icon();
    }
}

void LedWidget::handle_light_long_press() {
    spdlog::info("[LedWidget] Light long-press: opening LED control overlay");

    // Lazy-create overlay on first access
    if (!led_control_panel_ && parent_screen_) {
        auto& overlay = get_led_control_overlay();

        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();
        overlay.set_api(api_);

        led_control_panel_ = overlay.create(parent_screen_);
        if (!led_control_panel_) {
            spdlog::error("[LedWidget] Failed to load LED control overlay");
            return;
        }

        NavigationManager::instance().register_overlay_instance(led_control_panel_, &overlay);
    }

    if (led_control_panel_) {
        light_long_pressed_ = true; // Suppress the click that follows long-press
        get_led_control_overlay().set_api(api_);
        NavigationManager::instance().push_overlay(led_control_panel_);
    }
}

void LedWidget::update_light_icon() {
    if (!light_icon_) {
        return;
    }

    // Get current brightness
    int brightness = lv_subject_get_int(printer_state_.get_led_brightness_subject());

    // Set icon based on brightness level
    const char* icon_name = ui_brightness_to_lightbulb_icon(brightness);
    ui_icon_set_source(light_icon_, icon_name);

    // Calculate icon color from LED RGBW values
    if (brightness == 0) {
        // OFF state - use muted gray from design tokens
        ui_icon_set_color(light_icon_, theme_manager_get_color("light_icon_off"), LV_OPA_COVER);
    } else {
        // Get RGB values from PrinterState
        int r = lv_subject_get_int(printer_state_.get_led_r_subject());
        int g = lv_subject_get_int(printer_state_.get_led_g_subject());
        int b = lv_subject_get_int(printer_state_.get_led_b_subject());
        int w = lv_subject_get_int(printer_state_.get_led_w_subject());

        lv_color_t icon_color;
        // If white channel dominant or RGB near white, use gold from design tokens
        if (w > std::max({r, g, b}) || (r > 200 && g > 200 && b > 200)) {
            icon_color = theme_manager_get_color("light_icon_on");
        } else {
            // Use actual LED color, boost if too dark for visibility
            int max_val = std::max({r, g, b});
            if (max_val < 128 && max_val > 0) {
                float scale = 128.0f / static_cast<float>(max_val);
                icon_color =
                    lv_color_make(static_cast<uint8_t>(std::min(255, static_cast<int>(r * scale))),
                                  static_cast<uint8_t>(std::min(255, static_cast<int>(g * scale))),
                                  static_cast<uint8_t>(std::min(255, static_cast<int>(b * scale))));
            } else {
                icon_color = lv_color_make(static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                                           static_cast<uint8_t>(b));
            }
        }

        ui_icon_set_color(light_icon_, icon_color, LV_OPA_COVER);
    }

    spdlog::trace("[LedWidget] Light icon: {} at {}%", icon_name, brightness);
}

void LedWidget::flash_light_icon() {
    if (!light_icon_)
        return;

    // Flash gold briefly then fade back to muted
    ui_icon_set_color(light_icon_, theme_manager_get_color("light_icon_on"), LV_OPA_COVER);

    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        // No animations -- the next status update will restore the icon naturally
        return;
    }

    // Animate opacity 255 -> 0 then restore to muted on completion
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, light_icon_);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&anim, 300);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value), 0);
    });
    lv_anim_set_completed_cb(&anim, [](lv_anim_t* a) {
        auto* icon = static_cast<lv_obj_t*>(a->var);
        lv_obj_set_style_opa(icon, LV_OPA_COVER, 0);
        ui_icon_set_color(icon, theme_manager_get_color("light_icon_off"), LV_OPA_COVER);
    });
    lv_anim_start(&anim);

    spdlog::debug("[LedWidget] Flash light icon (TOGGLE macro, state unknown)");
}

void LedWidget::on_led_state_changed(int state) {
    auto& led_ctrl = helix::led::LedController::instance();
    if (led_ctrl.light_state_trackable()) {
        light_on_ = (state != 0);
        led_ctrl.sync_light_state(light_on_);
        spdlog::debug("[LedWidget] LED state changed: {} (from PrinterState)",
                      light_on_ ? "ON" : "OFF");
        update_light_icon();
    } else {
        spdlog::debug("[LedWidget] LED state changed but not trackable (TOGGLE macro mode)");
    }
}

void LedWidget::light_toggle_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedWidget] light_toggle_cb");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<LedWidget*>(lv_obj_get_user_data(target));
    if (self) {
        self->handle_light_toggle();
    } else {
        spdlog::warn("[LedWidget] light_toggle_cb: could not recover widget instance");
    }
    LVGL_SAFE_EVENT_CB_END();
}

void LedWidget::light_long_press_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedWidget] light_long_press_cb");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<LedWidget*>(lv_obj_get_user_data(target));
    if (self) {
        self->handle_light_long_press();
    } else {
        spdlog::warn("[LedWidget] light_long_press_cb: could not recover widget instance");
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
