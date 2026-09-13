// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_material_temps_chamber.cpp
 * @brief The Material Temperatures edit view: its columns and the effective
 *        caps they are held to (prestonbrown/helixscreen#1263, #1615, #1619).
 *
 * The overlay edits a sparse filament::MaterialOverride; find_material()
 * folds the override into MaterialInfo, and FilamentPanel::set_material()
 * reads each temp from that result to drive
 * TemperatureController::set_target(). These tests pin the capability gates
 * on the columns, the override's path into find_material(), and that every
 * input's ceiling is the EFFECTIVE cap TemperatureController enforces —
 * configfile max_temp over the heater default — not the field's own range.
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

#include <cstdio>
#include <fstream>
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
class ControllerScope {
  public:
    explicit ControllerScope(XMLTestFixture& f)
        : controller_(std::make_shared<helix::TemperatureController>(f.state(), &f.api())) {
        helix::PanelWidgetManager::instance().register_shared_resource(controller_);
    }

    ~ControllerScope() {
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

void check_two_by_two_reflow(const char* variant_path, XMLTestFixture& f) {
    ControllerScope scope(f);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Chamber,
                                                    60);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Nozzle,
                                                    290);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Bed,
                                                    110);
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

    // Every cap hint rides along with its reflowed column in this variant too,
    // not only in the base layout.
    struct {
        const char* widget;
        const char* digits;
    } hints[] = {
        {"edit_nozzle_cap_hint", "290"},
        {"edit_bed_cap_hint", "110"},
        {"edit_chamber_cap_hint", "60"},
    };
    for (const auto& h : hints) {
        lv_obj_t* hint = find_widget(h.widget);
        REQUIRE(hint != nullptr);
        CHECK_FALSE(hidden(hint));
        CHECK(std::string(lv_label_get_text(hint)).find(h.digits) != std::string::npos);
    }

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
    // Restore the standard component registration for any case that follows.
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/material_temps_overlay.xml") ==
            LV_RESULT_OK);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "Micro variant reflows the temp inputs to two rows of two",
                 "[material_temps][chamber]") {
    check_two_by_two_reflow("A:ui_xml/micro/material_temps_overlay.xml", *this);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "Micro-portrait variant reflows the temp inputs to two rows of two",
                 "[material_temps][chamber]") {
    check_two_by_two_reflow("A:ui_xml/micro_portrait/material_temps_overlay.xml", *this);
}

// The edit view's chamber ceiling is the EFFECTIVE cap — the one
// TemperatureController enforces (configfile max_temp over the backend's
// conservative default) — not the input's own 0-120 range, so what the user
// saves is what a send applies (prestonbrown/helixscreen#1615).
TEST_CASE_METHOD(XMLTestFixture, "Edit view surfaces the printer's chamber cap when it is tighter",
                 "[material_temps][chamber][1615]") {
    ControllerScope scope(*this);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Chamber,
                                                    60);
    open_abs_edit_view(*this);

    // Precondition: the setup reached a configured cap through the shared
    // ceiling helper every temperature-input surface must derive from.
    const float shared =
        scope.controller().effective_keypad_max(helix::HeaterType::Chamber, 120.0f);
    REQUIRE(shared == 60.0f);

    lv_subject_t* cap = lv_xml_get_subject(nullptr, "material_chamber_cap");
    REQUIRE(cap != nullptr);
    CHECK(lv_subject_get_int(cap) == static_cast<int>(shared));

    lv_obj_t* hint = find_widget("edit_chamber_cap_hint");
    REQUIRE(hint != nullptr);
    CHECK_FALSE(hidden(hint));
    CHECK(std::string(lv_label_get_text(hint)).find("60") != std::string::npos);

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "Edit view hides the cap hint when the cap is not tighter",
                 "[material_temps][chamber][1615]") {
    ControllerScope scope(*this);
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
    ControllerScope scope(*this);
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

// The reject-toast buffer must hold the longest locale at the widest cap:
// ru is the longest rendering today and 120 the most digits a cap can carry,
// so this is the worst case snprintf faces. A buffer that cuts it garbles the
// UTF-8 degree sign on every ru reject toast.
TEST_CASE("Chamber reject-toast buffer holds the longest locale at the widest cap",
          "[material_temps][chamber][1615]") {
    const std::string ru_widest = "Температура камеры должна быть 0-120°C";
    char buf[helix::settings::MaterialTempsOverlay::kToastBufBytes];
    const int written = snprintf(buf, sizeof(buf), "%s", ru_widest.c_str());
    CHECK(written == static_cast<int>(ru_widest.size()));
    CHECK(std::string(buf) == ru_widest);
}

// The nozzle and bed columns answer the same authority the chamber column
// gained in #1615: their ceilings are the EFFECTIVE caps, not the fields' own
// 100-500 / 0-200 ranges (prestonbrown/helixscreen#1619).
TEST_CASE_METHOD(XMLTestFixture, "Edit view surfaces the printer's nozzle cap when it is tighter",
                 "[material_temps][1619]") {
    ControllerScope scope(*this);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Nozzle,
                                                    290);
    open_abs_edit_view(*this);

    // Precondition: the setup reached a configured cap through the shared
    // ceiling helper every temperature-input surface must derive from.
    const float shared = scope.controller().effective_keypad_max(helix::HeaterType::Nozzle, 500.0f);
    REQUIRE(shared == 290.0f);

    lv_subject_t* cap = lv_xml_get_subject(nullptr, "material_nozzle_cap");
    REQUIRE(cap != nullptr);
    CHECK(lv_subject_get_int(cap) == static_cast<int>(shared));

    // The hint sits under the Nozzle Max column — the ceiling field of the
    // two nozzle inputs the save-time check holds to the cap.
    lv_obj_t* hint = find_widget("edit_nozzle_cap_hint");
    REQUIRE(hint != nullptr);
    CHECK_FALSE(hidden(hint));
    CHECK(std::string(lv_label_get_text(hint)).find("290") != std::string::npos);

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "Edit view surfaces the printer's bed cap when it is tighter",
                 "[material_temps][1619]") {
    ControllerScope scope(*this);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Bed,
                                                    110);
    open_abs_edit_view(*this);

    const float shared = scope.controller().effective_keypad_max(helix::HeaterType::Bed, 200.0f);
    REQUIRE(shared == 110.0f);

    lv_subject_t* cap = lv_xml_get_subject(nullptr, "material_bed_cap");
    REQUIRE(cap != nullptr);
    CHECK(lv_subject_get_int(cap) == static_cast<int>(shared));

    lv_obj_t* hint = find_widget("edit_bed_cap_hint");
    REQUIRE(hint != nullptr);
    CHECK_FALSE(hidden(hint));
    CHECK(std::string(lv_label_get_text(hint)).find("110") != std::string::npos);

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "Edit view hides the nozzle and bed cap hints when not tighter",
                 "[material_temps][1619]") {
    ControllerScope scope(*this);
    // 500 and 200 are the inputs' own absolute ceilings — nothing to surface.
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Nozzle,
                                                    500);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Bed,
                                                    200);
    open_abs_edit_view(*this);

    for (const char* name : {"material_nozzle_cap", "material_bed_cap"}) {
        lv_subject_t* cap = lv_xml_get_subject(nullptr, name);
        REQUIRE(cap != nullptr);
        CHECK(lv_subject_get_int(cap) == 0);
    }
    for (const char* name : {"edit_nozzle_cap_hint", "edit_bed_cap_hint"}) {
        lv_obj_t* hint = find_widget(name);
        REQUIRE(hint != nullptr);
        CHECK(hidden(hint));
    }

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "Saving a nozzle value above the effective cap is rejected",
                 "[material_temps][1619]") {
    ControllerScope scope(*this);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Nozzle,
                                                    290);
    open_abs_edit_view(*this);

    lv_obj_t* nozzle_max = find_widget("edit_nozzle_max");
    REQUIRE(nozzle_max != nullptr);

    // 350 is inside the input's own 100-500 range but above the printer's cap:
    // without the clamp it would persist and silently apply at 290.
    lv_textarea_set_text(nozzle_max, "350");
    helix::settings::get_material_temps_overlay().handle_save();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(MaterialSettingsManager::instance().get_override("ABS") == nullptr);

    // At the cap itself saving works: 290 differs from ABS's default 270, so
    // a nozzle_max override is stored and find_material() applies it.
    lv_textarea_set_text(nozzle_max, "290");
    helix::settings::get_material_temps_overlay().handle_save();
    helix::ui::UpdateQueue::instance().drain();

    const auto* ovr = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(ovr != nullptr);
    REQUIRE(ovr->nozzle_max.has_value());
    CHECK(*ovr->nozzle_max == 290);
    auto mat = filament::find_material("ABS");
    REQUIRE(mat.has_value());
    CHECK(mat->nozzle_max == 290);

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "Saving a bed value above the effective cap is rejected",
                 "[material_temps][1619]") {
    ControllerScope scope(*this);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Bed,
                                                    110);
    open_abs_edit_view(*this);

    lv_obj_t* bed_input = find_widget("edit_bed_temp");
    REQUIRE(bed_input != nullptr);

    // 150 is inside the input's own 0-200 range but above the printer's cap.
    lv_textarea_set_text(bed_input, "150");
    helix::settings::get_material_temps_overlay().handle_save();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(MaterialSettingsManager::instance().get_override("ABS") == nullptr);

    // At the cap itself saving works: 110 differs from ABS's default 100.
    lv_textarea_set_text(bed_input, "110");
    helix::settings::get_material_temps_overlay().handle_save();
    helix::ui::UpdateQueue::instance().drain();

    const auto* ovr = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(ovr != nullptr);
    REQUIRE(ovr->bed_temp.has_value());
    CHECK(*ovr->bed_temp == 110);
    auto mat = filament::find_material("ABS");
    REQUIRE(mat.has_value());
    CHECK(mat->bed_temp == 110);

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}

// Same worst-case shape as the chamber pin above, per new parameterized
// toast: ru at the widest cap each input can carry.
TEST_CASE("Bed and nozzle reject-toast buffers hold the longest locale at the widest cap",
          "[material_temps][1619]") {
    const std::string ru_widest[] = {
        "Температура сопла должна быть 100-500°C",
        "Температура стола должна быть 0-200°C",
    };
    for (const auto& widest : ru_widest) {
        char buf[helix::settings::MaterialTempsOverlay::kToastBufBytes];
        const int written = snprintf(buf, sizeof(buf), "%s", widest.c_str());
        CHECK(written == static_cast<int>(widest.size()));
        CHECK(std::string(buf) == widest);
    }
}

// The i18n gates are presence-only: they verify a key exists with matching
// format specifiers, not that its value is the intended string. A YAML folded
// scalar whose continuation line an edit orphans folds its debris into the
// next value ("... entre 100 et %d°C 500°C") and every gate stays green while
// the app loads the corrupted line. This pin holds the loaded catalog's fr
// value for the nozzle range key — the one a folded orphan corrupted — against
// its intended literal.
TEST_CASE("fr nozzle range key carries its intended value in the loaded catalog",
          "[material_temps][1619]") {
    std::ifstream catalog("ui_xml/translations/fr.xml");
    REQUIRE(catalog.is_open());

    const std::string needle = "<translation tag=\"Nozzle temp must be 100-%d°C\" fr=\"";
    bool found = false;
    std::string line;
    while (std::getline(catalog, line)) {
        const auto pos = line.find(needle);
        if (pos == std::string::npos) {
            continue;
        }
        found = true;
        const auto value_end = line.find("\"/>", pos + needle.size());
        REQUIRE(value_end != std::string::npos);
        CHECK(line.substr(pos + needle.size(), value_end - pos - needle.size()) ==
              "La température de la buse doit être entre 100 et %d°C");
    }
    REQUIRE(found);
}

// kCapHintBufBytes must hold the longest locale's formatted hint (ru) at the
// widest cap each column can carry, the same worst case the hint snprintf
// faces. A buffer that cuts it garbles the UTF-8 degree sign on the hint.
TEST_CASE("Cap-hint buffers hold the longest locale at the widest cap", "[material_temps][1619]") {
    const std::string ru_widest[] = {
        "Сопло принтера ограничено 500°C",
        "Стол принтера ограничен 200°C",
        "Камера принтера ограничена 120°C",
    };
    for (const auto& widest : ru_widest) {
        char buf[helix::settings::MaterialTempsOverlay::kCapHintBufBytes];
        const int written = snprintf(buf, sizeof(buf), "%s", widest.c_str());
        CHECK(written == static_cast<int>(widest.size()));
        CHECK(std::string(buf) == widest);
    }
}
