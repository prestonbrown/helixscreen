// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_lazy_load.cpp
 * @brief A print-select panel nobody has opened issues no file traffic and builds no detail view
 *
 * Listing, metadata and thumbnail fetches wait for the first on_activate(). A
 * programmatic selection made before the first listing lands (history reprint,
 * Print Last) is held until the listing arrives.
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_select.h"

#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
struct UnvisitedPrintSelectFixture : PrintSelectPanelFixture {
    UnvisitedPrintSelectFixture()
        : PrintSelectPanelFixture(PrintSelectFilelistHandler::Registered,
                                  PrintSelectVisit::Deferred) {}
};
} // namespace

TEST_CASE_METHOD(UnvisitedPrintSelectFixture,
                 "Unvisited print-select neither lists files nor builds the detail view",
                 "[print_select][lazy]") {
    PlantedGcode file("lazy_unvisited.gcode");
    REQUIRE(file.on_disk());
    drain();

    panel_->refresh_files(true);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_size(*panel_) == 0);
    REQUIRE_FALSE(PrintSelectPanelTestAccess::detail_view_built(*panel_));

    NavigationManager::instance().set_active(PanelId::PrintSelect);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
}

TEST_CASE_METHOD(UnvisitedPrintSelectFixture,
                 "Selecting a file before the first listing opens it once the listing lands",
                 "[print_select][lazy]") {
    PlantedGcode file("lazy_select.gcode");
    REQUIRE(file.on_disk());
    drain();

    NavigationManager::instance().set_active(PanelId::PrintSelect);
    REQUIRE(panel_->select_file_by_name(file.name()));
    drain();

    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
    REQUIRE(PrintSelectPanelTestAccess::detail_view_visible(*panel_));
}

TEST_CASE_METHOD(UnvisitedPrintSelectFixture,
                 "First file selection shows the no-thumbnail placeholder in the new detail view",
                 "[print_select][lazy]") {
    PlantedGcode file("lazy_nothumb.gcode");
    REQUIRE(file.on_disk());
    NavigationManager::instance().set_active(PanelId::PrintSelect);
    drain();
    REQUIRE_FALSE(PrintSelectPanelTestAccess::detail_view_built(*panel_));

    REQUIRE(panel_->select_file_by_name(file.name()));
    drain();

    lv_obj_t* icon = lv_obj_find_by_name(test_screen(), "detail_no_thumbnail_icon");
    REQUIRE(icon != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(UnvisitedPrintSelectFixture,
                 "A detail view reopened after closing still shows the no-thumbnail placeholder",
                 "[print_select][lazy]") {
    // Closing frees the overlay's widgets, and the next selection lands before
    // they are rebuilt: the placeholder has to come from the binding.
    PlantedGcode file("lazy_nothumb_reopen.gcode");
    REQUIRE(file.on_disk());
    NavigationManager::instance().set_active(PanelId::PrintSelect);
    drain();

    for (int open = 0; open < 2; ++open) {
        CAPTURE(open);
        REQUIRE(panel_->select_file_by_name(file.name()));
        drain();
        REQUIRE(PrintSelectPanelTestAccess::detail_view_visible(*panel_));
        lv_obj_t* icon = lv_obj_find_by_name(test_screen(), "detail_no_thumbnail_icon");
        REQUIRE(icon != nullptr);
        CHECK_FALSE(lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN));
        // Cards carry a gradient_bg too; this is the preview's.
        lv_obj_t* preview = lv_obj_find_by_name(test_screen(), "detail_preview_clear_area");
        REQUIRE(preview != nullptr);
        lv_obj_t* gradient = lv_obj_find_by_name(preview, "gradient_bg");
        REQUIRE(gradient != nullptr);
        CHECK(lv_obj_has_flag(gradient, LV_OBJ_FLAG_HIDDEN));

        NavigationManager::instance().go_back();
        drain();
        lv_timer_handler();
        drain();
    }
}

TEST_CASE_METHOD(UnvisitedPrintSelectFixture,
                 "Opening print-select starts building cards and leaving stops it",
                 "[print_select][lazy]") {
    const auto* cards = PrintSelectPanelTestAccess::card_view(*panel_);
    REQUIRE(cards != nullptr);
    REQUIRE_FALSE(cards->is_prebuilding());

    NavigationManager::instance().set_active(PanelId::PrintSelect);
    CHECK(cards->is_prebuilding());

    NavigationManager::instance().set_active(PanelId::Home);
    CHECK_FALSE(cards->is_prebuilding());
}

TEST_CASE_METHOD(UnvisitedPrintSelectFixture,
                 "Switching print-select to the list view stops building cards",
                 "[print_select][lazy]") {
    const auto* cards = PrintSelectPanelTestAccess::card_view(*panel_);
    REQUIRE(cards != nullptr);
    NavigationManager::instance().set_active(PanelId::PrintSelect);
    REQUIRE(cards->is_prebuilding());

    panel_->toggle_view();
    CHECK_FALSE(cards->is_prebuilding());
}
