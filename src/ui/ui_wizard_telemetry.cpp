// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_telemetry.h"

#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_subject_registry.h"
#include "ui_toast_manager.h"
#include "ui_wizard.h"

#include "lvgl/lvgl.h"
#include "static_panel_registry.h"
#include "system_settings_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>

using namespace helix;

// ============================================================================
// Global Instance
// ============================================================================

WizardTelemetryStep* get_wizard_telemetry_step() {
    return &helix::lazy_global<WizardTelemetryStep>("WizardTelemetryStep");
}

// ============================================================================
// Subject Initialization
// ============================================================================

void WizardTelemetryStep::init_subjects() {
    spdlog::debug("[{}] Initializing subjects", get_name());

    init_info_text_subject();

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void WizardTelemetryStep::init_info_text_subject() {
    static const char* telemetry_info_md =
        "**HelixScreen is a free, open-source project** built by a tiny team. "
        "Anonymous telemetry helps us understand how the app is actually used "
        "so we can focus on what matters.\n\n"
        "## What we collect\n"
        "- **App version** and platform (Pi model, screen size)\n"
        "- **Printer type** (kinematics, build volume — NOT your printer name)\n"
        "- **Print outcomes** (completed vs failed, duration, temps)\n"
        "- **Crash reports** (stack traces to fix bugs)\n"
        "- **Feature usage** (which panels you use, AMS, input shaper, etc.)\n\n"
        "## What we NEVER collect\n"
        "- Your name, location, or IP address\n"
        "- File names or G-code content\n"
        "- Camera images or thumbnails\n"
        "- WiFi passwords or network details\n"
        "- Anything that could identify you personally\n\n"
        "## Why it matters\n"
        "With just a few hundred users reporting anonymously, we can see which "
        "printers crash most, which features nobody uses, and where to spend our "
        "limited time. **You can view the exact data in Settings > System > View "
        "Telemetry Data anytime.**";
    UI_SUBJECT_INIT_AND_REGISTER_STRING(telemetry_info_text_, telemetry_info_text_buffer_,
                                        telemetry_info_md, "telemetry_info_text");
}

// ============================================================================
// Callback Registration
// ============================================================================

void WizardTelemetryStep::register_callbacks() {
    spdlog::debug("[{}] Registering callbacks", get_name());
    lv_xml_register_event_cb(nullptr, "on_wizard_telemetry_changed",
                             WizardTelemetryStep::on_wizard_telemetry_changed);
    lv_xml_register_event_cb(nullptr, "on_wizard_telemetry_info",
                             WizardTelemetryStep::on_wizard_telemetry_info);
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardTelemetryStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating telemetry screen", get_name());

    if (root_) {
        spdlog::warn("[{}] Screen pointer not null - cleanup may not have been called properly",
                     get_name());
        root_ = nullptr;
    }

    // Refresh subjects before creating UI
    init_subjects();

    root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_telemetry", nullptr));
    if (!root_) {
        spdlog::error("[{}] Failed to create screen from XML", get_name());
        return nullptr;
    }

    spdlog::debug("[{}] Screen created successfully", get_name());
    return root_;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardTelemetryStep::cleanup() {
    spdlog::debug("[{}] Cleaning up resources", get_name());
    root_ = nullptr;
}

// ============================================================================
// Skip Logic
// ============================================================================

bool WizardTelemetryStep::should_skip() const {
    // Skip logic handled by the wizard orchestrator, not the step itself
    return false;
}

// ============================================================================
// Static Callbacks
// ============================================================================

void WizardTelemetryStep::on_wizard_telemetry_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[WizardTelemetry] on_wizard_telemetry_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    spdlog::info("[WizardTelemetry] Telemetry toggled: {}", enabled ? "ON" : "OFF");
    SystemSettingsManager::instance().set_telemetry_enabled(enabled);
    if (enabled) {
        ToastManager::instance().show(
            ToastSeverity::SUCCESS,
            lv_tr("Thanks! Anonymous usage data helps improve HelixScreen."), 4000);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void WizardTelemetryStep::on_wizard_telemetry_info(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[WizardTelemetry] on_wizard_telemetry_info");
    spdlog::debug("[WizardTelemetry] Showing telemetry info modal");
    lv_obj_t* dialog = Modal::show("telemetry_info_modal");
    if (dialog) {
        lv_obj_t* ok_btn = lv_obj_find_by_name(dialog, "btn_primary");
        if (ok_btn) {
            lv_obj_add_event_cb(
                ok_btn,
                [](lv_event_t* ev) {
                    auto* dlg = static_cast<lv_obj_t*>(lv_event_get_user_data(ev));
                    Modal::hide(dlg);
                },
                LV_EVENT_CLICKED, dialog);
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}
