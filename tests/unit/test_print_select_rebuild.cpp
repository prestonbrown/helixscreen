// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_rebuild.cpp
 * @brief A hot-reload rebuild of print-select keeps the file cards on screen
 *
 * PanelBase::rebuild() recreates the card/list views empty, and the next refresh
 * returning the same listing skips repopulation as unchanged, so the rebuilt
 * panel has to re-render the list it already holds (#1616).
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_select.h"

#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Cards the current print-select widget tree is showing.
int visible_cards() {
    lv_obj_t* panel = NavigationManager::instance().get_panel_widget(PanelId::PrintSelect);
    REQUIRE(panel != nullptr);
    lv_obj_t* container = lv_obj_find_by_name(panel, "card_view_container");
    REQUIRE(container != nullptr);
    int shown = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); i++) {
        if (!lv_obj_has_flag(lv_obj_get_child(container, static_cast<int32_t>(i)),
                             LV_OBJ_FLAG_HIDDEN)) {
            shown++;
        }
    }
    return shown;
}

} // namespace

TEST_CASE_METHOD(PrintSelectPanelFixture, "A rebuilt print-select panel re-renders its file cards",
                 "[print_select][hot_reload]") {
    PlantedGcode file("rebuild_keeps_cards.gcode");

    panel_->refresh_files(true);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
    REQUIRE(visible_cards() > 0);

    lv_obj_t* before = panel_obj_;
    REQUIRE(panel_->rebuild());
    drain();
    REQUIRE(NavigationManager::instance().get_panel_widget(PanelId::PrintSelect) != before);

    CHECK(visible_cards() > 0);

    // The same listing again is "unchanged" and skips repopulation; the cards
    // must already be there.
    panel_->refresh_files(true);
    drain();
    CHECK(visible_cards() > 0);
}
