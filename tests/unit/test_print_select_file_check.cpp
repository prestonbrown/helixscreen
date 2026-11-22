// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_file_check.cpp
 * @brief Print waits on the shown file's scan only when an option needs it.
 *
 * Run with: ./build/bin/helix-tests "[print_select][file_check]"
 *
 * A start that turns off a step a file may embed rewrites the file by its
 * scan's line numbers, so while that scan is still reading the Print button
 * is disabled and says "Checking file…". A start that needs no scan prints at
 * once.
 */

#include "ui_panel_print_select.h"
#include "ui_print_preparation_manager.h"

#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/scoped_macros_read.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

constexpr const char* kMeshFile = "G28\n"
                                  "BED_MESH_CALIBRATE\n"
                                  "G1 X10\n";

class FileCheckFixture : private helix::PrintSelectGlobalStateReset,
                         public helix::PrintSelectPanelFixture {
  public:
    FileCheckFixture()
        : helix::PrintSelectPanelFixture(helix::PrintSelectFilelistHandler::Unregistered,
                                         helix::PrintSelectVisit::Immediate,
                                         helix::PrintSelectApi::Delayed) {
        set_moonraker_api(api_.get());
        get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
        panel_->refresh_files(/*force=*/true);
        drain();
        transfers().hold_partials = true;
    }

    ~FileCheckFixture() override {
        transfers().hold_partials = false;
        transfers().release_held_partials();
        drain();
        set_moonraker_api(nullptr);
    }

    helix::ui::PrintPreparationManager& prep() {
        auto* prep = PrintSelectPanelTestAccess::prep_manager(*panel_);
        REQUIRE(prep != nullptr);
        return *prep;
    }

    void open() {
        REQUIRE(panel_->select_file_by_name(file_.name()));
        drain();
    }

    static int can_print() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "print_select_can_print"));
    }

    static std::string blocked_reason() {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "print_select_blocked_reason");
        return s ? lv_subject_get_string(s) : std::string("<missing>");
    }

    helix::test::ScopedMacrosRead macros;
    helix::PlantedGcode file_{"file_check.gcode", "", kMeshFile};
};

} // namespace

TEST_CASE_METHOD(FileCheckFixture, "Print waits on the file check only when an option needs it",
                 "[print_select][file_check][start_waiter]") {
    REQUIRE(can_print() == 1);

    SECTION("bed mesh off: Print is disabled until the scan lands") {
        prep().set_start_option_override(
            [](const std::string& id) { return id == "bed_mesh" ? 0 : -1; });
        open();
        REQUIRE(prep().print_waits_on_scan(file_.name()));
        CHECK(can_print() == 0);
        CHECK(blocked_reason() == "Checking file…");

        transfers().release_held_partials();
        drain();
        CHECK(prep().has_scan_result_for(file_.name()));
        CHECK(can_print() == 1);
        CHECK(blocked_reason().empty());
        prep().set_start_option_override(nullptr);
    }
    SECTION("nothing turned off: Print works at once") {
        open();
        CHECK_FALSE(prep().has_scan_result_for(file_.name())); // still reading
        CHECK(can_print() == 1);
        CHECK(blocked_reason().empty());
    }
    SECTION("turning a step off while the file reads holds Print") {
        // A printer whose option set renders a bed mesh row.
        get_printer_state().set_printer_type_sync("Creality K2 Plus");
        open();
        REQUIRE(can_print() == 1);
        lv_obj_t* row = lv_obj_find_by_name(lv_screen_active(), "option_tile_bed_mesh");
        REQUIRE(row != nullptr);
        REQUIRE(lv_obj_has_state(row, LV_STATE_CHECKED));
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
        REQUIRE_FALSE(lv_obj_has_state(row, LV_STATE_CHECKED));
        drain();
        CHECK(can_print() == 0);
        CHECK(blocked_reason() == "Checking file…");
        get_printer_state().set_printer_type_sync("");
    }
}
