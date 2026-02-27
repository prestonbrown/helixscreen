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
#include "ui_nav_manager.h"
#include "ui_status_pill.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "lvgl/src/others/translation/lv_translation.h"
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
        lv_subject_deinit(&bypass_active_subject_);
        lv_subject_deinit(&hw_bypass_sensor_subject_);
        lv_subject_deinit(&supports_auto_heat_subject_);
        lv_subject_deinit(&has_backend_subject_);
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

    lv_subject_init_int(&bypass_active_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_bypass_active", &bypass_active_subject_);

    lv_subject_init_int(&hw_bypass_sensor_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_hw_bypass_sensor", &hw_bypass_sensor_subject_);

    lv_subject_init_int(&supports_auto_heat_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_supports_auto_heat",
                            &supports_auto_heat_subject_);

    lv_subject_init_int(&has_backend_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_device_ops_has_backend", &has_backend_subject_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void AmsDeviceOperationsOverlay::register_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_ams_device_ops_home", on_home_clicked);
    lv_xml_register_event_cb(nullptr, "on_ams_device_ops_recover", on_recover_clicked);
    lv_xml_register_event_cb(nullptr, "on_ams_device_ops_abort", on_abort_clicked);
    lv_xml_register_event_cb(nullptr, "on_ams_device_ops_bypass_toggled", on_bypass_toggled);
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
        lv_subject_set_int(&bypass_active_subject_, 0);
        lv_subject_set_int(&hw_bypass_sensor_subject_, 0);
        lv_subject_set_int(&supports_auto_heat_subject_, 0);
        system_info_buf_[0] = '\0';
        lv_subject_copy_string(&system_info_subject_, system_info_buf_);
        snprintf(status_buf_, sizeof(status_buf_), "No AMS connected");
        lv_subject_copy_string(&status_subject_, status_buf_);

        if (section_list_container_) {
            lv_obj_clean(section_list_container_);
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
    lv_subject_set_int(&supports_bypass_subject_, info.supports_bypass ? 1 : 0);
    lv_subject_set_int(&bypass_active_subject_, backend->is_bypass_active() ? 1 : 0);
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

    lv_obj_clean(section_list_container_);
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
        return "filament";
    if (id == "tip_forming")
        return "thermometer";
    if (id == "purge")
        return "water";
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
        return "Loading filament...";
    case AmsAction::UNLOADING:
        return "Unloading filament...";
    case AmsAction::SELECTING:
        return "Selecting slot...";
    case AmsAction::RESETTING:
        return "Resetting...";
    case AmsAction::FORMING_TIP:
        return "Forming tip...";
    case AmsAction::CUTTING:
        return "Cutting filament...";
    case AmsAction::HEATING:
        return lv_tr("Heating...");
    case AmsAction::CHECKING:
        return "Checking slots...";
    case AmsAction::PAUSED:
        return "Paused (attention needed)";
    case AmsAction::ERROR:
        return "Error state";
    default:
        return "Unknown";
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
        NOTIFY_WARNING("{}", lv_tr("No AMS system connected"));
    } else {
        AmsError result = backend->reset();
        if (result.success()) {
            NOTIFY_INFO("{}", lv_tr("Homing AFC system..."));
        } else {
            NOTIFY_ERROR("{}: {}", lv_tr("Home failed"), result.user_msg);
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
        NOTIFY_WARNING("{}", lv_tr("No AMS system connected"));
    } else {
        AmsError result = backend->recover();
        if (result.success()) {
            NOTIFY_INFO("{}", lv_tr("Recovering AFC system..."));
        } else {
            NOTIFY_ERROR("{}: {}", lv_tr("Recovery failed"), result.user_msg);
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
        NOTIFY_WARNING("{}", lv_tr("No AMS system connected"));
    } else {
        AmsError result = backend->cancel();
        if (result.success()) {
            NOTIFY_INFO("{}", lv_tr("Aborting AFC operation..."));
        } else {
            NOTIFY_ERROR("{}: {}", lv_tr("Abort failed"), result.user_msg);
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
        // Guard: hardware sensor controls bypass — toggle should be hidden but check anyway
        AmsBackend* backend_check = AmsState::instance().get_backend();
        if (backend_check && backend_check->get_system_info().has_hardware_bypass_sensor) {
            NOTIFY_WARNING("{}", lv_tr("Bypass controlled by hardware sensor"));
            return;
        }

        bool is_checked = lv_obj_has_state(toggle, LV_STATE_CHECKED);

        spdlog::info("[AmsDeviceOperationsOverlay] Bypass toggle: {}",
                     is_checked ? "enabled" : "disabled");

        AmsBackend* backend = AmsState::instance().get_backend();
        if (backend) {
            AmsError result;
            if (is_checked) {
                result = backend->enable_bypass();
            } else {
                result = backend->disable_bypass();
            }

            if (result.success()) {
                spdlog::info("[AmsDeviceOperationsOverlay] Bypass mode {}",
                             is_checked ? "enabled" : "disabled");
                lv_subject_set_int(&get_ams_device_operations_overlay().bypass_active_subject_,
                                   is_checked ? 1 : 0);
            } else {
                spdlog::error("[AmsDeviceOperationsOverlay] Failed to {} bypass: {}",
                              is_checked ? "enable" : "disable", result.user_msg);
                if (is_checked) {
                    lv_obj_remove_state(toggle, LV_STATE_CHECKED);
                } else {
                    lv_obj_add_state(toggle, LV_STATE_CHECKED);
                }
            }
        } else {
            spdlog::error("[AmsDeviceOperationsOverlay] No backend available for bypass toggle");
            if (is_checked) {
                lv_obj_remove_state(toggle, LV_STATE_CHECKED);
            } else {
                lv_obj_add_state(toggle, LV_STATE_CHECKED);
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceOperationsOverlay::on_section_row_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceOperationsOverlay] on_section_row_clicked");

    auto* row = static_cast<lv_obj_t*>(lv_event_get_target(e));
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

} // namespace helix::ui
