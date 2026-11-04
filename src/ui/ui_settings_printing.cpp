// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_printing.cpp
 * @brief Implementation of PrintingSettingsOverlay
 */

#include "ui_settings_printing.h"

#include "ui_ams_device_operations_overlay.h"
#include "ui_callback_helpers.h"
#include "ui_emergency_stop.h"
#include "ui_modal.h"
#include "ui_overlay_retraction_settings.h"
#include "ui_overlay_timelapse_settings.h"
#include "ui_settings_machine_limits.h"
#include "ui_settings_macro_buttons.h"
#include "ui_settings_material_temps.h"
#include "ui_settings_motion.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "core/update_queue.h"
#include "i_moonraker_api.h"
#include "plr_backend.h"
#include "plr_plugin_rpc.h"
#include "post_op_cooldown_manager.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"

#include <spdlog/spdlog.h>

namespace helix::settings {

using helix::ui::event_checked;
using helix::ui::event_selected;

namespace {

/// Ask the plugin whether printer.cfg carries the mesh include, and publish
/// the answer into the row's subject. Asked each time the overlay opens, so
/// a hand edit of printer.cfg is picked up; a failed ask keeps the last
/// answer and the next open asks again.
void query_plr_mesh_include() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    if (get_printer_state().plugin_status_state().plugin_feature(
            kPlrFeatureName, kPlrFeatureMinVersion) == PluginFeatureState::Absent) {
        return;
    }
    plr_rpc::request_mesh_include_status(
        *api,
        [](bool present) {
            helix::ui::queue_update(
                "PrintingSettingsOverlay::plr_mesh_include_status", [present]() {
                    get_printer_state().plugin_status_state().set_plr_mesh_include(present);
                });
        },
        [](const MoonrakerError&) {});
}

/// The restart a mesh-include change ends in. The restart is machine-moving,
/// so it keeps its own confirmation, separate from the change's.
void offer_firmware_restart() {
    helix::ui::modal_confirm(
        lv_tr("Restart Firmware?"),
        lv_tr("The change takes effect after Klipper restarts. This will restart Klipper "
              "firmware. Any active operations will be interrupted."),
        ModalSeverity::Warning, lv_tr("Restart"),
        [] { EmergencyStopOverlay::instance().firmware_restart(); });
}

/// Send the mesh-include action the user confirmed. The reply republishes
/// the row's state and offers the restart; both touch LVGL, and the client
/// delivers on its own thread, so they hop to the main thread.
void apply_plr_mesh_include(bool add) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    plr_rpc::mesh_include_action(
        *api, add,
        [](bool present, bool changed) {
            helix::ui::queue_update(
                "PrintingSettingsOverlay::plr_mesh_include_applied", [present, changed]() {
                    get_printer_state().plugin_status_state().set_plr_mesh_include(present);
                    // A no-op action (the include appeared between the ask and
                    // the confirm) changed nothing, so no restart is owed.
                    if (changed) {
                        offer_firmware_restart();
                    }
                });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[Settings] Changing the plr mesh include failed: {}", err.message);
            helix::ui::queue_update("PrintingSettingsOverlay::plr_mesh_include_failed", []() {
                ToastManager::instance().show(ToastSeverity::ERROR,
                                              lv_tr("Could not change printer.cfg"), 5000);
            });
        });
}

} // namespace

void PrintingSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_enclosure_style_changed",
         [](lv_event_t* e) {
             SettingsManager::instance().set_enclosure_style(
                 static_cast<helix::bed_drying::EnclosureStyle>(event_selected(e)));
         }},
        {"on_machine_limits_clicked",
         [](lv_event_t*) {
             auto& overlay = get_machine_limits_overlay();
             overlay.set_api(get_moonraker_api());
             overlay.show(get_printing_settings_overlay().parent_screen_);
         }},
        {"on_motion_settings_clicked", [](lv_event_t*) { show_motion_settings_overlay(); }},
        {"on_ams_settings_clicked",
         [](lv_event_t*) {
             helix::ui::get_ams_device_operations_overlay().show(
                 get_printing_settings_overlay().parent_screen_);
         }},
        {"on_material_temps_clicked",
         [](lv_event_t*) {
             get_material_temps_overlay().show(get_printing_settings_overlay().parent_screen_);
         }},
        {"on_allow_cold_extrude_changed",
         [](lv_event_t* e) {
             SafetySettingsManager::instance().set_allow_cold_extrude(event_checked(e));
         }},
        {"on_filament_auto_cooldown_changed",
         [](lv_event_t* e) {
             const bool enabled = event_checked(e);
             SettingsManager::instance().set_filament_auto_cooldown(enabled);
             // Turning it off mid-countdown takes effect now, not when the countdown ends.
             if (!enabled) {
                 PostOpCooldownManager::instance().cancel();
             }
         }},
        {"on_tool_change_confirm_changed",
         [](lv_event_t* e) {
             SafetySettingsManager::instance().set_tool_change_require_confirmation(
                 event_checked(e));
         }},
        {"on_filament_auto_open_editor_changed",
         [](lv_event_t* e) {
             SettingsManager::instance().set_filament_auto_open_editor(event_checked(e));
         }},
        {"on_retraction_row_clicked",
         [](lv_event_t*) {
             get_global_retraction_settings().show(get_printing_settings_overlay().parent_screen_);
         }},
        {"on_timelapse_settings_clicked", [](lv_event_t*) { open_timelapse_settings(); }},
        {"on_power_loss_recovery_changed",
         [](lv_event_t* e) {
             IMoonrakerAPI* api = get_moonraker_api();
             if (!api) {
                 return;
             }
             // The plugin's own verdict comes back through the features map,
             // which is also what puts the switch right again after a failure.
             plr_rpc::write_user(
                 *api, event_checked(e), [api]() { plr_rpc::refresh_features(*api); },
                 [api](const MoonrakerError& err) {
                     spdlog::warn("[Settings] Writing plr_user failed: {}", err.message);
                     plr_rpc::refresh_features(*api);
                 });
         }},
        {"on_macro_buttons_clicked",
         [](lv_event_t*) {
             get_macro_buttons_overlay().show(get_printing_settings_overlay().parent_screen_);
         }},
        {"on_plr_mesh_include_add_clicked",
         [](lv_event_t*) {
             helix::ui::modal_confirm(
                 lv_tr("Reload the saved bed mesh?"),
                 lv_tr("HelixScreen adds [include helix-plr/*.cfg] to printer.cfg, so a resume "
                       "reloads the mesh the print started with. A backup is kept beside "
                       "printer.cfg, and Klipper must restart for it to take effect."),
                 ModalSeverity::Warning, lv_tr("Add"), [] { apply_plr_mesh_include(true); });
         }},
        {"on_plr_mesh_include_remove_clicked",
         [](lv_event_t*) {
             helix::ui::modal_confirm(
                 lv_tr("Stop reloading the saved bed mesh?"),
                 lv_tr("HelixScreen removes its [include helix-plr/*.cfg] lines from "
                       "printer.cfg. A backup is kept beside printer.cfg, and Klipper must "
                       "restart for it to take effect. A resume then uses the printer's "
                       "default mesh profile."),
                 ModalSeverity::Warning, lv_tr("Remove"), [] { apply_plr_mesh_include(false); });
         }},
    });

    auto& plugin = get_printer_state().plugin_status_state();
    plr_feature_observer_ = helix::ui::observe<int>(
        plugin.get_plr_feature_state_subject(), this,
        [](PrintingSettingsOverlay*, int state) {
            if (state != static_cast<int>(PluginFeatureState::Absent)) {
                query_plr_mesh_include();
            }
        },
        get_printer_state().get_subjects_lifetime());
}

void PrintingSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    query_plr_mesh_include();
}

} // namespace helix::settings
