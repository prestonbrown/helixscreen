// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_material_temps_chamber.cpp
 * @brief The Material Temperatures edit view: its columns and the bounds
 *        they are held to (prestonbrown/helixscreen#1263, #1615, #1619).
 *
 * The overlay edits a sparse filament::MaterialOverride; find_material()
 * folds the override into MaterialInfo, and FilamentPanel::set_material()
 * reads each temp from that result to drive
 * TemperatureController::set_target(). The material database is a property of
 * the filament: saves are held only to each field's own absolute bounds, and
 * the printer's effective ceiling is applied where a target is actually sent
 * (TemperatureController), never as a database bound.
 */

#include "ui_settings_material_temps.h"

#include "../test_fixtures.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "filament_database.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "material_settings_manager.h"
#include "panel_widget_manager.h"
#include "temperature_controller.h"
#include "test_helpers/press_wash_probe.h"
#include "test_helpers/scoped_shared_resource.h"
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
    // destroy_all() hands back the trees its destructors may not delete inside
    // the window; they belong to this case's screen, so free them here.
    for (lv_obj_t* orphan : StaticPanelRegistry::instance().destroy_all()) {
        lv_obj_delete(orphan);
    }
    helix::ui::UpdateQueue::instance().drain();
}

lv_subject_t* set_capability(const char* name, int value) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    lv_subject_set_int(subject, value);
    return subject;
}

/// Components the overlay's XML instantiates; each case registers them itself
/// so it runs alone as well as inside the sweep.
void register_overlay_deps(XMLTestFixture& f) {
    REQUIRE(f.register_component("header_bar"));
    REQUIRE(f.register_component("overlay_panel"));
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
        : scope_(std::make_shared<helix::TemperatureController>(f.state(), &f.api())) {}

    helix::TemperatureController& controller() {
        return scope_.get();
    }

  private:
    helix_test::ScopedSharedResource<helix::TemperatureController> scope_;
};

/// Open the ABS edit view against a printer that has a chamber heater.
/// Fresh singleton + no stored override, same setup every case here needs.
void open_abs_edit_view(XMLTestFixture& f) {
    reset_material_temps_singleton();
    MaterialSettingsManager::instance().clear_override("ABS");
    set_capability("printer_has_chamber_heater", 1);
    register_overlay_deps(f);
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
    register_overlay_deps(*this);
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
    register_overlay_deps(*this);
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
    register_overlay_deps(*this);
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

// At 480x272 (MICRO) and 272x480 (MICRO_PORTRAIT) the one material_temps file
// wraps its temperature inputs two-by-two: the columns carry a 45% min-width
// bound on ui_breakpoint 0, and four of those cannot share a row. Pins that
// the wrap happens there, and only there (the tiny cases below keep one row).
namespace {

int32_t input_top(lv_obj_t* obj) {
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return area.y1;
}

void check_two_by_two_reflow(XMLTestFixture& f, int32_t screen_w, int32_t screen_h) {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    {
        ScopedResolution res(disp, screen_w, screen_h);
        theme_manager_refresh_layout_constants(disp);

        reset_material_temps_singleton();
        MaterialSettingsManager::instance().clear_override("ABS");
        set_capability("printer_has_chamber_heater", 1);
        register_overlay_deps(f);
        REQUIRE(lv_xml_register_component_from_file("A:ui_xml/material_temps_overlay.xml") ==
                LV_RESULT_OK);

        auto& overlay = helix::settings::get_material_temps_overlay();
        overlay.show(lv_screen_active());
        helix::ui::UpdateQueue::instance().drain();
        overlay.handle_material_row_clicked("ABS");
        lv_obj_update_layout(lv_screen_active());

        lv_obj_t* nozzle_min = find_widget("edit_nozzle_min");
        lv_obj_t* nozzle_max = find_widget("edit_nozzle_max");
        lv_obj_t* bed = find_widget("edit_bed_temp");
        lv_obj_t* chamber = find_widget("edit_chamber_temp");
        REQUIRE(nozzle_min != nullptr);
        REQUIRE(nozzle_max != nullptr);
        REQUIRE(bed != nullptr);
        REQUIRE(chamber != nullptr);
        CHECK_FALSE(hidden(lv_obj_get_parent(chamber)));

        CHECK(input_top(nozzle_max) == input_top(nozzle_min));
        CHECK(input_top(bed) > input_top(nozzle_min));
        CHECK(input_top(chamber) == input_top(bed));

        MaterialSettingsManager::instance().clear_override("ABS");
        reset_material_temps_singleton();
    }
    theme_manager_refresh_layout_constants(disp);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "Micro wraps the temp inputs to two rows of two",
                 "[material_temps][chamber]") {
    check_two_by_two_reflow(*this, 480, 272);
}

// The header's back button carries ui_breakpoint style observers that
// unsubscribe from its delete event; an overlay that strips the button's
// events leaves them to fire on the next breakpoint change, into freed memory.
TEST_CASE_METHOD(XMLTestFixture, "Destroying the overlay releases its ui_breakpoint observers",
                 "[material_temps][chamber]") {
    lv_subject_t* breakpoint = lv_xml_get_subject(nullptr, "ui_breakpoint");
    REQUIRE(breakpoint != nullptr);
    reset_material_temps_singleton();
    const uint32_t baseline = lv_ll_get_len(&breakpoint->subs_ll);

    open_abs_edit_view(*this);
    REQUIRE(lv_ll_get_len(&breakpoint->subs_ll) > baseline);

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
    CHECK(lv_ll_get_len(&breakpoint->subs_ll) == baseline);
}

TEST_CASE_METHOD(XMLTestFixture, "Micro-portrait wraps the temp inputs to two rows of two",
                 "[material_temps][chamber]") {
    check_two_by_two_reflow(*this, 272, 480);
}

// TINY (480x320) and TINY_PORTRAIT (320x480) have no ui_xml/tiny*/ override, so
// LayoutManager::variant_chain() falls through to this base file's own
// four-across row. Each column fills its own share of the row instead of
// carrying a fixed input width, so the row has to shrink with the screen
// rather than overflow it (prestonbrown/helixscreen#1263).
namespace {

/// Absolute screen bounds, matching what `helix-screen ctl geom` reports
/// (remote_control_server.cpp#geom_walk): lv_obj_get_coords() for the origin,
/// not the parent-relative value lv_obj_get_x() would give under a scrolled
/// ancestor.
struct Bounds {
    int32_t x1;
    int32_t x2;
};

Bounds bounds_of(lv_obj_t* obj) {
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return {area.x1, area.x1 + lv_obj_get_width(obj)};
}

void check_row_fits_at(XMLTestFixture& f, int32_t screen_w, int32_t screen_h) {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    {
        ScopedResolution res(disp, screen_w, screen_h);
        theme_manager_refresh_layout_constants(disp);

        reset_material_temps_singleton();
        MaterialSettingsManager::instance().clear_override("ABS");
        set_capability("printer_has_chamber_heater", 1);
        register_overlay_deps(f);
        REQUIRE(lv_xml_register_component_from_file("A:ui_xml/material_temps_overlay.xml") ==
                LV_RESULT_OK);

        auto& overlay = helix::settings::get_material_temps_overlay();
        overlay.show(lv_screen_active());
        helix::ui::UpdateQueue::instance().drain();
        overlay.handle_material_row_clicked("ABS");
        lv_obj_update_layout(lv_screen_active());

        lv_obj_t* chamber_input = find_widget("edit_chamber_temp");
        REQUIRE(chamber_input != nullptr);
        CHECK_FALSE(hidden(lv_obj_get_parent(chamber_input)));
        // One row here: the micro wrap above must not reach the tiny tiers.
        CHECK(input_top(chamber_input) == input_top(find_widget("edit_nozzle_min")));

        // Checked per column, not just against the row's trailing edge: flex_grow
        // divides the row evenly regardless of a child's own declared width, so a
        // middle column can overhang while the row's own right edge still lines up.
        for (const char* name :
             {"edit_nozzle_min", "edit_nozzle_max", "edit_bed_temp", "edit_chamber_temp"}) {
            lv_obj_t* input = find_widget(name);
            REQUIRE(input != nullptr);
            lv_obj_t* column = lv_obj_get_parent(input);
            REQUIRE(column != nullptr);
            INFO("input " << name);
            CHECK(bounds_of(input).x2 <= bounds_of(column).x2);
        }

        MaterialSettingsManager::instance().clear_override("ABS");
        reset_material_temps_singleton();
    }
    // Put the token table and breakpoint subject back where the rest of the
    // suite expects them now that the display is restored.
    theme_manager_refresh_layout_constants(disp);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "Base layout row fits at TINY with a chamber heater",
                 "[material_temps][chamber]") {
    check_row_fits_at(*this, 480, 320);
}

TEST_CASE_METHOD(XMLTestFixture, "Base layout row fits at TINY_PORTRAIT with a chamber heater",
                 "[material_temps][chamber]") {
    check_row_fits_at(*this, 320, 480);
}

// The inputs must shrink below 120px on a narrow screen (above) but never grow
// past it on a wide one: a flex_grow column with no cap would give each input
// far more than 120px at 1024x600, changing how the row has always looked on
// every screen that was never part of this finding.
namespace {

void check_row_capped_at(XMLTestFixture& f, int32_t screen_w, int32_t screen_h) {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    {
        ScopedResolution res(disp, screen_w, screen_h);
        theme_manager_refresh_layout_constants(disp);

        reset_material_temps_singleton();
        MaterialSettingsManager::instance().clear_override("ABS");
        set_capability("printer_has_chamber_heater", 1);
        register_overlay_deps(f);
        REQUIRE(lv_xml_register_component_from_file("A:ui_xml/material_temps_overlay.xml") ==
                LV_RESULT_OK);

        auto& overlay = helix::settings::get_material_temps_overlay();
        overlay.show(lv_screen_active());
        helix::ui::UpdateQueue::instance().drain();
        overlay.handle_material_row_clicked("ABS");
        lv_obj_update_layout(lv_screen_active());

        for (const char* name :
             {"edit_nozzle_min", "edit_nozzle_max", "edit_bed_temp", "edit_chamber_temp"}) {
            lv_obj_t* input = find_widget(name);
            REQUIRE(input != nullptr);
            INFO("input " << name);
            CHECK(lv_obj_get_width(input) <= 120);
        }

        MaterialSettingsManager::instance().clear_override("ABS");
        reset_material_temps_singleton();
    }
    theme_manager_refresh_layout_constants(disp);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "Base layout row inputs stay capped at 120px at 1024x600",
                 "[material_temps][chamber]") {
    check_row_capped_at(*this, 1024, 600);
}

// The printer's cap is a send-time authority, not a database bound: a value
// above the cap stores as entered, and TemperatureController clamps it where
// a target is actually sent (prestonbrown/helixscreen#1615).
TEST_CASE_METHOD(XMLTestFixture, "Saving a chamber value above the printer's cap stores it",
                 "[material_temps][chamber][1615]") {
    ControllerScope scope(*this);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Chamber,
                                                    60);
    open_abs_edit_view(*this);

    lv_obj_t* chamber_input = find_widget("edit_chamber_temp");
    REQUIRE(chamber_input != nullptr);

    // Precondition: 90 really is above the printer's cap.
    const float shared =
        scope.controller().effective_keypad_max(helix::HeaterType::Chamber, 120.0f);
    REQUIRE(shared == 60.0f);

    // 90 is inside the input's own 0-120 range and above the printer's 60 cap:
    // it stores, and the send path is what holds the target to the cap.
    lv_textarea_set_text(chamber_input, "90");
    helix::settings::get_material_temps_overlay().handle_save();
    helix::ui::UpdateQueue::instance().drain();

    const auto* ovr = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(ovr != nullptr);
    REQUIRE(ovr->chamber_temp.has_value());
    CHECK(*ovr->chamber_temp == 90);
    auto mat = filament::find_material("ABS");
    REQUIRE(mat.has_value());
    CHECK(mat->chamber_temp_c == 90);

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "The defaults hint mentions the chamber exactly when its column is shown",
                 "[material_temps][chamber]") {
    ControllerScope scope(*this);
    const int chamber_heater = GENERATE(1, 0);
    CAPTURE(chamber_heater);

    reset_material_temps_singleton();
    MaterialSettingsManager::instance().clear_override("ABS");
    set_capability("printer_has_chamber_heater", chamber_heater);
    register_overlay_deps(*this);
    REQUIRE(register_component("material_temps_overlay"));

    auto& overlay = helix::settings::get_material_temps_overlay();
    overlay.show(lv_screen_active());
    helix::ui::UpdateQueue::instance().drain();
    overlay.handle_material_row_clicked("ABS");

    lv_obj_t* chamber_input = find_widget("edit_chamber_temp");
    REQUIRE(chamber_input != nullptr);
    REQUIRE(hidden(lv_obj_get_parent(chamber_input)) == (chamber_heater == 0));

    lv_obj_t* hint = find_widget("edit_defaults_hint");
    REQUIRE(hint != nullptr);
    const std::string text = lv_label_get_text(hint);
    // The hint was written for this material at all.
    REQUIRE(text.find("nozzle") != std::string::npos);
    CHECK((text.find("chamber") != std::string::npos) == (chamber_heater == 1));

    reset_material_temps_singleton();
}

// A chamber override stored while the printer HAD a heater must not hold the
// edit view hostage on a printer whose heater is gone: the column is hidden,
// so a cap-driven refusal would name a control the user cannot see or fix.
TEST_CASE_METHOD(XMLTestFixture, "A hidden chamber override cannot block the edit view's save",
                 "[material_temps][chamber][1615]") {
    ControllerScope scope(*this);

    MaterialOverride ovr;
    ovr.chamber_temp = 100; // above the chamber default ceiling of 80
    MaterialSettingsManager::instance().set_override("ABS", ovr);

    reset_material_temps_singleton();
    set_capability("printer_has_chamber_heater", 0);
    register_overlay_deps(*this);
    REQUIRE(register_component("material_temps_overlay"));

    auto& overlay = helix::settings::get_material_temps_overlay();
    overlay.show(lv_screen_active());
    helix::ui::UpdateQueue::instance().drain();
    overlay.handle_material_row_clicked("ABS");

    // Precondition: the chamber column really is hidden on this printer.
    lv_obj_t* chamber_input = find_widget("edit_chamber_temp");
    REQUIRE(chamber_input != nullptr);
    CHECK(hidden(lv_obj_get_parent(chamber_input)));

    // Touch one visible field so a successful save leaves a trace.
    lv_obj_t* bed_input = find_widget("edit_bed_temp");
    REQUIRE(bed_input != nullptr);
    lv_textarea_set_text(bed_input, "110");
    overlay.handle_save();
    helix::ui::UpdateQueue::instance().drain();

    const auto* saved = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(saved != nullptr);
    REQUIRE(saved->bed_temp.has_value());
    CHECK(*saved->bed_temp == 110);
    // The hidden override survives the round-trip untouched.
    REQUIRE(saved->chamber_temp.has_value());
    CHECK(*saved->chamber_temp == 100);

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

// A printer whose nozzle ceiling sits below a material's database default
// must not make that material unsavable: the ceiling is a send-time bound,
// so the displayed defaults ride through an untouched save (the audit's
// stock-260-printer ABS case, prestonbrown/helixscreen#1619).
TEST_CASE_METHOD(XMLTestFixture,
                 "An ABS save is not refused for defaults below the printer's nozzle ceiling",
                 "[material_temps][1619]") {
    ControllerScope scope(*this);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Nozzle,
                                                    260);
    open_abs_edit_view(*this);

    // Precondition: the printer's ceiling really is tighter than the ABS
    // database default the view displays.
    const auto mat = filament::find_material("ABS");
    REQUIRE(mat.has_value());
    REQUIRE(mat->nozzle_max > 260);

    // Touch one field so a successful save leaves a trace; the nozzle inputs
    // keep their displayed 240-270 defaults against the 260 ceiling.
    lv_obj_t* bed_input = find_widget("edit_bed_temp");
    REQUIRE(bed_input != nullptr);
    lv_textarea_set_text(bed_input, "110");
    helix::settings::get_material_temps_overlay().handle_save();
    helix::ui::UpdateQueue::instance().drain();

    const auto* ovr = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(ovr != nullptr);
    REQUIRE(ovr->bed_temp.has_value());
    CHECK(*ovr->bed_temp == 110);
    CHECK_FALSE(ovr->nozzle_max.has_value()); // untouched defaults: nothing to override

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}

// Same send-time contract as the chamber column: a nozzle value above the
// printer's cap stores as entered (prestonbrown/helixscreen#1619).
TEST_CASE_METHOD(XMLTestFixture, "Saving a nozzle value above the printer's cap stores it",
                 "[material_temps][1619]") {
    ControllerScope scope(*this);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Nozzle,
                                                    290);
    open_abs_edit_view(*this);

    lv_obj_t* nozzle_max = find_widget("edit_nozzle_max");
    REQUIRE(nozzle_max != nullptr);

    // 350 is inside the input's own 100-500 range and above the printer's 290
    // cap: it stores, and the send path is what holds the target to the cap.
    lv_textarea_set_text(nozzle_max, "350");
    helix::settings::get_material_temps_overlay().handle_save();
    helix::ui::UpdateQueue::instance().drain();

    const auto* ovr = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(ovr != nullptr);
    REQUIRE(ovr->nozzle_max.has_value());
    CHECK(*ovr->nozzle_max == 350);
    auto mat = filament::find_material("ABS");
    REQUIRE(mat.has_value());
    CHECK(mat->nozzle_max == 350);

    MaterialSettingsManager::instance().clear_override("ABS");
    reset_material_temps_singleton();
}

// Same send-time contract for the bed column (prestonbrown/helixscreen#1619).
TEST_CASE_METHOD(XMLTestFixture, "Saving a bed value above the printer's cap stores it",
                 "[material_temps][1619]") {
    ControllerScope scope(*this);
    helix::TemperatureControllerTestAccess::set_max(scope.controller(), helix::HeaterType::Bed,
                                                    110);
    open_abs_edit_view(*this);

    lv_obj_t* bed_input = find_widget("edit_bed_temp");
    REQUIRE(bed_input != nullptr);

    // 150 is inside the input's own 0-200 range and above the printer's 110 cap.
    lv_textarea_set_text(bed_input, "150");
    helix::settings::get_material_temps_overlay().handle_save();
    helix::ui::UpdateQueue::instance().drain();

    const auto* ovr = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(ovr != nullptr);
    REQUIRE(ovr->bed_temp.has_value());
    CHECK(*ovr->bed_temp == 150);
    auto mat = filament::find_material("ABS");
    REQUIRE(mat.has_value());
    CHECK(mat->bed_temp == 150);

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
// value for the nozzle range key against its intended literal.
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

TEST_CASE_METHOD(XMLTestFixture,
                 "A material row presses with the shared wash and a tap opens its editor",
                 "[material_temps][press_wash][1297]") {
    reset_material_temps_singleton();
    register_overlay_deps(*this);
    REQUIRE(register_component("components/material_temps_row"));
    REQUIRE(register_component("material_temps_overlay"));

    auto& overlay = helix::settings::get_material_temps_overlay();
    overlay.show(lv_screen_active());
    helix::ui::UpdateQueue::instance().drain();

    // Rows are named for their material.
    lv_obj_t* row = find_widget("PLA");
    REQUIRE(row != nullptr);
    CHECK(helix::test::paints_press_wash(row));

    lv_subject_t* editing = lv_xml_get_subject(nullptr, "material_editing");
    REQUIRE(editing != nullptr);
    REQUIRE(lv_subject_get_int(editing) == 0);
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    CHECK(lv_subject_get_int(editing) == 1);

    reset_material_temps_singleton();
}
