// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_context_menu.h"

#include "../helix_test_fixture.h"
#include "../test_fixtures.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include <array>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace helix {
void register_print_status_widget();
} // namespace helix

TEST_CASE_METHOD(HelixTestFixture, "PrintStatusWidget config defaults", "[print_status][config]") {
    PrintStatusWidget w;
    nlohmann::json empty = nlohmann::json::object();
    w.set_config(empty);
    REQUIRE(w.layout_style_for_test() == "library");
    REQUIRE(w.nozzle_tool_override_for_test() == "auto");
}

TEST_CASE_METHOD(HelixTestFixture, "PrintStatusWidget config sets detailed",
                 "[print_status][config]") {
    PrintStatusWidget w;
    nlohmann::json cfg = {{"layout_style", "detailed"}, {"nozzle_tool_override", "extruder1"}};
    w.set_config(cfg);
    REQUIRE(w.layout_style_for_test() == "detailed");
    // 'extruder1' doesn't resolve in this fixture (no multi-extruder PrinterState
    // init), so set_config's formatter dispatch returns false and the widget
    // clears the stale override back to 'auto'. That's the expected fallback —
    // see DetailedFormatter::set_nozzle_tool_override and the matching
    // ghost-tool clean-up in PrintStatusWidget::set_config.
    REQUIRE(w.nozzle_tool_override_for_test() == "auto");
}

TEST_CASE_METHOD(HelixTestFixture, "PrintStatusWidget config skips unknown and wrong-typed values",
                 "[print_status][config]") {
    using Flags = std::array<bool, 5>; // title, files, reprint, recent, queue
    PrintStatusWidget w;
    w.set_config({{"layout_style", "bogus"}, {"show_title", false}, {"show_print_files", 0}});
    CHECK(w.layout_style_for_test() == "library");
    CHECK(w.show_options_for_test() == Flags{false, true, true, true, true});
    w.set_config({{"layout_style", "detailed"}});
    w.set_config({{"layout_style", 1}, {"show_title", true}, {"show_job_queue", "false"}});
    CHECK(w.layout_style_for_test() == "detailed");
    CHECK(w.show_options_for_test() == Flags{true, true, true, true, true});
}

// The Layout picker's Show Sections group belongs to the Library layout only.
TEST_CASE_METHOD(XMLTestFixture, "PrintStatusWidget picker: Show Sections follows the layout",
                 "[print_status][config][context_menu]") {
    helix::ui::ContextMenu::register_shared_callbacks();
    register_print_status_widget();
    REQUIRE(register_component("print_status_configure_picker"));

    PrintStatusWidget w;
    w.set_config({{"layout_style", "detailed"}});
    lv_obj_t* tile = lv_obj_create(test_screen());
    w.attach(tile, test_screen());
    w.on_edit_configure();

    lv_obj_t* group = lv_obj_find_by_name(test_screen(), "show_sections_group");
    REQUIRE(group != nullptr);
    CHECK(lv_obj_is_hidden(group));

    lv_obj_send_event(lv_obj_find_by_name(lv_obj_get_parent(group), "layout_btn_library"),
                      LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(lv_obj_is_hidden(group));

    lv_obj_send_event(lv_obj_find_by_name(lv_obj_get_parent(group), "layout_btn_detailed"),
                      LV_EVENT_CLICKED, nullptr);
    CHECK(lv_obj_is_hidden(group));

    w.detach();
}

namespace {
bool row_checked(lv_obj_t* row) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
        lv_obj_t* child = lv_obj_get_child(row, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_checkbox_class))
            return lv_obj_has_state(child, LV_STATE_CHECKED);
    }
    FAIL("no checkbox in the row");
    return false;
}
} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "PrintStatusWidget picker: section rows show and toggle the widget's options",
                 "[print_status][config][context_menu]") {
    using Flags = std::array<bool, 5>; // title, files, reprint, recent, queue
    helix::ui::ContextMenu::register_shared_callbacks();
    register_print_status_widget();
    REQUIRE(register_component("print_status_configure_picker"));
    PrintStatusWidget w;
    w.set_config({{"layout_style", "library"}, {"show_print_files", false}});
    lv_obj_t* tile = lv_obj_create(test_screen());
    w.attach(tile, test_screen());
    w.on_edit_configure();

    lv_obj_t* files = lv_obj_find_by_name(test_screen(), "opt_print_files");
    lv_obj_t* title = lv_obj_find_by_name(test_screen(), "opt_title");
    REQUIRE(files != nullptr);
    REQUIRE(title != nullptr);
    CHECK_FALSE(row_checked(files));
    CHECK(row_checked(title));

    lv_obj_send_event(files, LV_EVENT_CLICKED, nullptr);
    CHECK(row_checked(files));
    CHECK(w.show_options_for_test() == Flags{true, true, true, true, true});
    lv_obj_send_event(title, LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(row_checked(title));
    CHECK(w.show_options_for_test() == Flags{false, true, true, true, true});

    // The chosen layout's button carries the primary fill, the other none.
    lv_obj_t* lib = lv_obj_find_by_name(test_screen(), "layout_btn_library");
    lv_obj_t* det = lv_obj_find_by_name(test_screen(), "layout_btn_detailed");
    CHECK(lv_obj_get_style_bg_opa(lib, LV_PART_MAIN) == LV_OPA_COVER);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(lib, LV_PART_MAIN),
                      theme_manager_get_color("primary")));
    CHECK(lv_obj_get_style_bg_opa(det, LV_PART_MAIN) == LV_OPA_TRANSP);
    lv_obj_send_event(det, LV_EVENT_CLICKED, nullptr);
    CHECK(lv_obj_get_style_bg_opa(det, LV_PART_MAIN) == LV_OPA_COVER);
    CHECK(lv_obj_get_style_bg_opa(lib, LV_PART_MAIN) == LV_OPA_TRANSP);
    w.detach();
}
