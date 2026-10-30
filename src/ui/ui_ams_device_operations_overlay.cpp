// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_ams_device_operations_overlay.cpp
 * @brief Implementation of AmsDeviceOperationsOverlay (progressive disclosure)
 */

#include "ui_ams_device_operations_overlay.h"

#include "ui_ams_device_section_detail_overlay.h"
#include "ui_ams_recover_state_modal.h"
#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_modal.h"
#include "ui_status_pill.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_bypass_policy.h"
#include "ams_state.h"
#include "ams_types.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "settings_manager.h"
#include "sort_small.h"
#include "static_panel_registry.h"
#include "ui/ams_drawing_utils.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

// ============================================================================
// DESTRUCTOR
// ============================================================================

AmsDeviceOperationsOverlay::~AmsDeviceOperationsOverlay() {
    // subjects_ tears the subjects down, withdrawing each XML-scope name before
    // the storage it resolves to goes away.
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void AmsDeviceOperationsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        // System info text (e.g. "System: AFC · v1.2.3")
        UI_MANAGED_SUBJECT_STRING(system_info_subject_, system_info_buf_, "",
                                  "ams_device_ops_system_info", subjects_);

        // Capability subjects
        UI_MANAGED_SUBJECT_INT(supports_bypass_subject_, 0, "ams_device_ops_supports_bypass",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(fw_supports_bypass_subject_, 0, "ams_device_ops_fw_supports_bypass",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(hw_bypass_sensor_subject_, 0, "ams_device_ops_hw_bypass_sensor",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(supports_auto_heat_subject_, 0, "ams_device_ops_supports_auto_heat",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(has_backend_subject_, 0, "ams_device_ops_has_backend", subjects_);
        UI_MANAGED_SUBJECT_INT(bypass_is_virtual_subject_, 0, "ams_device_ops_bypass_is_virtual",
                               subjects_);

        // Keep-spool-info-on-eject row visibility. Gates on
        // AmsBackend::printer_reports_spool_ids() in update_from_backend().
        UI_MANAGED_SUBJECT_INT(reports_spool_ids_subject_, 0, "ams_device_ops_reports_spool_ids",
                               subjects_);

        // Link-spools-from-tags row visibility. Gates on
        // AmsBackend::tags_name_spoolman_spools() in update_from_backend().
        UI_MANAGED_SUBJECT_INT(tags_name_spools_subject_, 0, "ams_device_ops_tags_name_spools",
                               subjects_);

        // Disables the keep-spool-info toggle when firmware retention owns the
        // behavior. Gates on AmsBackend::printer_retains_spool_info() in
        // update_from_backend().
        UI_MANAGED_SUBJECT_INT(printer_retains_spool_info_subject_, 0,
                               "ams_device_ops_printer_retains_spool_info", subjects_);

        // "Reset Endless Spool" row visibility. Gates on
        // EndlessSpoolCapabilities::editable() in update_from_backend().
        UI_MANAGED_SUBJECT_INT(can_reset_endless_spool_subject_, 0,
                               "ams_device_ops_can_reset_endless_spool", subjects_);
        UI_MANAGED_SUBJECT_INT(can_abort_subject_, 0, "ams_device_ops_can_abort", subjects_);
        UI_MANAGED_SUBJECT_INT(can_home_subject_, 0, "ams_device_ops_can_home", subjects_);
        UI_MANAGED_SUBJECT_INT(can_recover_subject_, 0, "ams_device_ops_can_recover", subjects_);
        UI_MANAGED_SUBJECT_STRING(details_subject_, details_buf_, "", "ams_device_ops_details",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(has_details_subject_, 0, "ams_device_ops_has_details", subjects_);
    });
}

void AmsDeviceOperationsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_ams_device_ops_home",
         [](lv_event_t*) {
             AmsBackend* backend = AmsState::instance().get_backend();
             if (!backend) {
                 NOTIFY_WARNING("{}", lv_tr("No Multi-Filament System connected"));
                 return;
             }
             AmsError result = backend->reset();
             if (result.success()) {
                 NOTIFY_INFO("{}", lv_tr("Homing..."));
             } else {
                 helix::ui::notify_ams_error(result, lv_tr("Home failed"));
             }
             get_ams_device_operations_overlay().refresh();
         }},
        {"on_ams_device_ops_recover",
         [](lv_event_t*) {
             AmsBackend* backend = AmsState::instance().get_backend();
             if (!backend) {
                 NOTIFY_WARNING("{}", lv_tr("No Multi-Filament System connected"));
                 return;
             }
             if (AmsRecoverStateModal::show_owned()) {
                 return; // The modal sends the state the user asserts.
             }
             AmsError result = backend->recover();
             if (result.success()) {
                 NOTIFY_INFO("{}", lv_tr("Recovering..."));
             } else {
                 helix::ui::notify_ams_error(result, lv_tr("Recovery failed"));
             }
             get_ams_device_operations_overlay().refresh();
         }},
        {"on_ams_device_ops_abort",
         [](lv_event_t*) {
             abort_ams_operation();
             get_ams_device_operations_overlay().refresh();
         }},
        {"on_ams_device_ops_bypass_toggled",
         [](lv_event_t*) {
             // The switch flips its own CHECKED state before this runs, so the widget
             // is not the authority on intent: the controller reads the backend, the
             // same way the sidebar toggle and the home tile do. It owns the print
             // guard, the hardware-sensor refusal and the unload-first chain.
             get_ams_device_operations_overlay().bypass_toggle_.toggle();

             // Put the switch back where the backend actually is. A refusal, or an
             // armed unload->enable chain that has not settled yet, leaves the widget
             // flipped ahead of reality; sync_from_backend() republishes
             // ams_bypass_active from every backend, and the notify re-applies the
             // binding for the case where that value did NOT change (lv_subject_set_int
             // is a no-op notify-wise when the value is unchanged, which is exactly
             // the refusal case).
             AmsState::instance().sync_from_backend();
             lv_subject_notify(AmsState::instance().get_bypass_active_subject());
         }},
        {"on_ams_endless_spool_toggled",
         [](lv_event_t* e) {
             const bool enable = event_checked(e);
             AmsBackend* backend = AmsState::instance().get_backend();
             AmsError result = backend ? backend->set_endless_spool_enabled(enable)
                                       : AmsErrorHelper::not_supported("Endless Spool");
             if (result.success()) {
                 return; // The backend's next report moves ams_endless_spool_on.
             }
             helix::ui::notify_ams_error(result, lv_tr("Endless Spool change failed"));
             // The switch flipped itself and the refusal left ams_endless_spool_on
             // where it was; an unchanged set does not notify, so the binding is
             // re-applied by hand.
             lv_subject_notify(AmsState::instance().get_endless_spool_on_subject());
         }},
        {"on_ams_always_show_bypass_spool_toggled",
         [](lv_event_t* e) {
             SettingsManager::instance().set_ams_always_show_bypass_spool(event_checked(e));
         }},
        {"on_ams_keep_spool_info_toggled",
         [](lv_event_t* e) {
             SettingsManager::instance().set_ams_keep_spool_info_on_eject(event_checked(e));
         }},
        {"on_ams_tag_links_spoolman_toggled",
         [](lv_event_t* e) {
             SettingsManager::instance().set_ams_tag_links_spoolman(event_checked(e));
         }},
        {"on_ams_force_bypass_controls_toggled",
         [](lv_event_t* e) {
             SettingsManager::instance().set_ams_force_bypass_controls(event_checked(e));
             // Both gating subjects are recomputed from the backend rather than from
             // the setting, so neither moves on its own when the override flips.
             // AmsState drives the sidebar toggle and the path node; this overlay
             // drives its own section.
             AmsState::instance().sync_from_backend();
             get_ams_device_operations_overlay().update_from_backend();
         }},
        {"on_ams_reset_endless_spool_clicked",
         [](lv_event_t*) {
             // The reset wipes ALL failover config, so it needs a confirmation, not a
             // bare tap. on_confirm re-fetches the backend so it cannot dangle if the
             // panel/backend changed while the dialog was open; the dialog closes
             // itself after the press.
             //
             // Both outcomes are announced. refresh() only re-derives
             // can_reset_endless_spool_subject_ from editable(), which a reset does not
             // change, and this overlay renders no endless-spool assignments at all (the
             // backup arrows live on AmsPanel and are not refreshed from here) - so
             // without a toast, wiping every backup looks exactly like a no-op.
             helix::ui::modal_confirm(
                 lv_tr("Reset Endless Spool?"),
                 lv_tr("This clears every Endless Spool backup. The print will stop on runout "
                       "until you set up Endless Spool again."),
                 ModalSeverity::Warning, lv_tr("Reset"), [] {
                     AmsBackend* b = AmsState::instance().get_backend();
                     if (!b) {
                         return;
                     }
                     AmsError result = b->reset_endless_spool();
                     if (!result.success()) {
                         helix::ui::notify_ams_error(result, lv_tr("Reset Endless Spool failed"));
                     } else {
                         NOTIFY_INFO("{}", lv_tr("Endless Spool backups cleared"));
                     }
                     get_ams_device_operations_overlay().refresh();
                 });
         }},
        {"on_ams_section_clicked",
         [](lv_event_t* e) {
             auto& self = get_ams_device_operations_overlay();
             auto* row = lv_event_get_current_target_obj(e);
             auto index = reinterpret_cast<size_t>(lv_obj_get_user_data(row));
             if (index >= self.cached_sections_.size()) {
                 spdlog::warn("[{}] Invalid section index: {}", self.get_name(), index);
                 return;
             }
             const auto& section = self.cached_sections_[index];
             get_ams_device_section_detail_overlay().show(self.parent_screen_, section.id,
                                                          section.label);
         }},
    });
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* AmsDeviceOperationsOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }

    section_list_container_ =
        helix::ui::find_required(overlay_root_, "section_list_container", get_name());

    action_observer_ = observe<int>(
        AmsState::instance().get_ams_action_subject(), this,
        [](AmsDeviceOperationsOverlay* self, int) { self->update_abort_available(); },
        AmsState::instance().get_subjects_lifetime());
    can_cancel_observer_ = observe<int>(
        AmsState::instance().get_ams_can_cancel_subject(), this,
        [](AmsDeviceOperationsOverlay* self, int) { self->update_abort_available(); },
        AmsState::instance().get_subjects_lifetime());

    return overlay_root_;
}

void AmsDeviceOperationsOverlay::on_ui_destroyed() {
    action_observer_.reset();
    can_cancel_observer_.reset();
    bypass_toggle_.cancel_pending();
}

void abort_ams_operation() {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING("{}", lv_tr("No Multi-Filament System connected"));
        return;
    }
    // Said first: a backend that reports its own result toasts it from inside
    // cancel(), and that toast must come after this one.
    NOTIFY_INFO("{}", lv_tr("Aborting..."));
    AmsError result = backend->cancel();
    if (!result.success()) {
        helix::ui::notify_ams_error(result, lv_tr("Abort failed"));
    }
}

std::vector<DeviceDetailRow> ams_device_detail_rows(const AmsSystemInfo& info) {
    std::vector<DeviceDetailRow> rows;
    for (size_t i = 0; i < info.units.size(); ++i) {
        const AmsUnit& unit = info.units[i];
        if (unit.absent) {
            continue;
        }
        const std::string name = ams_draw::get_unit_display_name(unit, static_cast<int>(i));
        if (!unit.firmware_version.empty()) {
            rows.push_back(
                {fmt::format(fmt::runtime(lv_tr("{} firmware")), name), unit.firmware_version});
        }
        if (!unit.serial_number.empty()) {
            rows.push_back(
                {fmt::format(fmt::runtime(lv_tr("{} serial")), name), unit.serial_number});
        }
    }
    if (info.toolchange_purge_volume > 0.0f) {
        rows.push_back({lv_tr("Toolchange purge"),
                        fmt::format("{:.0f} mm\xC2\xB3", info.toolchange_purge_volume)});
    }
    if (info.spoolman_mode != SpoolmanMode::OFF) {
        rows.push_back({lv_tr("Spoolman"), lv_tr(spoolman_mode_to_string(info.spoolman_mode))});
    }
    if (info.pending_spool_id >= 0) {
        rows.push_back({lv_tr("Pending spool"), fmt::format("#{}", info.pending_spool_id)});
    }
    return rows;
}

void AmsDeviceOperationsOverlay::refresh() {
    if (!overlay_) {
        return;
    }

    spdlog::debug("[{}] Refreshing from backend", get_name());
    update_from_backend();
}

// ============================================================================
// BACKEND QUERIES
// ============================================================================

void AmsDeviceOperationsOverlay::update_abort_available() {
    if (!subjects_initialized_) {
        return;
    }
    AmsBackend* backend = AmsState::instance().get_backend();
    lv_subject_set_int(&can_abort_subject_, backend && backend->can_cancel_operation() ? 1 : 0);
}

void AmsDeviceOperationsOverlay::update_from_backend() {
    AmsBackend* backend = AmsState::instance().get_backend();

    if (!backend) {
        spdlog::warn("[{}] No backend available", get_name());
        lv_subject_set_int(&has_backend_subject_, 0);
        lv_subject_set_int(&supports_bypass_subject_, 0);
        lv_subject_set_int(&fw_supports_bypass_subject_, 0);
        lv_subject_set_int(&hw_bypass_sensor_subject_, 0);
        lv_subject_set_int(&supports_auto_heat_subject_, 0);
        lv_subject_set_int(&bypass_is_virtual_subject_, 0);
        lv_subject_set_int(&reports_spool_ids_subject_, 0);
        lv_subject_set_int(&tags_name_spools_subject_, 0);
        lv_subject_set_int(&printer_retains_spool_info_subject_, 0);
        lv_subject_set_int(&can_reset_endless_spool_subject_, 0);
        lv_subject_set_int(&can_abort_subject_, 0);
        lv_subject_set_int(&can_home_subject_, 0);
        lv_subject_set_int(&can_recover_subject_, 0);
        details_buf_[0] = '\0';
        lv_subject_copy_string(&details_subject_, details_buf_);
        lv_subject_set_int(&has_details_subject_, 0);
        system_info_buf_[0] = '\0';
        lv_subject_copy_string(&system_info_subject_, system_info_buf_);

        if (section_list_container_) {
            helix::ui::safe_clean_children(section_list_container_); // [L081]
        }
        cached_sections_.clear();
        return;
    }

    // Has backend
    lv_subject_set_int(&has_backend_subject_, 1);
    update_abort_available();

    // Query capabilities
    auto info = backend->get_system_info();

    // System info line (e.g. "System: AFC · v1.2.3")
    if (info.version.empty() || info.version == "unknown") {
        snprintf(system_info_buf_, sizeof(system_info_buf_), "%s: %s", lv_tr("System"),
                 info.type_name.c_str());
    } else {
        snprintf(system_info_buf_, sizeof(system_info_buf_), "%s: %s · v%s", lv_tr("System"),
                 info.type_name.c_str(), info.version.c_str());
    }
    lv_subject_copy_string(&system_info_subject_, system_info_buf_);

    std::string details;
    for (const DeviceDetailRow& row : ams_device_detail_rows(info)) {
        if (!details.empty()) {
            details += '\n';
        }
        details += row.label + ": " + row.value;
    }
    snprintf(details_buf_, sizeof(details_buf_), "%s", details.c_str());
    lv_subject_copy_string(&details_subject_, details_buf_);
    lv_subject_set_int(&has_details_subject_, details.empty() ? 0 : 1);

    lv_subject_set_int(&supports_bypass_subject_,
                       helix::bypass_available_for(info.supports_bypass) ? 1 : 0);
    lv_subject_set_int(&fw_supports_bypass_subject_, info.supports_bypass ? 1 : 0);
    lv_subject_set_int(&hw_bypass_sensor_subject_, info.has_hardware_bypass_sensor ? 1 : 0);

    // Update hardware bypass status pill if applicable
    if (info.has_hardware_bypass_sensor && overlay_) {
        auto* pill = helix::ui::find_required(overlay_, "bypass_status_pill", get_name());
        if (pill) {
            bool active = backend->is_bypass_active();
            ui_status_pill_set_text(pill, active ? lv_tr("Active") : lv_tr("Inactive"));
            ui_status_pill_set_variant(pill, active ? "success" : "muted");
        }
    }

    lv_subject_set_int(&supports_auto_heat_subject_, backend->supports_auto_heat_on_load() ? 1 : 0);

    // The always-show-bypass row is only meaningful where the bypass node
    // hides when disengaged, i.e. where the reported bypass is virtual.
    lv_subject_set_int(&bypass_is_virtual_subject_, backend->bypass_is_virtual() ? 1 : 0);

    // Keep-spool-info-on-eject is only meaningful where the firmware reports
    // spool ids per lane (AFC, Happy Hare); other systems clear on a detected
    // spool swap regardless of the toggle, so the row stays hidden there.
    lv_subject_set_int(&reports_spool_ids_subject_, backend->printer_reports_spool_ids() ? 1 : 0);

    // Linking a lane from its tag means something only where a tag can name a
    // Spoolman spool.
    lv_subject_set_int(&tags_name_spools_subject_, backend->tags_name_spoolman_spools() ? 1 : 0);

    // Firmware retention (AFC remember_spool = true everywhere) makes the
    // keep-spool-info toggle a no-op: firmware keeps reporting the spool id,
    // so neither the eject rule nor the re-assert push ever fires. Show it
    // disabled with a note rather than letting it silently lie.
    lv_subject_set_int(&printer_retains_spool_info_subject_,
                       backend->printer_retains_spool_info() ? 1 : 0);

    // "Reset Endless Spool" lights up for any backend whose endless-spool
    // mapping the UI may write (editable() = available + not ReadOnly): AFC's
    // per-slot edges, single-unit Happy Hare's groups, and the mock. CFS and
    // AD5X IFS are read-only, so the row stays hidden there. The base
    // reset_endless_spool() re-checks this and rejects if it moved, so a
    // stale button that won the race just reports the refusal.
    lv_subject_set_int(&can_reset_endless_spool_subject_,
                       backend->get_endless_spool_capabilities().editable() ? 1 : 0);

    // Home sends reset() and Recover recover(); a machine with no command
    // behind either shows the button disabled rather than toasting a refusal.
    lv_subject_set_int(&can_home_subject_, backend->reset_available() ? 1 : 0);
    lv_subject_set_int(&can_recover_subject_, backend->recover_available() ? 1 : 0);

    // Populate section rows
    populate_section_list();
}

// ============================================================================
// SECTION LIST
// ============================================================================

void AmsDeviceOperationsOverlay::populate_section_list() {
    if (!section_list_container_) {
        return;
    }

    helix::ui::safe_clean_children(section_list_container_); // [L081]
    cached_sections_.clear();

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }

    cached_sections_ = backend->get_device_sections();

    // Sort by display_order
    helix::sort_small(
        cached_sections_.begin(), cached_sections_.end(),
        [](const auto& a, const auto& b) { return a.display_order < b.display_order; });

    // Only show sections that have actions
    auto all_actions = backend->get_device_actions();

    for (const auto& section : cached_sections_) {
        bool has_actions = std::any_of(all_actions.begin(), all_actions.end(),
                                       [&](const auto& a) { return a.section == section.id; });
        if (has_actions) {
            create_section_row(section_list_container_, section);
        }
    }

    spdlog::debug("[{}] Populated {} section rows", get_name(), cached_sections_.size());
}

void AmsDeviceOperationsOverlay::create_section_row(lv_obj_t* parent,
                                                    const helix::printer::DeviceSection& section) {
    // Reuse the standard setting_action_row XML component
    const char* attrs[] = {"label",
                           lv_tr(section.label.c_str()),
                           "label_tag",
                           section.label.c_str(),
                           "icon",
                           section.icon.c_str(),
                           "description",
                           lv_tr(section.description.c_str()),
                           "description_tag",
                           section.description.c_str(),
                           "callback",
                           "on_ams_section_clicked",
                           nullptr};

    lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(parent, "setting_action_row", attrs));
    if (!row) {
        spdlog::warn("[{}] Failed to create section row for '{}'", get_name(), section.id);
        return;
    }
    // Every row from the XML component is called "action_row", so a section list
    // reads as N identical names and none of them can be addressed from
    // `helix-screen ctl`. Rename to the section id, which is already unique.
    lv_obj_set_name(row, fmt::format("section_row_{}", section.id).c_str());

    // Store section index in user_data for click dispatch
    size_t section_index = 0;
    for (size_t i = 0; i < cached_sections_.size(); i++) {
        if (cached_sections_[i].id == section.id) {
            section_index = i;
            break;
        }
    }
    lv_obj_set_user_data(row, reinterpret_cast<void*>(section_index));
}

} // namespace helix::ui
