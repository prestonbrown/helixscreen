// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_subject_name_withdrawal.cpp
 * @brief Subject names must leave the XML scope when their owner is torn down.
 *
 * prestonbrown/helixscreen#1538: objects freed mid-process by
 * StaticPanelRegistry::destroy_all() published names into the global XML
 * subject scope without withdrawing them, so the next lv_xml_create() that
 * bound one installed an observer inside freed storage. The fix moves
 * registration onto SubjectManager (names recorded at registration, withdrawn
 * before deinit) and hardens deinit_all() against the silent variant where a
 * subject was registered WITHOUT a name while still published under one.
 */

#include "ui_ams_device_operations_overlay.h"
#include "ui_settings_appearance.h"
#include "ui_settings_barcode_scanner.h"
#include "ui_settings_display.h"
#include "ui_settings_label_printer.h"
#include "ui_settings_material_temps.h"
#include "ui_spoolman_overlay.h"
#include "ui_wizard_input_shaper.h"
#include "ui_wizard_language_chooser.h"

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "subject_debug_registry.h"
#include "subject_managed_panel.h"

#include <lvgl/lvgl.h>

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace helix::ui {
SpoolmanOverlay& get_spoolman_overlay();
} // namespace helix::ui

WizardInputShaperStep* get_wizard_input_shaper_step();
WizardLanguageChooserStep* get_wizard_language_chooser_step();

namespace {

struct OverlayCase {
    const char* label;
    void (*init_fn)();
    std::vector<const char*> names;
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "SubjectManager withdraws a nameless registration that is published anyway",
                 "[subject-scope][1538]") {
    constexpr const char* kName = "nameless_trap_probe";

    lv_subject_t subject{};
    lv_subject_init_int(&subject, 0);

    // The silent variant: published under a name, handed to the manager
    // without it. The raw register plus the debug record model what a
    // previous caller left behind; the manager only sees the one-arg
    // registration.
    lv_xml_register_subject(nullptr, kName, &subject);
    SubjectDebugRegistry::instance().register_subject(&subject, kName, LV_SUBJECT_TYPE_INT,
                                                      __FILE__, __LINE__);

    {
        SubjectManager manager;
        manager.register_subject(&subject);
        manager.deinit_all();
    }

    // The withdrawal is what prevents the next lv_xml_create() from binding
    // freed storage; the debug registry must not answer for the pointer
    // either.
    CHECK(lv_xml_get_subject(nullptr, kName) == nullptr);
    CHECK(SubjectDebugRegistry::instance().lookup(&subject) == nullptr);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "SubjectManager deinit leaves a truly unnamed subject without crashing",
                 "[subject-scope][1538]") {
    // The conservative inventory also names subjects that were never
    // published (direct bindings, no scope entry). Teardown must keep
    // deinitializing those quietly.
    lv_subject_t subject{};
    lv_subject_init_int(&subject, 0);

    SubjectManager manager;
    manager.register_subject(&subject);
    manager.deinit_all();

    CHECK(lv_xml_get_subject(nullptr, "never_published_probe") == nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "Converted overlays withdraw their subject names on destroy_all",
                 "[subject-scope][1538]") {
    StaticPanelRegistry::instance().destroy_all();

    const OverlayCase cases[] = {
        {"Display",
         []() { helix::settings::get_display_settings_overlay().init_subjects(); },
         {"brightness_value"}},
        {"Appearance",
         []() { helix::settings::get_appearance_settings_overlay().init_subjects(); },
         {"theme_apply_disabled"}},
        {"MaterialTemps",
         []() { helix::settings::get_material_temps_overlay().init_subjects(); },
         {"material_editing", "material_edit_name", "material_edit_defaults",
          "material_has_macro"}},
        {"Spoolman",
         []() { helix::ui::get_spoolman_overlay().init_subjects(); },
         {"ams_spoolman_sync_enabled", "ams_spoolman_refresh_interval", "scanner_device_status"}},
        {"BarcodeScanner",
         []() { helix::ui::get_barcode_scanner_settings_overlay().init_subjects(); },
         {"scanner_bt_available", "scanner_bt_discovering", "scanner_keymap_index",
          "scanner_current_device_label"}},
        {"LabelPrinter",
         []() { helix::settings::get_label_printer_settings_overlay().init_subjects(); },
         {"bt_scanning", "test_printing", "ipp_selected"}},
        {"WizardLanguageChooser",
         []() {
             WizardLanguageChooserStep* step = get_wizard_language_chooser_step();
             REQUIRE(step != nullptr);
             step->init_subjects();
         },
         {"wizard_welcome_text"}},
        {"WizardInputShaper",
         []() {
             WizardInputShaperStep* step = get_wizard_input_shaper_step();
             REQUIRE(step != nullptr);
             step->init_subjects();
         },
         {"wizard_input_shaper_status", "wizard_input_shaper_progress",
          "wizard_input_shaper_started", "wizard_input_shaper_active",
          "wizard_input_shaper_indeterminate"}},
        {"AmsDeviceOperations",
         []() { helix::ui::get_ams_device_operations_overlay().init_subjects(); },
         {"ams_device_ops_system_info", "ams_device_ops_supports_bypass",
          "ams_device_ops_fw_supports_bypass", "ams_device_ops_hw_bypass_sensor",
          "ams_device_ops_supports_auto_heat", "ams_device_ops_has_backend",
          "ams_device_ops_bypass_is_virtual", "ams_device_ops_reports_spool_ids",
          "ams_device_ops_printer_retains_spool_info", "ams_device_ops_can_reset_endless_spool",
          "ams_device_ops_can_abort", "ams_device_ops_can_home", "ams_device_ops_can_recover"}},
    };

    for (const auto& c : cases) {
        INFO(c.label);
        c.init_fn();
        for (const char* name : c.names) {
            INFO(c.label);
            INFO(name);
            // The absence assertions below only mean something if the names
            // were there to withdraw.
            REQUIRE(lv_xml_get_subject(nullptr, name) != nullptr);
        }
    }

    StaticPanelRegistry::instance().destroy_all();

    for (const auto& c : cases) {
        for (const char* name : c.names) {
            INFO(c.label);
            INFO(name);
            CHECK(lv_xml_get_subject(nullptr, name) == nullptr);
        }
    }
}

TEST_CASE_METHOD(XMLTestFixture, "A destroyed PrinterState withdraws the extruder subject names",
                 "[subject-scope][1538]") {
    // extruder_temp and extruder_target are published by PrinterTemperatureState,
    // whose owner is a stack member of this very fixture. Nine production layouts
    // bind both names, so a name left resolving into storage that has gone out of
    // scope is an observer installed inside freed memory on the next
    // lv_xml_create(). The names differ from their members, so no macro derives
    // them - they are handed to the SubjectManager at the call site or not at all.
    static constexpr const char* kNames[] = {"extruder_temp", "extruder_target"};

    {
        helix::PrinterState scoped;
        scoped.init_subjects(true);

        // The absence assertions below only mean anything if the names were
        // there to withdraw, resolving to THIS state's storage.
        REQUIRE(lv_xml_get_subject(nullptr, "extruder_temp") ==
                scoped.temperature_state().get_active_extruder_temp_subject());
        REQUIRE(lv_xml_get_subject(nullptr, "extruder_target") ==
                scoped.temperature_state().get_active_extruder_target_subject());
    }

    for (const char* name : kNames) {
        INFO(name);
        CHECK(lv_xml_get_subject(nullptr, name) == nullptr);
    }
}

namespace {
int g_status_notifies = 0;
void count_notify(lv_observer_t*, lv_subject_t*) {
    ++g_status_notifies;
}
} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "Device Operations initialises its subjects once when pre-inited before show",
                 "[ams][device-ops][subject-scope]") {
    StaticPanelRegistry::instance().destroy_all();
    auto& overlay = helix::ui::get_ams_device_operations_overlay();

    // The outside path: panels pre-init before show().
    overlay.init_subjects();
    REQUIRE(overlay.are_subjects_initialized());

    // A second init through show() would re-init the subject and drop this observer.
    lv_subject_t* status = lv_xml_get_subject(nullptr, "ams_device_ops_system_info");
    REQUIRE(status != nullptr);
    g_status_notifies = 0;
    lv_observer_t* obs = lv_subject_add_observer(status, count_notify, nullptr);
    g_status_notifies = 0;

    REQUIRE(register_component("header_bar"));
    REQUIRE(register_component("ams_device_operations"));
    overlay.show(test_screen());
    overlay.show(test_screen());

    lv_subject_notify(status);
    CHECK(g_status_notifies >= 1);
    CHECK(lv_xml_get_subject(nullptr, "ams_device_ops_system_info") == status);
    lv_observer_remove(obs);

    StaticPanelRegistry::instance().destroy_all();
    CHECK(lv_xml_get_subject(nullptr, "ams_device_ops_system_info") == nullptr);
}
