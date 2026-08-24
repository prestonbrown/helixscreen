// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_ams_device_operations_overlay.cpp
 * @brief Implementation of AmsDeviceOperationsOverlay (progressive disclosure)
 */

#include "ui_ams_device_operations_overlay.h"

#include "ui_ams_device_section_detail_overlay.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_status_pill.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_bypass_policy.h"
#include "ams_state.h"
#include "ams_types.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "settings_manager.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<AmsDeviceOperationsOverlay> g_ams_device_operations_overlay;

AmsDeviceOperationsOverlay& get_ams_device_operations_overlay() {
    if (!g_ams_device_operations_overlay) {
        g_ams_device_operations_overlay = std::make_unique<AmsDeviceOperationsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "AmsDeviceOperationsOverlay", []() { g_ams_device_operations_overlay.reset(); });
    }
    return *g_ams_device_operations_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

AmsDeviceOperationsOverlay::AmsDeviceOperationsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

AmsDeviceOperationsOverlay::~AmsDeviceOperationsOverlay() {
    if (subjects_initialized_ && lv_is_initialized()) {
        lv_subject_deinit(&system_info_subject_);
        lv_subject_deinit(&status_subject_);
        lv_subject_deinit(&supports_bypass_subject_);
        lv_subject_deinit(&fw_supports_bypass_subject_);
        lv_subject_deinit(&hw_bypass_sensor_subject_);
        lv_subject_deinit(&supports_auto_heat_subject_);
        lv_subject_deinit(&has_backend_subject_);
        lv_subject_deinit(&is_afc_subject_);
        lv_subject_deinit(&reports_spool_ids_subject_);
        lv_subject_deinit(&printer_retains_spool_info_subject_);
        lv_subject_deinit(&is_qidi_subject_);
        lv_subject_deinit(&qidi_eject_distance_display_subject_);
        lv_subject_deinit(&qidi_eject_velocity_display_subject_);
        lv_subject_deinit(&can_reset_endless_spool_subject_);
    }
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void AmsDeviceOperationsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // System info text (e.g. "System: AFC · v1.2.3")
    system_info_buf_[0] = '\0';
    lv_subject_init_string(&system_info_subject_, system_info_buf_, nullptr,
                           sizeof(system_info_buf_), system_info_buf_);
    lv_xml_register_subject(nullptr, "ams_device_ops_system_info", &system_info_subject_);

    // Status text
    snprintf(status_buf_, sizeof(status_buf_), "%s", lv_tr("Idle"));
    lv_subject_init_string(&status_subject_, status_buf_, nullptr, sizeof(status_buf_),
                           status_buf_);
    lv_xml_register_subject(nullptr, "ams_device_ops_status", &status_subject_);

    // Capability subjects
    lv_subject_init_int(&supports_bypass_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_supports_bypass", &supports_bypass_subject_);

    lv_subject_init_int(&fw_supports_bypass_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_fw_supports_bypass",
                            &fw_supports_bypass_subject_);

    lv_subject_init_int(&hw_bypass_sensor_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_hw_bypass_sensor", &hw_bypass_sensor_subject_);

    lv_subject_init_int(&supports_auto_heat_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_supports_auto_heat",
                            &supports_auto_heat_subject_);

    lv_subject_init_int(&has_backend_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_has_backend", &has_backend_subject_);

    lv_subject_init_int(&is_afc_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_is_afc", &is_afc_subject_);

    // Keep-spool-info-on-eject row visibility. Gates on
    // AmsBackend::printer_reports_spool_ids() in update_from_backend().
    lv_subject_init_int(&reports_spool_ids_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_reports_spool_ids",
                            &reports_spool_ids_subject_);

    // Disables the keep-spool-info toggle when firmware retention owns the
    // behavior. Gates on AmsBackend::printer_retains_spool_info() in
    // update_from_backend().
    lv_subject_init_int(&printer_retains_spool_info_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_printer_retains_spool_info",
                            &printer_retains_spool_info_subject_);

    // QIDI Box gating + eject distance/velocity value displays
    lv_subject_init_int(&is_qidi_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_is_qidi", &is_qidi_subject_);

    qidi_eject_distance_buf_[0] = '\0';
    lv_subject_init_string(&qidi_eject_distance_display_subject_, qidi_eject_distance_buf_, nullptr,
                           sizeof(qidi_eject_distance_buf_), qidi_eject_distance_buf_);
    lv_xml_register_subject(nullptr, "ams_device_ops_qidi_eject_distance_display",
                            &qidi_eject_distance_display_subject_);

    qidi_eject_velocity_buf_[0] = '\0';
    lv_subject_init_string(&qidi_eject_velocity_display_subject_, qidi_eject_velocity_buf_, nullptr,
                           sizeof(qidi_eject_velocity_buf_), qidi_eject_velocity_buf_);
    lv_xml_register_subject(nullptr, "ams_device_ops_qidi_eject_velocity_display",
                            &qidi_eject_velocity_display_subject_);

    // "Reset Endless Spool" row visibility. Gates on
    // EndlessSpoolCapabilities::editable() in update_from_backend().
    lv_subject_init_int(&can_reset_endless_spool_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_can_reset_endless_spool",
                            &can_reset_endless_spool_subject_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void AmsDeviceOperationsOverlay::register_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_ams_device_ops_home", on_home_clicked);
    lv_xml_register_event_cb(nullptr, "on_ams_device_ops_recover", on_recover_clicked);
    lv_xml_register_event_cb(nullptr, "on_ams_device_ops_abort", on_abort_clicked);
    lv_xml_register_event_cb(nullptr, "on_ams_device_ops_bypass_toggled", on_bypass_toggled);
    lv_xml_register_event_cb(nullptr, "on_ams_afc_unload_after_print_toggled",
                             on_afc_unload_after_print_toggled);
    lv_xml_register_event_cb(nullptr, "on_ams_always_show_bypass_spool_toggled",
                             on_always_show_bypass_spool_toggled);
    lv_xml_register_event_cb(nullptr, "on_ams_keep_spool_info_toggled", on_keep_spool_info_toggled);
    lv_xml_register_event_cb(nullptr, "on_ams_force_bypass_controls_toggled",
                             on_force_bypass_controls_toggled);
    lv_xml_register_event_cb(nullptr, "on_ams_qidi_eject_distance_changed",
                             on_qidi_eject_distance_changed);
    lv_xml_register_event_cb(nullptr, "on_ams_qidi_eject_velocity_changed",
                             on_qidi_eject_velocity_changed);
    lv_xml_register_event_cb(nullptr, "on_ams_reset_endless_spool_clicked",
                             on_reset_endless_spool_clicked);
    lv_xml_register_event_cb(nullptr, "on_ams_section_clicked", on_section_row_clicked);
    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* AmsDeviceOperationsOverlay::create(lv_obj_t* parent) {
    if (overlay_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_device_operations", nullptr));
    if (!overlay_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Find section list container
    section_list_container_ = lv_obj_find_by_name(overlay_, "section_list_container");
    if (!section_list_container_) {
        spdlog::warn("[{}] section_list_container not found in XML", get_name());
    }

    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_;
}

void AmsDeviceOperationsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    if (!overlay_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    refresh();

    NavigationManager::instance().register_overlay_instance(overlay_, this);
    NavigationManager::instance().push_overlay(overlay_);
}

void AmsDeviceOperationsOverlay::on_ui_destroyed() {
    bypass_toggle_.cancel_pending();
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

void AmsDeviceOperationsOverlay::update_from_backend() {
    AmsBackend* backend = AmsState::instance().get_backend();

    if (!backend) {
        spdlog::warn("[{}] No backend available", get_name());
        lv_subject_set_int(&has_backend_subject_, 0);
        lv_subject_set_int(&supports_bypass_subject_, 0);
        lv_subject_set_int(&fw_supports_bypass_subject_, 0);
        lv_subject_set_int(&hw_bypass_sensor_subject_, 0);
        lv_subject_set_int(&supports_auto_heat_subject_, 0);
        lv_subject_set_int(&is_afc_subject_, 0);
        lv_subject_set_int(&reports_spool_ids_subject_, 0);
        lv_subject_set_int(&printer_retains_spool_info_subject_, 0);
        lv_subject_set_int(&is_qidi_subject_, 0);
        lv_subject_set_int(&can_reset_endless_spool_subject_, 0);
        system_info_buf_[0] = '\0';
        lv_subject_copy_string(&system_info_subject_, system_info_buf_);
        snprintf(status_buf_, sizeof(status_buf_), "%s",
                 lv_tr("No Multi-Filament System connected"));
        lv_subject_copy_string(&status_subject_, status_buf_);

        if (section_list_container_) {
            helix::ui::safe_clean_children(section_list_container_); // [L081]
        }
        cached_sections_.clear();
        return;
    }

    // Has backend
    lv_subject_set_int(&has_backend_subject_, 1);

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
    lv_subject_set_int(&supports_bypass_subject_,
                       helix::bypass_available_for(info.supports_bypass) ? 1 : 0);
    lv_subject_set_int(&fw_supports_bypass_subject_, info.supports_bypass ? 1 : 0);
    lv_subject_set_int(&hw_bypass_sensor_subject_, info.has_hardware_bypass_sensor ? 1 : 0);

    // Update hardware bypass status pill if applicable
    if (info.has_hardware_bypass_sensor && overlay_) {
        auto* pill = lv_obj_find_by_name(overlay_, "bypass_status_pill");
        if (pill) {
            bool active = backend->is_bypass_active();
            ui_status_pill_set_text(pill, active ? lv_tr("Active") : lv_tr("Inactive"));
            ui_status_pill_set_variant(pill, active ? "success" : "muted");
        }
    }

    lv_subject_set_int(&supports_auto_heat_subject_, backend->supports_auto_heat_on_load() ? 1 : 0);

    // AFC-only: the unload-after-print toggle applies only to AFC systems
    lv_subject_set_int(&is_afc_subject_, backend->is_afc_system() ? 1 : 0);

    // Keep-spool-info-on-eject is only meaningful where the firmware reports
    // spool ids per lane (AFC, Happy Hare); other systems clear on a detected
    // spool swap regardless of the toggle, so the row stays hidden there.
    lv_subject_set_int(&reports_spool_ids_subject_, backend->printer_reports_spool_ids() ? 1 : 0);

    // Firmware retention (AFC remember_spool = true everywhere) makes the
    // keep-spool-info toggle a no-op: firmware keeps reporting the spool id,
    // so neither the eject rule nor the re-assert push ever fires. Show it
    // disabled with a note rather than letting it silently lie.
    lv_subject_set_int(&printer_retains_spool_info_subject_,
                       backend->printer_retains_spool_info() ? 1 : 0);

    // The eject distance/velocity rows apply only to backends with configurable
    // eject params (QIDI Box). Sync the sliders + value displays from settings.
    bool show_eject_params = backend->supports_configurable_eject_params();
    lv_subject_set_int(&is_qidi_subject_, show_eject_params ? 1 : 0);
    if (show_eject_params && overlay_) {
        int eject_distance = SettingsManager::instance().get_qidi_eject_distance();
        int eject_velocity = SettingsManager::instance().get_qidi_eject_velocity();

        auto* dist_slider = lv_obj_find_by_name(overlay_, "qidi_eject_distance_slider");
        if (dist_slider) {
            lv_slider_set_value(dist_slider, eject_distance, LV_ANIM_OFF);
        }
        snprintf(qidi_eject_distance_buf_, sizeof(qidi_eject_distance_buf_), "%d mm",
                 eject_distance);
        lv_subject_copy_string(&qidi_eject_distance_display_subject_, qidi_eject_distance_buf_);

        auto* vel_slider = lv_obj_find_by_name(overlay_, "qidi_eject_velocity_slider");
        if (vel_slider) {
            lv_slider_set_value(vel_slider, eject_velocity, LV_ANIM_OFF);
        }
        snprintf(qidi_eject_velocity_buf_, sizeof(qidi_eject_velocity_buf_), "%d mm/s",
                 eject_velocity);
        lv_subject_copy_string(&qidi_eject_velocity_display_subject_, qidi_eject_velocity_buf_);
    }

    // "Reset Endless Spool" lights up for any backend whose endless-spool
    // mapping the UI may write (editable() = available + not ReadOnly): AFC's
    // per-slot edges, single-unit Happy Hare's groups, and the mock. CFS and
    // AD5X IFS are read-only, so the row stays hidden there. The base
    // reset_endless_spool() re-checks this and rejects if it moved, so a
    // stale button that won the race just reports the refusal.
    lv_subject_set_int(&can_reset_endless_spool_subject_,
                       backend->get_endless_spool_capabilities().editable() ? 1 : 0);

    // Update status
    AmsAction action = backend->get_current_action();
    const char* status_str = action_to_string(static_cast<int>(action));
    snprintf(status_buf_, sizeof(status_buf_), "%s", status_str);
    lv_subject_copy_string(&status_subject_, status_buf_);

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
    std::sort(cached_sections_.begin(), cached_sections_.end(),
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

/// Map section ID to icon name (UI concern — backends don't specify icons)
static const char* section_icon_for_id(const std::string& id) {
    // Ordered by expected frequency
    if (id == "setup")
        return "cog";
    if (id == "speed")
        return "speed_up";
    if (id == "maintenance")
        return "wrench";
    if (id == "hub")
        return "source_branch";
    if (id == "tip_forming")
        return "thermometer";
    if (id == "purge")
        return "water";
    if (id == "toolhead")
        return "filament";
    if (id == "config")
        return "cog";
    return "cog"; // fallback for unknown sections
}

void AmsDeviceOperationsOverlay::create_section_row(lv_obj_t* parent,
                                                    const helix::printer::DeviceSection& section) {
    const char* icon = section_icon_for_id(section.id);

    // Reuse the standard setting_action_row XML component
    const char* attrs[] = {"label",
                           lv_tr(section.label.c_str()),
                           "label_tag",
                           section.label.c_str(),
                           "icon",
                           icon,
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

// ============================================================================
// ACTION TO STRING
// ============================================================================

const char* AmsDeviceOperationsOverlay::action_to_string(int action) {
    switch (static_cast<AmsAction>(action)) {
    case AmsAction::IDLE:
        return lv_tr("Idle");
    case AmsAction::LOADING:
        return lv_tr("Loading filament...");
    case AmsAction::UNLOADING:
        return lv_tr("Unloading filament...");
    case AmsAction::SELECTING:
        return lv_tr("Selecting slot...");
    case AmsAction::RESETTING:
        return lv_tr("Resetting...");
    case AmsAction::FORMING_TIP:
        return lv_tr("Forming tip...");
    case AmsAction::CUTTING:
        return lv_tr("Cutting filament...");
    case AmsAction::HEATING:
        return lv_tr("Heating...");
    case AmsAction::CHECKING:
        return lv_tr("Checking slots...");
    case AmsAction::PAUSED:
        return lv_tr("Paused (attention needed)");
    case AmsAction::ERROR:
        return lv_tr("Error state");
    default:
        return lv_tr("Unknown");
    }
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void AmsDeviceOperationsOverlay::on_home_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceOperationsOverlay] on_home_clicked");
    LV_UNUSED(e);

    spdlog::info("[AmsDeviceOperationsOverlay] Home button clicked");

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING("{}", lv_tr("No Multi-Filament System connected"));
    } else {
        AmsError result = backend->reset();
        if (result.success()) {
            NOTIFY_INFO("{}", lv_tr("Homing AFC system..."));
        } else {
            helix::ui::notify_ams_error(result, lv_tr("Home failed"));
        }
        get_ams_device_operations_overlay().refresh();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceOperationsOverlay::on_recover_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceOperationsOverlay] on_recover_clicked");
    LV_UNUSED(e);

    spdlog::info("[AmsDeviceOperationsOverlay] Recover button clicked");

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING("{}", lv_tr("No Multi-Filament System connected"));
    } else {
        AmsError result = backend->recover();
        if (result.success()) {
            NOTIFY_INFO("{}", lv_tr("Recovering AFC system..."));
        } else {
            helix::ui::notify_ams_error(result, lv_tr("Recovery failed"));
        }
        get_ams_device_operations_overlay().refresh();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceOperationsOverlay::on_abort_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceOperationsOverlay] on_abort_clicked");
    LV_UNUSED(e);

    spdlog::info("[AmsDeviceOperationsOverlay] Abort button clicked");

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING("{}", lv_tr("No Multi-Filament System connected"));
    } else {
        AmsError result = backend->cancel();
        if (result.success()) {
            NOTIFY_INFO("{}", lv_tr("Aborting AFC operation..."));
        } else {
            helix::ui::notify_ams_error(result, lv_tr("Abort failed"));
        }
        get_ams_device_operations_overlay().refresh();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceOperationsOverlay::on_bypass_toggled(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceOperationsOverlay] on_bypass_toggled");

    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!toggle || !lv_obj_is_valid(toggle)) {
        spdlog::warn("[AmsDeviceOperationsOverlay] Stale callback - toggle no longer valid");
    } else {
        // The switch flips its own CHECKED state before this runs, so the widget
        // is not the authority on intent — the controller reads the backend, the
        // same way the sidebar toggle and the home tile do. It owns the print
        // guard, the hardware-sensor refusal and the #1229 unload-first chain,
        // none of which this handler had while it called the backend directly.
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
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceOperationsOverlay::on_afc_unload_after_print_toggled(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceOperationsOverlay] on_afc_unload_after_print_toggled");

    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!toggle || !lv_obj_is_valid(toggle)) {
        spdlog::warn("[AmsDeviceOperationsOverlay] Stale callback - toggle no longer valid");
    } else {
        bool is_checked = lv_obj_has_state(toggle, LV_STATE_CHECKED);
        spdlog::info("[AmsDeviceOperationsOverlay] AFC unload-after-print toggle: {}",
                     is_checked ? "enabled" : "disabled");
        SettingsManager::instance().set_afc_unload_after_print(is_checked);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceOperationsOverlay::on_always_show_bypass_spool_toggled(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceOperationsOverlay] on_always_show_bypass_spool_toggled");

    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!toggle || !lv_obj_is_valid(toggle)) {
        spdlog::warn("[AmsDeviceOperationsOverlay] Stale callback - toggle no longer valid");
    } else {
        bool is_checked = lv_obj_has_state(toggle, LV_STATE_CHECKED);
        spdlog::info("[AmsDeviceOperationsOverlay] Always-show-bypass-spool toggle: {}",
                     is_checked ? "enabled" : "disabled");
        SettingsManager::instance().set_ams_always_show_bypass_spool(is_checked);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceOperationsOverlay::on_keep_spool_info_toggled(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceOperationsOverlay] on_keep_spool_info_toggled");

    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!toggle || !lv_obj_is_valid(toggle)) {
        spdlog::warn("[AmsDeviceOperationsOverlay] Stale callback - toggle no longer valid");
    } else {
        bool is_checked = lv_obj_has_state(toggle, LV_STATE_CHECKED);
        spdlog::info("[AmsDeviceOperationsOverlay] Keep-spool-info-on-eject toggle: {}",
                     is_checked ? "enabled" : "disabled");
        SettingsManager::instance().set_ams_keep_spool_info_on_eject(is_checked);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceOperationsOverlay::on_force_bypass_controls_toggled(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceOperationsOverlay] on_force_bypass_controls_toggled");

    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!toggle || !lv_obj_is_valid(toggle)) {
        spdlog::warn("[AmsDeviceOperationsOverlay] Stale callback - toggle no longer valid");
    } else {
        bool is_checked = lv_obj_has_state(toggle, LV_STATE_CHECKED);
        spdlog::info("[AmsDeviceOperationsOverlay] Force-bypass-controls toggle: {}",
                     is_checked ? "enabled" : "disabled");
        SettingsManager::instance().set_ams_force_bypass_controls(is_checked);
        // Both gating subjects are recomputed from the backend rather than from
        // the setting, so neither moves on its own when the override flips.
        // AmsState drives the sidebar toggle and the path node; this overlay
        // drives its own section.
        AmsState::instance().sync_from_backend();
        get_ams_device_operations_overlay().update_from_backend();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceOperationsOverlay::on_qidi_eject_distance_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceOperationsOverlay] on_qidi_eject_distance_changed");

    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!slider || !lv_obj_is_valid(slider)) {
        spdlog::warn("[AmsDeviceOperationsOverlay] Stale callback - eject distance slider invalid");
    } else {
        int value = lv_slider_get_value(slider);
        spdlog::info("[AmsDeviceOperationsOverlay] QIDI eject distance: {} mm", value);
        SettingsManager::instance().set_qidi_eject_distance(value);

        auto& overlay = get_ams_device_operations_overlay();
        snprintf(overlay.qidi_eject_distance_buf_, sizeof(overlay.qidi_eject_distance_buf_),
                 "%d mm", SettingsManager::instance().get_qidi_eject_distance());
        lv_subject_copy_string(&overlay.qidi_eject_distance_display_subject_,
                               overlay.qidi_eject_distance_buf_);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceOperationsOverlay::on_qidi_eject_velocity_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceOperationsOverlay] on_qidi_eject_velocity_changed");

    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!slider || !lv_obj_is_valid(slider)) {
        spdlog::warn("[AmsDeviceOperationsOverlay] Stale callback - eject velocity slider invalid");
    } else {
        int value = lv_slider_get_value(slider);
        spdlog::info("[AmsDeviceOperationsOverlay] QIDI eject velocity: {} mm/s", value);
        SettingsManager::instance().set_qidi_eject_velocity(value);

        auto& overlay = get_ams_device_operations_overlay();
        snprintf(overlay.qidi_eject_velocity_buf_, sizeof(overlay.qidi_eject_velocity_buf_),
                 "%d mm/s", SettingsManager::instance().get_qidi_eject_velocity());
        lv_subject_copy_string(&overlay.qidi_eject_velocity_display_subject_,
                               overlay.qidi_eject_velocity_buf_);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceOperationsOverlay::on_section_row_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceOperationsOverlay] on_section_row_clicked");

    auto* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!row || !lv_obj_is_valid(row)) {
        spdlog::warn("[AmsDeviceOperationsOverlay] on_section_row_clicked: invalid target");
    } else {
        auto& overlay = get_ams_device_operations_overlay();
        auto index = reinterpret_cast<size_t>(lv_obj_get_user_data(row));

        if (index >= overlay.cached_sections_.size()) {
            spdlog::warn("[AmsDeviceOperationsOverlay] Invalid section index: {}", index);
        } else {
            const auto& section = overlay.cached_sections_[index];
            spdlog::info("[AmsDeviceOperationsOverlay] Section clicked: {} ('{}')", section.id,
                         section.label);

            // Push the detail overlay for this section
            auto& detail = get_ams_device_section_detail_overlay();
            detail.show(overlay.parent_screen_, section.id, section.label);
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceOperationsOverlay::on_reset_endless_spool_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceOperationsOverlay] on_reset_endless_spool_clicked");
    LV_UNUSED(e);

    spdlog::info("[AmsDeviceOperationsOverlay] Reset Endless Spool button clicked");

    // The reset wipes ALL failover config, so it needs a confirmation, not a
    // bare tap. on_confirm re-fetches the backend so it cannot dangle if the
    // panel/backend changed while the dialog was open, and dismisses the dialog
    // itself (a custom on_confirm replaces the default close handler).
    //
    // Both outcomes are announced. refresh() only re-derives
    // can_reset_endless_spool_subject_ from editable(), which a reset does not
    // change, and this overlay renders no endless-spool assignments at all (the
    // backup arrows live on AmsPanel and are not refreshed from here) — so
    // without a toast, wiping every spool's failover looks exactly like a no-op.
    helix::ui::modal_show_confirmation(
        lv_tr("Reset Endless Spool?"),
        lv_tr("This clears every spool's failover assignment. The print will stop on runout "
              "until you set up failover again."),
        ModalSeverity::Warning, lv_tr("Reset"),
        +[](lv_event_t* /*e*/) {
            AmsBackend* b = AmsState::instance().get_backend();
            if (b) {
                AmsError result = b->reset_endless_spool();
                if (!result.success()) {
                    helix::ui::notify_ams_error(result, lv_tr("Reset endless spool failed"));
                } else {
                    NOTIFY_INFO("{}", lv_tr("Endless spool failover cleared for every slot"));
                }
                get_ams_device_operations_overlay().refresh();
            }
            helix::ui::modal_hide(helix::ui::modal_get_top());
        },
        /*on_cancel*/ nullptr, /*user_data*/ nullptr);

    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui
