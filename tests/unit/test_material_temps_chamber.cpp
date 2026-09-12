// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_material_temps_chamber.cpp
 * @brief Chamber-temp column of the Material Temperatures overlay
 *        (prestonbrown/helixscreen#1263).
 *
 * The overlay edits a sparse filament::MaterialOverride; find_material()
 * folds the override into MaterialInfo, and FilamentPanel::set_material()
 * reads chamber_temp_c from that result to drive
 * TemperatureController::set_target(HeaterType::Chamber, ...). These tests pin
 * the capability gate on the row and the override's path into find_material().
 */

#include "ui_settings_material_temps.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "filament_database.h"
#include "material_settings_manager.h"
#include "static_panel_registry.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"

using filament::MaterialOverride;
using helix::MaterialSettingsManager;

namespace {

/// The overlay is a process-lifetime singleton whose widgets belong to
/// whichever test screen built it; XMLTestFixture gives each TEST_CASE a fresh
/// screen. Drop any instance an earlier case left behind so create() runs
/// against this case's screen (same pattern as
/// test_ams_environment_overlay_zones.cpp).
void reset_material_temps_singleton() {
    helix::ui::UpdateQueue::instance().drain();
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

lv_subject_t* set_capability(const char* name, int value) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    lv_subject_set_int(subject, value);
    return subject;
}

lv_obj_t* find_widget(const char* name) {
    return lv_obj_find_by_name(lv_screen_active(), name);
}

bool hidden(lv_obj_t* obj) {
    return obj == nullptr || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "Chamber row shows the material's chamber default when a heater exists",
                 "[material_temps][chamber]") {
    reset_material_temps_singleton();
    MaterialSettingsManager::instance().clear_override("ABS");
    set_capability("printer_has_chamber_heater", 1);
    REQUIRE(register_component("material_temps_overlay"));

    auto& overlay = helix::settings::get_material_temps_overlay();
    overlay.show(lv_screen_active());
    helix::ui::UpdateQueue::instance().drain();

    overlay.handle_material_row_clicked("ABS");

    lv_obj_t* chamber_input = find_widget("edit_chamber_temp");
    REQUIRE(chamber_input != nullptr);
    CHECK_FALSE(hidden(lv_obj_get_parent(chamber_input)));
    // ABS ships with chamber_temp_c = 50 in the filament database.
    CHECK(std::string(lv_textarea_get_text(chamber_input)) == "50");

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "Saving the edit view persists a chamber override find_material applies",
                 "[material_temps][chamber]") {
    reset_material_temps_singleton();
    MaterialSettingsManager::instance().clear_override("ABS");
    set_capability("printer_has_chamber_heater", 1);
    REQUIRE(register_component("material_temps_overlay"));

    auto& overlay = helix::settings::get_material_temps_overlay();
    overlay.show(lv_screen_active());
    helix::ui::UpdateQueue::instance().drain();
    overlay.handle_material_row_clicked("ABS");

    lv_obj_t* chamber_input = find_widget("edit_chamber_temp");
    REQUIRE(chamber_input != nullptr);
    lv_textarea_set_text(chamber_input, "60");

    overlay.handle_save();
    helix::ui::UpdateQueue::instance().drain();

    // The sparse override carries the chamber value...
    const auto* ovr = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(ovr != nullptr);
    REQUIRE(ovr->chamber_temp.has_value());
    CHECK(*ovr->chamber_temp == 60);

    // ...and find_material() folds it into the MaterialInfo whose
    // chamber_temp_c drives the chamber preset send.
    auto mat = filament::find_material("ABS");
    REQUIRE(mat.has_value());
    CHECK(mat->chamber_temp_c == 60);

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "A chamber override of zero means no chamber heat",
                 "[material_temps][chamber]") {
    MaterialSettingsManager::instance().clear_override("ABS");

    MaterialOverride ovr;
    ovr.chamber_temp = 0; // ABS defaults to 50; zero is a deliberate off
    MaterialSettingsManager::instance().set_override("ABS", ovr);

    auto mat = filament::find_material("ABS");
    REQUIRE(mat.has_value());
    CHECK(mat->chamber_temp_c == 0);

    // Optional presence, not value truthiness: a stored zero must survive the
    // get_override read so find_material above keeps applying it.
    const auto* stored = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(stored != nullptr);
    REQUIRE(stored->chamber_temp.has_value());
    CHECK(*stored->chamber_temp == 0);

    MaterialSettingsManager::instance().clear_override("ABS");
}

// Pin: a printer with no chamber heater never offers the field (the column is
// gated on the capability subject, hidden rather than disabled).
TEST_CASE_METHOD(XMLTestFixture, "Chamber row is absent when the printer has no chamber heater",
                 "[material_temps][chamber]") {
    reset_material_temps_singleton();
    MaterialSettingsManager::instance().clear_override("ABS");
    set_capability("printer_has_chamber_heater", 0);
    REQUIRE(register_component("material_temps_overlay"));

    auto& overlay = helix::settings::get_material_temps_overlay();
    overlay.show(lv_screen_active());
    helix::ui::UpdateQueue::instance().drain();
    overlay.handle_material_row_clicked("ABS");

    // The capability bind gates the input's column (LVGL does not propagate a
    // parent's HIDDEN flag to descendants, so assert at the column).
    lv_obj_t* chamber_input = find_widget("edit_chamber_temp");
    REQUIRE(chamber_input != nullptr);
    CHECK(hidden(lv_obj_get_parent(chamber_input)));

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}
