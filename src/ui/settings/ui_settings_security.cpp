// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_security.cpp
 * @brief Security Settings overlay — PIN management and auto-lock configuration.
 */

#include "ui_settings_security.h"

#include "ui_callback_helpers.h"
#include "ui_pin_entry_modal.h"
#include "ui_toast_manager.h"

#include "lock_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

namespace helix::settings {

using helix::ui::PinEntryModal;

void SecuritySettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_set_pin_clicked",
         [](lv_event_t*) { get_security_settings_overlay().run_set_pin_flow(); }},
        {"on_change_pin_clicked",
         [](lv_event_t*) { get_security_settings_overlay().handle_change_pin_clicked(); }},
        {"on_remove_pin_clicked",
         [](lv_event_t*) { get_security_settings_overlay().handle_remove_pin_clicked(); }},
        {"on_auto_lock_changed",
         [](lv_event_t* e) {
             helix::LockManager::instance().set_auto_lock(helix::ui::event_checked(e));
         }},
    });
}

void SecuritySettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    init_auto_lock_toggle();
}

// The LockManager flag has no subject, so the toggle is re-synced on each open.
void SecuritySettingsOverlay::init_auto_lock_toggle() {
    lv_obj_t* row = helix::ui::find_required(overlay_root_, "row_auto_lock", get_name());
    lv_obj_t* toggle = helix::ui::find_required(row, "toggle", get_name());
    if (!toggle) {
        return;
    }
    if (helix::LockManager::instance().auto_lock_enabled()) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    }
}

/**
 * Two-step Set PIN flow: "Enter New PIN" → "Confirm PIN".
 * On match, the PIN is saved. On mismatch, error toast is shown.
 */
void SecuritySettingsOverlay::run_set_pin_flow() {
    PinEntryModal::show_pin_entry(
        lv_tr("Enter New PIN"), [](const std::string& pin1) -> std::string {
            if (pin1.empty()) {
                spdlog::debug("[SecuritySettings] Set PIN cancelled at step 1");
                return "";
            }
            // Second entry for confirmation
            PinEntryModal::show_pin_entry(
                lv_tr("Confirm PIN"), [pin1](const std::string& pin2) -> std::string {
                    if (pin2.empty()) {
                        spdlog::debug("[SecuritySettings] Set PIN cancelled at step 2");
                        return "";
                    }
                    if (pin1 != pin2) {
                        spdlog::info("[SecuritySettings] PIN confirmation mismatch");
                        return lv_tr("PINs don't match");
                    }
                    if (helix::LockManager::instance().set_pin(pin1)) {
                        spdlog::info("[SecuritySettings] PIN set successfully");
                        ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("PIN set"));
                    } else {
                        spdlog::warn("[SecuritySettings] set_pin() failed (invalid length?)");
                        ToastManager::instance().show(ToastSeverity::ERROR,
                                                      lv_tr("PIN must be 4-6 digits"));
                    }
                    return "";
                });
            return "";
        });
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void SecuritySettingsOverlay::handle_change_pin_clicked() {
    spdlog::info("[{}] Change PIN clicked", get_name());

    PinEntryModal::show_pin_entry(
        lv_tr("Enter Current PIN"), [](const std::string& current) -> std::string {
            if (current.empty()) {
                spdlog::debug("[SecuritySettings] Change PIN cancelled at current PIN step");
                return "";
            }
            if (!helix::LockManager::instance().verify_pin(current)) {
                spdlog::info("[SecuritySettings] Change PIN: wrong current PIN");
                return lv_tr("Wrong PIN");
            }
            // Current PIN verified — proceed to set new PIN
            get_security_settings_overlay().run_set_pin_flow();
            return "";
        });
}

void SecuritySettingsOverlay::handle_remove_pin_clicked() {
    spdlog::info("[{}] Remove PIN clicked", get_name());

    PinEntryModal::show_pin_entry(
        lv_tr("Enter Current PIN"), [](const std::string& current) -> std::string {
            if (current.empty()) {
                spdlog::debug("[SecuritySettings] Remove PIN cancelled");
                return "";
            }
            if (!helix::LockManager::instance().verify_pin(current)) {
                spdlog::info("[SecuritySettings] Remove PIN: wrong PIN");
                return lv_tr("Wrong PIN");
            }
            helix::LockManager::instance().remove_pin();
            spdlog::info("[SecuritySettings] PIN removed");
            ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("PIN removed"));
            return "";
        });
}

} // namespace helix::settings
