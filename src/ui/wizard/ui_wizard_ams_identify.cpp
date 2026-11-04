// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_ams_identify.h"

#include "ams_state.h"
#include "ams_types.h"
#include "core/static_panel_registry.h"
#include "i18n_plural.h"
#include "lvgl/lvgl.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

// Static buffer definitions for subject strings
char WizardAmsIdentifyStep::ams_type_buffer_[64] = {};
char WizardAmsIdentifyStep::ams_details_buffer_[128] = {};

// ============================================================================
// Global Instance
// ============================================================================

WizardAmsIdentifyStep* get_wizard_ams_identify_step() {
    return &helix::lazy_global<WizardAmsIdentifyStep>("WizardAmsIdentifyStep");
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardAmsIdentifyStep::WizardAmsIdentifyStep() {
    spdlog::debug("[{}] Instance created", get_name());
}

WizardAmsIdentifyStep::~WizardAmsIdentifyStep() {
    // CRITICAL: Deinitialize subjects BEFORE they're destroyed
    // This prevents use-after-free when widgets with bindings are deleted
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    // NOTE: Do NOT log here - spdlog may be destroyed first
    screen_root_ = nullptr;
}

// ============================================================================

// ============================================================================
// Subject Initialization
// ============================================================================

void WizardAmsIdentifyStep::init_subjects() {
    spdlog::debug("[{}] Initializing subjects", get_name());

    // Initialize string subjects with buffers and register with XML system and SubjectManager
    UI_MANAGED_SUBJECT_STRING(wizard_ams_type_, ams_type_buffer_, "Unknown", "wizard_ams_type",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(wizard_ams_details_, ams_details_buffer_, "", "wizard_ams_details",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(wizard_ams_logo_color_, 0, "wizard_ams_logo_color", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

// ============================================================================
// Callback Registration (no-op for this step)
// ============================================================================

void WizardAmsIdentifyStep::register_callbacks() {
    spdlog::debug("[{}] Register callbacks (no-op)", get_name());
    // No callbacks needed - this is a display-only step
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardAmsIdentifyStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating AMS identify screen", get_name());

    // Safety check: cleanup should have been called by wizard navigation
    if (screen_root_) {
        spdlog::warn("[{}] Screen pointer not null - cleanup may not have been called properly",
                     get_name());
        screen_root_ = nullptr; // Reset pointer, wizard framework handles deletion
    }

    // Create screen from XML
    screen_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_ams_identify", nullptr));
    if (!screen_root_) {
        spdlog::error("[{}] Failed to create screen from XML", get_name());
        return nullptr;
    }

    // Update display with current AMS info
    update_display();

    spdlog::debug("[{}] Screen created successfully", get_name());
    return screen_root_;
}

// ============================================================================
// Display Update
// ============================================================================

void WizardAmsIdentifyStep::update_display() {
    if (!screen_root_) {
        return;
    }

    auto& ams = helix::AmsState::instance();
    helix::AmsBackend* backend = ams.get_backend();

    // Update type via subject (reactive binding)
    std::string type_name = get_ams_type_name();
    lv_subject_copy_string(&wizard_ams_type_, type_name.c_str());
    spdlog::debug("[{}] Set type subject: {}", get_name(), type_name);

    // Update details via subject (reactive binding)
    std::string details = get_ams_details();
    lv_subject_copy_string(&wizard_ams_details_, details.c_str());
    spdlog::debug("[{}] Set details subject: {}", get_name(), details);

    // Set logo image (imperative - images don't support bind_src)
    lv_obj_t* logo = helix::ui::find_required(screen_root_, "ams_logo", get_name());
    if (logo && backend) {
        const helix::LogoArt art = helix::AmsState::get_logo(backend->get_system_info().type_name);
        const char* logo_path = art.path;
        lv_subject_set_int(&wizard_ams_logo_color_, art.color ? 1 : 0);
        if (logo_path && logo_path[0] != '\0') {
            lv_image_set_src(logo, logo_path);
            lv_obj_remove_flag(logo, LV_OBJ_FLAG_HIDDEN);
            spdlog::debug("[{}] Set logo: {}", get_name(), logo_path);
        } else {
            lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);
            spdlog::debug("[{}] No logo for type: {}", get_name(),
                          backend->get_system_info().type_name);
        }
    }
}

std::string WizardAmsIdentifyStep::get_ams_type_name() const {
    auto& ams = helix::AmsState::instance();
    helix::AmsBackend* backend = ams.get_backend();

    if (!backend) {
        return lv_tr("Unknown");
    }

    const helix::AmsTypeTraits& traits = helix::ams_type_traits(backend->get_type());
    if (traits.type == helix::AmsType::NONE) {
        return lv_tr("Unknown");
    }
    return traits.product_name;
}

std::string WizardAmsIdentifyStep::get_ams_details() const {
    auto& ams = helix::AmsState::instance();
    helix::AmsBackend* backend = ams.get_backend();

    if (!backend) {
        return lv_tr("System detected");
    }

    helix::AmsSystemInfo info = backend->get_system_info();
    std::string details;

    // Start with slot count if available
    if (const int bays = info.present_slot_count(); bays > 0) {
        details = fmt::format(
            fmt::runtime(helix::ui::tr_plural(nullptr, "{} slots", static_cast<uint64_t>(bays))),
            bays);
    }

    // Add unit name if available (e.g., "• Turtle 1")
    if (!info.units.empty() && !info.units[0].name.empty()) {
        if (!details.empty()) {
            details += " • ";
        }
        // Prefer display_name (short, no type prefix), replace _ with spaces
        std::string uname =
            !info.units[0].display_name.empty() ? info.units[0].display_name : info.units[0].name;
        std::replace(uname.begin(), uname.end(), '_', ' ');
        details += uname;
    }

    return details.empty() ? std::string(lv_tr("System detected")) : details;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardAmsIdentifyStep::cleanup() {
    spdlog::debug("[{}] Cleaning up resources", get_name());

    // No config to save - this is a display-only step

    // Reset UI references
    // Note: Do NOT call lv_obj_del() here - the wizard framework handles
    // object deletion when clearing wizard_content container
    screen_root_ = nullptr;

    spdlog::debug("[{}] Cleanup complete", get_name());
}

// ============================================================================
// Validation
// ============================================================================

bool WizardAmsIdentifyStep::is_validated() const {
    // Always return true - this is a display-only step
    return true;
}

// ============================================================================
// Skip Logic
// ============================================================================

bool WizardAmsIdentifyStep::should_skip() const {
    auto& ams = helix::AmsState::instance();
    helix::AmsBackend* backend = ams.get_backend();

    // Skip if no backend available
    if (!backend) {
        spdlog::debug("[{}] No AMS backend, skipping step", get_name());
        return true;
    }

    helix::AmsType type = backend->get_type();
    // A backend whose hardware feeds only the external spool presents no
    // system, so there is nothing to identify (or to expect next session).
    bool skip = !backend->get_system_info().presents_filament_system();

    if (skip) {
        spdlog::info("[{}] No AMS detected (type=NONE), skipping step", get_name());
    } else {
        spdlog::debug("[{}] AMS detected (type={}), showing step", get_name(),
                      static_cast<int>(type));
    }

    return skip;
}
