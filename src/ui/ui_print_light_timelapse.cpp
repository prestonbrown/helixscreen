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
    // Button is gated by `led_controllable` in XML, so it can't be clicked unless
    // LedController has at least one selected strip — no defensive bail-out needed.
    spdlog::info("[PrintLightTimelapseControls] Light button clicked (subject: {})",
                 led_on_ ? "ON" : "OFF");

    // Send the opposite of what Moonraker reports.  Icon updates when
    // Moonraker status arrives via update_led_state().
    helix::led::LedController::instance().light_set(!led_on_);
}

void PrintLightTimelapseControls::handle_timelapse_button() {
    spdlog::info("[PrintLightTimelapseControls] Timelapse button clicked (current state: {})",
                 timelapse_enabled_ ? "enabled" : "disabled");

    // Toggle to opposite of current state
    bool new_state = !timelapse_enabled_;

    if (api_) {
        api_->timelapse().set_timelapse_enabled(
            new_state,
            [this, new_state]() {
                spdlog::info("[PrintLightTimelapseControls] Timelapse {} successfully",
                             new_state ? "enabled" : "disabled");

                // Defer UI updates to LVGL thread - API callbacks may be on background thread
                auto data_ptr = std::make_unique<std::pair<PrintLightTimelapseControls*, bool>>(
                    this, new_state);
                helix::ui::async_call(
                    [](void* user_data) {
                        // Wrap raw pointer in unique_ptr for RAII cleanup
                        std::unique_ptr<std::pair<PrintLightTimelapseControls*, bool>> data(
                            static_cast<std::pair<PrintLightTimelapseControls*, bool>*>(user_data));
                        auto* self = data->first;
                        bool enabled = data->second;

                        // Update local state
                        self->timelapse_enabled_ = enabled;

                        // Update icon and label: video (F0567) enabled, video-off (F0568) disabled
                        // MDI Plane 15 icons use 4-byte UTF-8 encoding
                        if (enabled) {
                            std::snprintf(self->timelapse_button_buf_,
                                          sizeof(self->timelapse_button_buf_),
                                          "\xF3\xB0\x95\xA7"); // video
                            std::snprintf(self->timelapse_label_buf_,
                                          sizeof(self->timelapse_label_buf_), "On");
                        } else {
                            std::snprintf(self->timelapse_button_buf_,
                                          sizeof(self->timelapse_button_buf_),
                                          "\xF3\xB0\x95\xA8"); // video-off
                            std::snprintf(self->timelapse_label_buf_,
                                          sizeof(self->timelapse_label_buf_), "Off");
                        }
                        lv_subject_copy_string(&self->timelapse_button_subject_,
                                               self->timelapse_button_buf_);
                        lv_subject_copy_string(&self->timelapse_label_subject_,
                                               self->timelapse_label_buf_);
                        // data automatically freed via ~unique_ptr()
                    },
                    data_ptr.release());
            },
            [](const MoonrakerError& err) {
                spdlog::error("[PrintLightTimelapseControls] Failed to toggle timelapse: {}",
                              err.message);
                NOTIFY_ERROR(lv_tr("Failed to toggle timelapse: {}"), err.user_message());
            });
    } else {
        spdlog::warn("[PrintLightTimelapseControls] API not available - cannot control timelapse");
        NOTIFY_ERROR(lv_tr("Cannot control timelapse: printer not connected"));
    }
}

// ============================================================================
// STATE UPDATES
// ============================================================================

void PrintLightTimelapseControls::update_led_state(bool on) {
    led_on_ = on;

    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    // Update light button icon: lightbulb_on (F06E8) or lightbulb_outline (F0336)
    if (led_on_) {
        std::snprintf(light_button_buf_, sizeof(light_button_buf_), "\xF3\xB0\x9B\xA8");
    } else {
        std::snprintf(light_button_buf_, sizeof(light_button_buf_), "\xF3\xB0\x8C\xB6");
    }
    lv_subject_copy_string(&light_button_subject_, light_button_buf_);

    spdlog::debug("[PrintLightTimelapseControls] LED state changed: {}", led_on_ ? "ON" : "OFF");
}
