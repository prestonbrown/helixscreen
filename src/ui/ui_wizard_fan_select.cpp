// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_fan_select.h"

#include "ui_error_reporting.h"
#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_wizard.h"
#include "ui_wizard_hardware_selector.h"
#include "ui_wizard_helpers.h"

#include "app_globals.h"
#include "config.h"
#include "i_moonraker_api.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_hardware.h"
#include "static_panel_registry.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace helix;

// Global wizard subject for Next button state (defined in ui_wizard.cpp)
extern lv_subject_t connection_test_passed;

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<WizardFanSelectStep> g_wizard_fan_select_step;

WizardFanSelectStep* get_wizard_fan_select_step() {
    if (!g_wizard_fan_select_step) {
        g_wizard_fan_select_step = std::make_unique<WizardFanSelectStep>();
        StaticPanelRegistry::instance().register_destroy(
            "WizardFanSelectStep", []() { g_wizard_fan_select_step.reset(); });
    }
    return g_wizard_fan_select_step.get();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardFanSelectStep::WizardFanSelectStep() {
    spdlog::debug("[{}] Instance created", get_name());
}

WizardFanSelectStep::~WizardFanSelectStep() {
    // NOTE: Do NOT call LVGL functions here - LVGL may be destroyed first
    // NOTE: Do NOT log here - spdlog may be destroyed first
    screen_root_ = nullptr;
}

// ============================================================================

// ============================================================================
// Subject Initialization
// ============================================================================

void WizardFanSelectStep::init_subjects() {
    spdlog::debug("[{}] Initializing subjects", get_name());

    // Initialize subjects with default index 0
    // Actual selection will be restored from config during create() after hardware is discovered
    helix::ui::wizard::init_int_subject(&hotend_fan_selected_, 0, "hotend_fan_selected");
    helix::ui::wizard::init_int_subject(&part_fan_selected_, 0, "part_fan_selected");
    helix::ui::wizard::init_int_subject(&chamber_fan_selected_, 0, "chamber_fan_selected");
    helix::ui::wizard::init_int_subject(&exhaust_fan_selected_, 0, "exhaust_fan_selected");

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

// ============================================================================
// Static Callbacks (XML event_cb pattern)
// ============================================================================

/// Updates the Next button state and error message based on current validation (no duplicates)
static void update_next_button_state() {
    auto* step = get_wizard_fan_select_step();
    if (!step)
        return;

    bool valid = step->is_validated();
    lv_subject_set_int(&connection_test_passed, valid ? 1 : 0);

    // Update status text visibility and content
    lv_obj_t* screen = step->get_screen_root();
    if (screen) {
        lv_obj_t* status_text = lv_obj_find_by_name(screen, "fan_status_text");
        if (status_text) {
            if (valid) {
                lv_obj_add_flag(status_text, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_label_set_text(status_text, lv_tr("Each fan can only be selected once"));
                lv_obj_remove_flag(status_text, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    spdlog::debug("[WizardFanSelectStep] Validation state: {} -> Next button {}",
                  valid ? "valid" : "invalid", valid ? "enabled" : "disabled");
}

static void on_hotend_fan_dropdown_changed(lv_event_t* e) {
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    auto* step = get_wizard_fan_select_step();
    if (step) {
        lv_subject_set_int(step->get_hotend_fan_subject(), index);
        spdlog::debug("[WizardFanSelectStep] Hotend fan selection changed to index {}", index);
        update_next_button_state();
    }
}

static void on_part_fan_dropdown_changed(lv_event_t* e) {
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    auto* step = get_wizard_fan_select_step();
    if (step) {
        lv_subject_set_int(step->get_part_fan_subject(), index);
        spdlog::debug("[WizardFanSelectStep] Part fan selection changed to index {}", index);
        update_next_button_state();
    }
}

static void on_chamber_fan_dropdown_changed(lv_event_t* e) {
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    auto* step = get_wizard_fan_select_step();
    if (step) {
        lv_subject_set_int(step->get_chamber_fan_subject(), index);
        spdlog::debug("[WizardFanSelectStep] Chamber fan selection changed to index {}", index);
        update_next_button_state();
    }
}

static void on_exhaust_fan_dropdown_changed(lv_event_t* e) {
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    auto* step = get_wizard_fan_select_step();
    if (step) {
        lv_subject_set_int(step->get_exhaust_fan_subject(), index);
        spdlog::debug("[WizardFanSelectStep] Exhaust fan selection changed to index {}", index);
        update_next_button_state();
    }
}

// ============================================================================
// Callback Registration
// ============================================================================

void WizardFanSelectStep::register_callbacks() {
    // Register XML event callbacks for dropdown value changes
    lv_xml_register_event_cb(nullptr, "on_hotend_fan_dropdown_changed",
                             on_hotend_fan_dropdown_changed);
    lv_xml_register_event_cb(nullptr, "on_part_fan_dropdown_changed", on_part_fan_dropdown_changed);
    lv_xml_register_event_cb(nullptr, "on_chamber_fan_dropdown_changed",
                             on_chamber_fan_dropdown_changed);
    lv_xml_register_event_cb(nullptr, "on_exhaust_fan_dropdown_changed",
                             on_exhaust_fan_dropdown_changed);
    spdlog::debug("[{}] Registered dropdown callbacks", get_name());
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardFanSelectStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating fan select screen", get_name());

    // Safety check: cleanup should have been called by wizard navigation
    if (screen_root_) {
        spdlog::warn("[{}] Screen pointer not null - cleanup may not have been called properly",
                     get_name());
        screen_root_ = nullptr; // Reset pointer, wizard framework handles deletion
    }

    // Create screen from XML
    screen_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_fan_select", nullptr));
    if (!screen_root_) {
        LOG_ERROR_INTERNAL("[{}] Failed to create screen from XML", get_name());
        NOTIFY_ERROR(lv_tr("Failed to load fan configuration screen"));
        return nullptr;
    }

    // Get Moonraker API for hardware discovery
    IMoonrakerAPI* api = get_moonraker_api();

    // Build hotend fan options with custom filter (heater_fan OR hotend_fan)
    hotend_fan_items_.clear();
    if (api) {
        const auto& fans = api->hardware().fans();
        for (const auto& fan : fans) {
            if (fan.find("heater_fan") != std::string::npos ||
                fan.find("hotend_fan") != std::string::npos) {
                hotend_fan_items_.push_back(fan);
            }
        }
    }

    // Build dropdown options string with "None" option and friendly display names
    std::string hotend_options_str = helix::ui::wizard::build_dropdown_options(
        hotend_fan_items_, nullptr, true, helix::DeviceType::FAN);

    // Add "None" FIRST in items vector to match dropdown order
    hotend_fan_items_.insert(hotend_fan_items_.begin(), "None");

    // Build part cooling fan options with custom filter (has "fan" but NOT heater/hotend)
    part_fan_items_.clear();
    if (api) {
        const auto& fans = api->hardware().fans();
        for (const auto& fan : fans) {
            if (fan.find("fan") != std::string::npos &&
                fan.find("heater_fan") == std::string::npos &&
                fan.find("hotend_fan") == std::string::npos) {
                part_fan_items_.push_back(fan);
            }
        }
    }

    // Build dropdown options string with "None" option and friendly display names
    std::string part_options_str = helix::ui::wizard::build_dropdown_options(
        part_fan_items_, nullptr, true, helix::DeviceType::FAN);

    // Add "None" FIRST in items vector to match dropdown order
    part_fan_items_.insert(part_fan_items_.begin(), "None");

    // Create PrinterHardware for guessing
    std::unique_ptr<PrinterHardware> hw;
    if (api) {
        hw = std::make_unique<PrinterHardware>(api->hardware().heaters(), api->hardware().sensors(),
                                               api->hardware().fans(), api->hardware().leds());
    }

    // Find and configure hotend fan dropdown
    // Event handler is wired via XML <event_cb>
    lv_obj_t* hotend_dropdown = lv_obj_find_by_name(screen_root_, "hotend_fan_dropdown");
    if (hotend_dropdown) {
        lv_dropdown_set_options(hotend_dropdown, hotend_options_str.c_str());
        helix::ui::wizard::restore_dropdown_selection(
            hotend_dropdown, &hotend_fan_selected_, hotend_fan_items_, helix::wizard::HOTEND_FAN,
            hw.get(), [](const PrinterHardware& h) { return h.guess_hotend_fan(); },
            "[Wizard Fan]");
    }

    // Find and configure part fan dropdown
    // Event handler is wired via XML <event_cb>
    lv_obj_t* part_dropdown = lv_obj_find_by_name(screen_root_, "part_cooling_fan_dropdown");
    if (part_dropdown) {
        lv_dropdown_set_options(part_dropdown, part_options_str.c_str());
        helix::ui::wizard::restore_dropdown_selection(
            part_dropdown, &part_fan_selected_, part_fan_items_, helix::wizard::PART_FAN, hw.get(),
            [](const PrinterHardware& h) { return h.guess_part_cooling_fan(); }, "[Wizard Fan]");
    }

    // Check if we should show optional fans row (only if > 2 fans discovered)
    size_t fan_count = api ? api->hardware().fans().size() : 0;
    bool show_optional_fans = fan_count > 2;

    lv_obj_t* optional_row = lv_obj_find_by_name(screen_root_, "optional_fans_row");
    if (!show_optional_fans) {
        if (optional_row) {
            lv_obj_add_flag(optional_row, LV_OBJ_FLAG_HIDDEN);
        }
        spdlog::debug("[{}] Only {} fans, hiding optional fan row", get_name(), fan_count);
    } else {
        // Build chamber fan options - show ALL fans
        chamber_fan_items_.clear();
        if (api) {
            const auto& fans = api->hardware().fans();
            for (const auto& fan : fans) {
                chamber_fan_items_.push_back(fan);
            }
        }

        // Build dropdown options string with "None" option and friendly display names
        std::string chamber_options_str = helix::ui::wizard::build_dropdown_options(
            chamber_fan_items_, nullptr, true, helix::DeviceType::FAN);

        // Add "None" FIRST in items vector to match dropdown order
        chamber_fan_items_.insert(chamber_fan_items_.begin(), "None");

        // Build exhaust fan options - show ALL fans
        exhaust_fan_items_.clear();
        if (api) {
            const auto& fans = api->hardware().fans();
            for (const auto& fan : fans) {
                exhaust_fan_items_.push_back(fan);
            }
        }

        // Build dropdown options string with "None" option and friendly display names
        std::string exhaust_options_str = helix::ui::wizard::build_dropdown_options(
            exhaust_fan_items_, nullptr, true, helix::DeviceType::FAN);

        // Add "None" FIRST in items vector to match dropdown order
        exhaust_fan_items_.insert(exhaust_fan_items_.begin(), "None");

        // Find and configure chamber fan dropdown
        lv_obj_t* chamber_dropdown = lv_obj_find_by_name(screen_root_, "chamber_fan_dropdown");
        if (chamber_dropdown) {
            lv_dropdown_set_options(chamber_dropdown, chamber_options_str.c_str());
            helix::ui::wizard::restore_dropdown_selection(
                chamber_dropdown, &chamber_fan_selected_, chamber_fan_items_,
                helix::wizard::CHAMBER_FAN, hw.get(),
                [](const PrinterHardware& h) { return h.guess_chamber_fan(); }, "[Wizard Fan]");
        }

        // Find and configure exhaust fan dropdown
        lv_obj_t* exhaust_dropdown = lv_obj_find_by_name(screen_root_, "exhaust_fan_dropdown");
        if (exhaust_dropdown) {
            lv_dropdown_set_options(exhaust_dropdown, exhaust_options_str.c_str());
            helix::ui::wizard::restore_dropdown_selection(
                exhaust_dropdown, &exhaust_fan_selected_, exhaust_fan_items_,
                helix::wizard::EXHAUST_FAN, hw.get(),
                [](const PrinterHardware& h) { return h.guess_exhaust_fan(); }, "[Wizard Fan]");
        }

        spdlog::debug("[{}] {} fans discovered, showing optional fan row", get_name(), fan_count);
    }

    // Update Next button state based on initial validation
    // (may be invalid if auto-detection caused duplicate selections)
    update_next_button_state();

    spdlog::debug("[{}] Screen created successfully", get_name());
    return screen_root_;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardFanSelectStep::cleanup() {
    spdlog::debug("[{}] Cleaning up resources", get_name());

    // Save current selections to config before cleanup (deferred save pattern)
    helix::ui::wizard::save_dropdown_selection(&hotend_fan_selected_, hotend_fan_items_,
                                               helix::wizard::HOTEND_FAN, "[Wizard Fan]");

    helix::ui::wizard::save_dropdown_selection(&part_fan_selected_, part_fan_items_,
                                               helix::wizard::PART_FAN, "[Wizard Fan]");

    // Save optional fan selections if they were populated
    if (!chamber_fan_items_.empty()) {
        helix::ui::wizard::save_dropdown_selection(&chamber_fan_selected_, chamber_fan_items_,
                                                   helix::wizard::CHAMBER_FAN, "[Wizard Fan]");
    }

    if (!exhaust_fan_items_.empty()) {
        helix::ui::wizard::save_dropdown_selection(&exhaust_fan_selected_, exhaust_fan_items_,
                                                   helix::wizard::EXHAUST_FAN, "[Wizard Fan]");
    }

    // Persist to disk
    Config* config = Config::get_instance();
    if (config) {
        if (!config->save()) {
            NOTIFY_ERROR(lv_tr("Failed to save fan configuration"));
        }
    }

    // Reset Next button state to enabled for other wizard steps
    lv_subject_set_int(&connection_test_passed, 1);

    // Reset UI references
    // Note: Do NOT call lv_obj_del() here - the wizard framework handles
    // object deletion when clearing wizard_content container
    screen_root_ = nullptr;

    spdlog::debug("[{}] Cleanup complete", get_name());
}

// ============================================================================
// Validation
// ============================================================================

bool WizardFanSelectStep::is_validated() const {
    // Check for duplicate selections across all 4 fans
    std::vector<std::string> selected;

    auto add_if_valid = [&](lv_subject_t* subject, const std::vector<std::string>& items) {
        int idx = lv_subject_get_int(const_cast<lv_subject_t*>(subject));
        if (idx >= 0 && static_cast<size_t>(idx) < items.size() && items[idx] != "None") {
            selected.push_back(items[idx]);
        }
    };

    add_if_valid(const_cast<lv_subject_t*>(&hotend_fan_selected_), hotend_fan_items_);
    add_if_valid(const_cast<lv_subject_t*>(&part_fan_selected_), part_fan_items_);

    // Only check chamber/exhaust if they were populated
    if (!chamber_fan_items_.empty()) {
        add_if_valid(const_cast<lv_subject_t*>(&chamber_fan_selected_), chamber_fan_items_);
    }
    if (!exhaust_fan_items_.empty()) {
        add_if_valid(const_cast<lv_subject_t*>(&exhaust_fan_selected_), exhaust_fan_items_);
    }

    // Check for duplicates
    std::set<std::string> unique(selected.begin(), selected.end());
    bool no_duplicates = unique.size() == selected.size();

    if (!no_duplicates) {
        spdlog::debug("[{}] Validation failed: duplicate fan selections detected", get_name());
    }

    return no_duplicates;
}
