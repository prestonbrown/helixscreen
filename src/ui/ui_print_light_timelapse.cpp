// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_light_timelapse.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "i_moonraker_api.h"
#include "led/led_controller.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observe_language.h"
#include "observer_factory.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <memory>
#include <utility>

// ============================================================================
// GLOBAL INSTANCE ACCESSOR
// ============================================================================

static PrintLightTimelapseControls* g_light_timelapse_controls = nullptr;

PrintLightTimelapseControls& get_global_light_timelapse_controls() {
    if (!g_light_timelapse_controls) {
        spdlog::error("[PrintLightTimelapseControls] Global instance not set!");
        // This will crash, but it's a programming error that should never happen
        static PrintLightTimelapseControls fallback;
        return fallback;
    }
    return *g_light_timelapse_controls;
}

void set_global_light_timelapse_controls(PrintLightTimelapseControls* instance) {
    g_light_timelapse_controls = instance;
}

// ============================================================================
// XML EVENT CALLBACKS (free functions using global accessor)
// ============================================================================

static void on_print_status_light_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintLightTimelapseControls] on_print_status_light_cb");
    (void)e;
    get_global_light_timelapse_controls().handle_light_button();
    LVGL_SAFE_EVENT_CB_END();
}

static void on_print_status_timelapse_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintLightTimelapseControls] on_print_status_timelapse_cb");
    (void)e;
    get_global_light_timelapse_controls().handle_timelapse_button();
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

PrintLightTimelapseControls::PrintLightTimelapseControls() = default;

PrintLightTimelapseControls::~PrintLightTimelapseControls() {
    deinit_subjects();
}

// ============================================================================
// SUBJECT INITIALIZATION
// ============================================================================

void PrintLightTimelapseControls::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Timelapse button icon: video-off (F0568) initially disabled
    UI_MANAGED_SUBJECT_STRING(timelapse_button_subject_, timelapse_button_buf_, "\xF3\xB0\x95\xA8",
                              "timelapse_button_icon", subjects_);
    UI_MANAGED_SUBJECT_STRING(timelapse_label_subject_, timelapse_label_buf_, "Off",
                              "timelapse_button_label", subjects_);

    // Light button icon: lightbulb_outline (F0336) initially off
    UI_MANAGED_SUBJECT_STRING(light_button_subject_, light_button_buf_, "\xF3\xB0\x8C\xB6",
                              "light_button_icon", subjects_);

    auto& leds = helix::led::LedController::instance();
    led_state_observer_ = helix::ui::observe<int>(
        leds.get_led_state_version_subject(), this,
        [](PrintLightTimelapseControls* self, int /*version*/) { self->refresh_light_state(); },
        leds.get_subjects_lifetime());

    // The On/Off label is translated into its buffer, so a switch re-fills it.
    refresh_timelapse_display();
    language_observer_ = helix::ui::observe_language_change(
        this, [](PrintLightTimelapseControls* self) { self->refresh_timelapse_display(); });

    // Register XML event callbacks
    lv_xml_register_event_cb(nullptr, "on_print_status_light", on_print_status_light_cb);
    lv_xml_register_event_cb(nullptr, "on_print_status_timelapse", on_print_status_timelapse_cb);

    subjects_initialized_ = true;
    spdlog::debug("[PrintLightTimelapseControls] Subjects initialized");
}

void PrintLightTimelapseControls::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    led_state_observer_.reset();
    language_observer_.reset();
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::debug("[PrintLightTimelapseControls] Subjects deinitialized");
}

// ============================================================================
// BUTTON HANDLERS
// ============================================================================

void PrintLightTimelapseControls::handle_light_button() {
    if (helix::led::LedController::instance().light_command_in_flight()) {
        spdlog::debug("[PrintLightTimelapseControls] Ignoring toggle — LED command in flight");
        ToastManager::instance().show(
            ToastSeverity::INFO, lv_tr("Light will switch when the current operation finishes"));
        return;
    }
    auto& ctrl = helix::led::LedController::instance();
    // Toggles the chamber light alone. The icon updates when its state does,
    // via refresh_light_state().
    const bool on = ctrl.toggle_power({ctrl.chamber_light()});
    spdlog::info("[PrintLightTimelapseControls] Light button clicked, chamber light -> {}",
                 on ? "ON" : "OFF");
}

void PrintLightTimelapseControls::refresh_timelapse_display() {
    // MDI Plane 15 icons use 4-byte UTF-8: video (F0567) on, video-off (F0568) off
    std::snprintf(timelapse_button_buf_, sizeof(timelapse_button_buf_), "%s",
                  timelapse_enabled_ ? "\xF3\xB0\x95\xA7" : "\xF3\xB0\x95\xA8");
    std::snprintf(timelapse_label_buf_, sizeof(timelapse_label_buf_), "%s",
                  timelapse_enabled_ ? lv_tr("On") : lv_tr("Off"));
    lv_subject_copy_string(&timelapse_button_subject_, timelapse_button_buf_);
    lv_subject_copy_string(&timelapse_label_subject_, timelapse_label_buf_);
}

void PrintLightTimelapseControls::handle_timelapse_button() {
    spdlog::info("[PrintLightTimelapseControls] Timelapse button clicked (current state: {})",
                 timelapse_enabled_ ? "enabled" : "disabled");

    // Toggle to opposite of current state
    bool new_state = !timelapse_enabled_;

    if (api_) {
        api_->timelapse().set_timelapse_enabled(
            new_state,
            lifetime_.bg_cb("PrintLightTimelapseControls::timelapse_set",
                            [this, new_state]() {
                                spdlog::info(
                                    "[PrintLightTimelapseControls] Timelapse {} successfully",
                                    new_state ? "enabled" : "disabled");
                                timelapse_enabled_ = new_state;
                                refresh_timelapse_display();
                            }),
            [](const MoonrakerError& err) {
                spdlog::error("[PrintLightTimelapseControls] Failed to toggle timelapse: {}",
                              err.message);
                helix::ui::notify_error_tr(TR_NOOP("Failed to toggle timelapse: {}"), err);
            });
    } else {
        spdlog::warn("[PrintLightTimelapseControls] API not available - cannot control timelapse");
        NOTIFY_ERROR(lv_tr("Cannot control timelapse: printer not connected"));
    }
}

// ============================================================================
// STATE UPDATES
// ============================================================================

void PrintLightTimelapseControls::refresh_light_state() {
    if (!subjects_initialized_) {
        return;
    }
    const bool on = helix::led::chamber_light_on();

    // Update light button icon: lightbulb_on (F06E8) or lightbulb_outline (F0336)
    if (on) {
        std::snprintf(light_button_buf_, sizeof(light_button_buf_), "\xF3\xB0\x9B\xA8");
    } else {
        std::snprintf(light_button_buf_, sizeof(light_button_buf_), "\xF3\xB0\x8C\xB6");
    }
    lv_subject_copy_string(&light_button_subject_, light_button_buf_);

    spdlog::debug("[PrintLightTimelapseControls] Chamber light: {}", on ? "ON" : "OFF");
}
