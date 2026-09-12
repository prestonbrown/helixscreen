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
#include "helix-xml/src/xml/lv_xml.h"
#include "material_settings_manager.h"
#include "panel_widget_manager.h"
#include "static_panel_registry.h"
#include "temperature_controller.h"
#include "test_helpers/temperature_controller_test_access.h"

#include <lvgl.h>
#include <memory>
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

/// Registers a TemperatureController over this fixture's PrinterState/API as
/// the app-global shared resource — the same wiring SubjectInitializer does at
/// app boot, so get_temperature_controller() answers inside the overlay.
class ChamberControllerScope {
  public:
    explicit ChamberControllerScope(XMLTestFixture& f)
        : controller_(std::make_shared<helix::TemperatureController>(f.state(), &f.api())) {
        helix::PanelWidgetManager::instance().register_shared_resource(controller_);
    }

    ~ChamberControllerScope() {
        helix::PanelWidgetManager::instance().clear_shared_resources();
    }

    helix::TemperatureController& controller() {
        return *controller_;
    }

  private:
    std::shared_ptr<helix::TemperatureController> controller_;
};

/// Open the ABS edit view against a printer that has a chamber heater.
/// Fresh singleton + no stored override, same setup every case here needs.
void open_abs_edit_view(XMLTestFixture& f) {
    reset_material_temps_singleton();
    MaterialSettingsManager::instance().clear_override("ABS");
    set_capability("printer_has_chamber_heater", 1);
    REQUIRE(f.register_component("material_temps_overlay"));

    auto& overlay = helix::settings::get_material_temps_overlay();
    overlay.show(lv_screen_active());
    helix::ui::UpdateQueue::instance().drain();
    overlay.handle_material_row_clicked("ABS");
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

// The layout-variant files the app loads at 480x272 (micro) and 272x480
// (micro_portrait) reflow the temperature inputs to two rows of two; a
// four-across row cannot fit those screens. Pins the reflow's structure.
namespace {

void check_two_by_two_reflow(const char* variant_path) {
    reset_material_temps_singleton();
    MaterialSettingsManager::instance().clear_override("ABS");
    set_capability("printer_has_chamber_heater", 1);
    REQUIRE(lv_xml_register_component_from_file(variant_path) == LV_RESULT_OK);

    auto& overlay = helix::settings::get_material_temps_overlay();
    overlay.show(lv_screen_active());
    helix::ui::UpdateQueue::instance().drain();
    overlay.handle_material_row_clicked("ABS");

    lv_obj_t* nozzle_min = find_widget("edit_nozzle_min");
    lv_obj_t* chamber_input = find_widget("edit_chamber_temp");
    REQUIRE(nozzle_min != nullptr);
    REQUIRE(chamber_input != nullptr);

    lv_obj_t* nozzle_row = lv_obj_get_parent(lv_obj_get_parent(nozzle_min));
    lv_obj_t* chamber_row = lv_obj_get_parent(lv_obj_get_parent(chamber_input));
    REQUIRE(nozzle_row != nullptr);
    REQUIRE(chamber_row != nullptr);
    CHECK(nozzle_row != chamber_row);
    CHECK_FALSE(hidden(lv_obj_get_parent(chamber_input)));

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
    // Restore the standard component registration for any case that follows.
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/material_temps_overlay.xml") ==
            LV_RESULT_OK);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "Micro variant reflows the temp inputs to two rows of two",
                 "[material_temps][chamber]") {
    check_two_by_two_reflow("A:ui_xml/micro/material_temps_overlay.xml");
}

TEST_CASE_METHOD(XMLTestFixture,
                 "Micro-portrait variant reflows the temp inputs to two rows of two",
                 "[material_temps][chamber]") {
    check_two_by_two_reflow("A:ui_xml/micro_portrait/material_temps_overlay.xml");
}

// The edit view's chamber ceiling is the EFFECTIVE cap — the one
// TemperatureController enforces (configfile max_temp over the backend's
// conservative default) — not the input's own 0-120 range, so what the user
// saves is what a send applies (prestonbrown/helixscreen#1615).
TEST_CASE_METHOD(XMLTestFixture, "Edit view surfaces the printer's chamber cap when it is tighter",
                 "[material_temps][chamber][1615]") {
    ChamberControllerScope scope(*this);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Chamber,
                                                    60);
    open_abs_edit_view(*this);

    lv_subject_t* cap = lv_xml_get_subject(nullptr, "material_chamber_cap");
    REQUIRE(cap != nullptr);
    CHECK(lv_subject_get_int(cap) == 60);

    lv_obj_t* hint = find_widget("edit_chamber_cap_hint");
    REQUIRE(hint != nullptr);
    CHECK_FALSE(hidden(hint));
    CHECK(std::string(lv_label_get_text(hint)).find("60") != std::string::npos);

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "Edit view hides the cap hint when the cap is not tighter",
                 "[material_temps][chamber][1615]") {
    ChamberControllerScope scope(*this);
    // 120 is the input's own absolute ceiling — nothing tighter to surface.
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Chamber,
                                                    120);
    open_abs_edit_view(*this);

    lv_subject_t* cap = lv_xml_get_subject(nullptr, "material_chamber_cap");
    REQUIRE(cap != nullptr);
    CHECK(lv_subject_get_int(cap) == 0);

    lv_obj_t* hint = find_widget("edit_chamber_cap_hint");
    REQUIRE(hint != nullptr);
    CHECK(hidden(hint));

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "Saving a chamber value above the effective cap is rejected",
                 "[material_temps][chamber][1615]") {
    ChamberControllerScope scope(*this);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Chamber,
                                                    60);
    open_abs_edit_view(*this);

    lv_obj_t* chamber_input = find_widget("edit_chamber_temp");
    REQUIRE(chamber_input != nullptr);

    // 90 is inside the input's own 0-120 range but above the printer's cap:
    // without the clamp it would persist and silently apply at 60.
    lv_textarea_set_text(chamber_input, "90");
    helix::settings::get_material_temps_overlay().handle_save();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(MaterialSettingsManager::instance().get_override("ABS") == nullptr);

    // At the cap itself saving works: 60 differs from ABS's default 50, so a
    // chamber override is stored and find_material() applies it.
    lv_textarea_set_text(chamber_input, "60");
    helix::settings::get_material_temps_overlay().handle_save();
    helix::ui::UpdateQueue::instance().drain();

    const auto* ovr = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(ovr != nullptr);
    REQUIRE(ovr->chamber_temp.has_value());
    CHECK(*ovr->chamber_temp == 60);
    auto mat = filament::find_material("ABS");
    REQUIRE(mat.has_value());
    CHECK(mat->chamber_temp_c == 60);

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}
