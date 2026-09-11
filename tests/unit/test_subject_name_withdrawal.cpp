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

#include "ui_settings_barcode_scanner.h"
#include "ui_settings_display_sound.h"
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

namespace helix::settings {
DisplaySoundSettingsOverlay& get_display_sound_settings_overlay();
MaterialTempsOverlay& get_material_temps_overlay();
LabelPrinterSettingsOverlay& get_label_printer_settings_overlay();
} // namespace helix::settings

namespace helix::ui {
SpoolmanOverlay& get_spoolman_overlay();
BarcodeScannerSettingsOverlay& get_barcode_scanner_settings_overlay();
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
        {"DisplaySound",
         []() { helix::settings::get_display_sound_settings_overlay().init_subjects(); },
         {"brightness_value", "theme_apply_disabled"}},
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
                scoped.get_active_extruder_temp_subject());
        REQUIRE(lv_xml_get_subject(nullptr, "extruder_target") ==
                scoped.get_active_extruder_target_subject());
    }

    for (const char* name : kNames) {
        INFO(name);
        CHECK(lv_xml_get_subject(nullptr, name) == nullptr);
    }
}
