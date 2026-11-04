// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_toolhead.cpp
 * @brief The toolhead widget draws what its ladder promises at each rung: which parts
 * carry HIDDEN, which text they show, where the plate puts the crosshair, and what Home
 * All and a tile tap do.
 *
 * Boxes are the pixel budgets measured on a live grid (55x54, 114x54, 55x113, 114x113 at
 * medium; 51x53 / 51x112 / 108x53 at large). The decision is toolhead_layout.h's; these
 * tests pin its wiring to the real fonts and XML.
 *
 * Run with: ./build/bin/helix-tests "[widget_size][toolhead]"
 */

#include "ui_nav_manager.h"
#include "ui_panel_motion.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/bed_occupancy_state_test_access.h"
#include "../test_helpers/holding_motion_api.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/tile_boxes.h"
#include "app_globals.h"
#include "bed_occupancy_state.h"
#include "bed_plate_context.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_capabilities_state.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/toolhead_widget.h"
#include "static_panel_registry.h"
#include "tool_state.h"
#include "toolhead_preset_config_modal.h"

#include <array>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::test::Box;
using helix::test::kCell;
using helix::test::kColumn;
using helix::test::kFull;
using helix::test::kGlance;
using helix::test::kStrip;
using helix::test::kWide;
using nlohmann::json;

namespace {

struct ToolheadFixture : public LVGLUITestFixture {
    ToolheadFixture() : mock(MoonrakerClientMock::PrinterType::VORON_24), mock_api(mock, state()) {
        StaticPanelRegistry::instance().destroy_all();
        state().set_klippy_state_sync(KlippyState::READY);
        mock.connect("ws://mock/websocket", []() {}, []() {});
        previous_api = get_moonraker_api();
        set_moonraker_api(&mock_api);

        ToolState::instance().deinit_subjects();
        ToolState::instance().init_subjects(true);
        PrinterDiscovery hw;
        hw.parse_objects(json::array({"gcode_move"}));
        ToolState::instance().init_tools(hw);

        PanelWidgetManager::instance().init_widget_subjects();
        reset_state();
        sentinel();
    }
    ~ToolheadFixture() override {
        StaticPanelRegistry::instance().destroy_all();
        reset_state();
        set_moonraker_api(previous_api);
        mock.stop_temperature_simulation();
        mock.disconnect();
        ToolState::instance().deinit_subjects();
        drain();
    }

    PrinterMotionState& motion() {
        return state().motion_state();
    }
    void reset_state() {
        set_print_active(false);
        set_homed("");
        set_velocity(0);
        set_position(0, 0, 0);
        set_live_position(0, 0, 0);
        drain();
    }
    void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    void set_print_active(bool on) {
        lv_subject_set_int(state().print_state().get_print_active_subject(), on ? 1 : 0);
        drain();
    }
    void set_homed(const char* axes) {
        lv_subject_copy_string(motion().get_homed_axes_subject(), axes);
        drain();
    }
    void set_velocity(int mm_s) {
        lv_subject_set_int(motion().get_live_velocity_subject(), mm_s);
        drain();
    }
    /// Centimillimetres, as the printer state stores them.
    void set_position(int x, int y, int z) {
        lv_subject_set_int(motion().get_position_x_subject(), x);
        lv_subject_set_int(motion().get_position_y_subject(), y);
        lv_subject_set_int(motion().get_position_z_subject(), z);
        drain();
    }
    void set_live_position(int x, int y, int z) {
        lv_subject_set_int(motion().get_live_position_x_subject(), x);
        lv_subject_set_int(motion().get_live_position_y_subject(), y);
        lv_subject_set_int(motion().get_live_position_z_subject(), z);
        drain();
    }
    /// Let the 4 Hz refresh come due.
    void settle() {
        process_lvgl(300);
        drain();
    }

    /// A gcode no action sends, so "nothing was sent" is distinguishable.
    void sentinel() {
        mock_api.execute_gcode("M117 sentinel", nullptr, nullptr);
        REQUIRE(last_sent() == "M117 sentinel");
    }
    const std::string& last_sent() const {
        return mock.last_send_script();
    }
    void tap(lv_obj_t* obj) {
        lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
    }

    MoonrakerClientMock mock;
    MoonrakerAPI mock_api;
    IMoonrakerAPI* previous_api = nullptr;
};

int subject_int(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

std::string subject_text(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_string(subject);
}

int rung_subject() {
    return subject_int("toolhead_rung");
}

lv_obj_t* part(PanelWidgetHarness<ToolheadWidget>& h, const char* name) {
    lv_obj_t* obj = h.child(name);
    REQUIRE(obj != nullptr);
    return obj;
}

bool hidden(lv_obj_t* obj) {
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/// Whether an object draws: it and every ancestor up to the widget root is unhidden.
bool shown(lv_obj_t* obj) {
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
        if (hidden(o))
            return false;
    }
    return true;
}

} // namespace

TEST_CASE_METHOD(ToolheadFixture, "toolhead: medium-tier budgets land on their rung",
                 "[widget_size][toolhead]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    const auto& boxes = test::kMediumLadder;
    for (const auto& b : boxes) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(rung_subject() == static_cast<int>(b.rung));
    }
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: large-tier half cells land on their rung",
                 "[widget_size][toolhead]") {
    helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    const Box boxes[] = {
        {51, 53, SizeRung::Glance},
        {108, 53, SizeRung::Strip},
        {51, 112, SizeRung::Column},
    };
    for (const auto& b : boxes) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(rung_subject() == static_cast<int>(b.rung));
    }
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: each rung shows exactly its parts",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());

    h.resize(1, 1, 55, 54);
    CHECK(shown(part(h, "toolhead_letters")));
    CHECK_FALSE(shown(part(h, "toolhead_zonly")));
    CHECK_FALSE(shown(part(h, "toolhead_rows")));
    CHECK_FALSE(shown(part(h, "toolhead_map")));

    h.resize(1, 1, 55, 113);
    CHECK_FALSE(shown(part(h, "toolhead_letters")));
    CHECK(shown(part(h, "toolhead_rows")));
    CHECK_FALSE(shown(part(h, "toolhead_row_x_dot")));
    CHECK_FALSE(shown(part(h, "toolhead_map")));

    h.resize(1, 1, 114, 113);
    CHECK(shown(part(h, "toolhead_rows")));
    CHECK(shown(part(h, "toolhead_row_x_dot")));
    CHECK(shown(part(h, "toolhead_row_z_dot")));
    CHECK_FALSE(shown(part(h, "toolhead_map")));

    h.resize(1, 1, kWide.w, kWide.h);
    CHECK_FALSE(shown(part(h, "toolhead_rows")));
    CHECK(shown(part(h, "toolhead_plate")));
    CHECK(shown(part(h, "toolhead_values")));
    CHECK_FALSE(shown(part(h, "toolhead_footer_home")));
    CHECK_FALSE(shown(part(h, "toolhead_footer_status")));

    h.resize(1, 1, kFull.w, kFull.h);
    CHECK(shown(part(h, "toolhead_plate")));
    CHECK(shown(part(h, "toolhead_values")));
    // Homed and idle: nothing to add under the values.
    CHECK_FALSE(shown(part(h, "toolhead_footer_home")));
    CHECK_FALSE(shown(part(h, "toolhead_footer_status")));
}

TEST_CASE_METHOD(ToolheadFixture,
                 "toolhead: Glance and Strip swap the letters for Z while printing",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    set_position(10000, 10000, 1234);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());

    for (const auto& b : {kGlance, kStrip}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(shown(part(h, "toolhead_letters")));
        CHECK_FALSE(shown(part(h, "toolhead_zonly")));

        set_print_active(true);
        CHECK_FALSE(shown(part(h, "toolhead_letters")));
        CHECK(shown(part(h, "toolhead_zonly")));
        settle();
        CHECK(subject_text("toolhead_z_text") == "12.34");

        set_print_active(false);
    }
}

TEST_CASE_METHOD(ToolheadFixture,
                 "toolhead: an unhomed axis reads a dash, a homed one its position",
                 "[widget_size][toolhead]") {
    set_position(12750, -1000, 2000);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    settle();
    CHECK(subject_text("toolhead_x_text") == "--");
    CHECK(subject_text("toolhead_y_text") == "--");
    CHECK(subject_text("toolhead_z_text") == "--");
    CHECK(subject_int("toolhead_homed_x") == 0);

    set_homed("xy");
    settle();
    CHECK(subject_text("toolhead_x_text") == "127.5");
    CHECK(subject_text("toolhead_y_text") == "-10.0");
    CHECK(subject_text("toolhead_z_text") == "--");
    CHECK(subject_int("toolhead_homed_x") == 1);
    CHECK(subject_int("toolhead_homed_y") == 1);
    CHECK(subject_int("toolhead_homed_z") == 0);

    set_homed("xyz");
    settle();
    CHECK(subject_text("toolhead_z_text") == "20.00");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: the live position leads only while the toolhead moves",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    set_position(10000, 10000, 1000);
    set_live_position(10500, 10500, 1500);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    settle();
    CHECK(subject_text("toolhead_x_text") == "100.0");

    set_velocity(80);
    settle();
    CHECK(subject_text("toolhead_x_text") == "105.0");
    CHECK(subject_text("toolhead_z_text") == "15.00");

    set_velocity(0);
    settle();
    CHECK(subject_text("toolhead_x_text") == "100.0");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: position updates are coalesced to the refresh period",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    set_position(10000, 10000, 1000);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    settle();
    REQUIRE(subject_text("toolhead_x_text") == "100.0");

    for (int x = 10100; x <= 10500; x += 100) {
        set_position(x, 10000, 1000);
        process_lvgl(10);
        drain();
    }
    // Still inside the period the first update armed: nothing republished yet.
    CHECK(subject_text("toolhead_x_text") == "100.0");
    settle();
    CHECK(subject_text("toolhead_x_text") == "105.0");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: a rung that shows no positions does no work per tick",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, 55, 54);
    settle();
    const std::string before = subject_text("toolhead_x_text");
    set_position(12345, 12345, 1234);
    settle();
    CHECK(subject_text("toolhead_x_text") == before);
    CHECK_FALSE(h.widget().plate_geometry().has_value());
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: homed letters go green, unhomed red",
                 "[widget_size][toolhead]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, 55, 54);
    const lv_color_t ok = theme_manager_get_color("success");
    const lv_color_t bad = theme_manager_get_color("danger");
    const auto color = [&](const char* name) {
        return lv_obj_get_style_text_color(part(h, name), LV_PART_MAIN);
    };

    CHECK(lv_color_eq(color("toolhead_letter_x"), bad));
    set_homed("xy");
    CHECK(lv_color_eq(color("toolhead_letter_x"), ok));
    CHECK(lv_color_eq(color("toolhead_letter_y"), ok));
    CHECK(lv_color_eq(color("toolhead_letter_z"), bad));
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: content sits vertically centred at the small rungs",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    struct Case {
        Box box;
        const char* section;
    };
    const Case cases[] = {{kGlance, "toolhead_letters"},
                          {kStrip, "toolhead_letters"},
                          {kColumn, "toolhead_rows"},
                          {kCell, "toolhead_rows"}};
    for (const auto& c : cases) {
        INFO(c.box.w << "x" << c.box.h << " " << c.section);
        h.resize(1, 1, c.box.w, c.box.h);
        lv_obj_t* section = part(h, c.section);
        lv_area_t box;
        lv_area_t content;
        lv_obj_get_coords(h.root(), &box);
        lv_obj_get_coords(section, &content);
        const int above = content.y1 - box.y1;
        const int below = box.y2 - content.y2;
        CHECK(std::abs(above - below) <= 2);
    }
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: Home All shows at Full, idle, unhomed, and sends G28",
                 "[widget_size][toolhead][actions]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    CHECK(shown(part(h, "toolhead_footer_home")));
    CHECK(shown(part(h, "toolhead_home_all")));
    CHECK(subject_int("toolhead_show_home") == 1);

    tap(part(h, "toolhead_home_all"));
    CHECK(last_sent() == "G28");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: Home All is gone once homed and while printing",
                 "[widget_size][toolhead][actions]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(shown(part(h, "toolhead_home_all")));

    set_homed("xyz");
    CHECK_FALSE(shown(part(h, "toolhead_footer_home")));
    h.widget().home_all();
    CHECK(last_sent() == "M117 sentinel");

    set_homed("xy");
    REQUIRE(shown(part(h, "toolhead_home_all")));
    set_print_active(true);
    CHECK_FALSE(shown(part(h, "toolhead_footer_home")));
    h.widget().home_all();
    CHECK(last_sent() == "M117 sentinel");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: below Full there is no Home All to tap",
                 "[widget_size][toolhead][actions]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kWide.w, kWide.h);
    CHECK_FALSE(shown(part(h, "toolhead_footer_home")));
    h.widget().home_all();
    CHECK(last_sent() == "M117 sentinel");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: printing at Full shows the tool and the live speed",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    set_print_active(true);
    set_velocity(120);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    settle();
    CHECK(shown(part(h, "toolhead_footer_status")));
    CHECK_FALSE(shown(part(h, "toolhead_footer_home")));
    CHECK(subject_text("toolhead_speed_text") == "120 mm/s");
    // A single-tool printer has no tool to name.
    CHECK(subject_text("toolhead_tool_text").empty());

    set_velocity(95);
    settle();
    CHECK(subject_text("toolhead_speed_text") == "95 mm/s");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: a tool changer names its active tool at Full",
                 "[widget_size][toolhead]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array(
        {"gcode_move", "toolhead", "extruder", "extruder1", "toolchanger", "tool T0", "tool T1"}));
    state().set_hardware(hw);
    ToolState::instance().init_tools(hw);
    drain();
    set_homed("xyz");
    set_print_active(true);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    settle();
    CHECK_FALSE(subject_text("toolhead_tool_text").empty());
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: the crosshair follows the position on the plate",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    state().update_from_status(
        {{"toolhead", {{"axis_minimum", {0, 0, 0, 0}}, {"axis_maximum", {250, 250, 200, 0}}}}});
    drain();
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kWide.w, kWide.h);
    settle();

    set_position(0, 0, 0);
    settle();
    auto g = h.widget().plate_geometry();
    REQUIRE(g.has_value());
    REQUIRE(g->show_crosshair);
    const float x0 = g->cross_x;
    const float y0 = g->cross_y;

    set_position(10000, 10000, 5000);
    settle();
    g = h.widget().plate_geometry();
    REQUIRE(g.has_value());
    CHECK(g->cross_x > x0);
    CHECK(g->cross_y < y0);
    CHECK(g->z_fraction > 0.0f);

    set_homed("");
    settle();
    g = h.widget().plate_geometry();
    REQUIRE(g.has_value());
    CHECK_FALSE(g->show_crosshair);
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: a bed that moves in Z hangs the bar from the top",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kWide.w, kWide.h);
    settle();
    REQUIRE(h.widget().plate_geometry().has_value());
    CHECK_FALSE(h.widget().plate_geometry()->fill_from_top);

    lv_subject_set_int(state().capabilities_state().subject(Capability::BedMoves), 1);
    settle();
    CHECK(h.widget().plate_geometry()->fill_from_top);
    lv_subject_set_int(state().capabilities_state().subject(Capability::BedMoves), 0);
    drain();
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: Home All does not open Motion, a tile tap does",
                 "[widget_size][toolhead][actions]") {
    std::array<lv_obj_t*, 7> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);

    tap(part(h, "toolhead_home_all"));
    process_lvgl(50);
    CHECK_FALSE(get_global_motion_panel().is_visible());

    tap(h.root());
    process_lvgl(500);
    CHECK(get_global_motion_panel().is_visible());
}

TEST_CASE_METHOD(ToolheadFixture,
                 "toolhead: a transient instance does not publish over the live tile",
                 "[widget_size][toolhead]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));

    ToolheadWidget transient(state());
    transient.notify_size_changed(1, 1, 55, 54);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: a transient instance leaving hands the tile back",
                 "[widget_size][toolhead][actions]") {
    PanelWidgetHarness<ToolheadWidget> live(test_screen(), state());
    live.resize(1, 1, kFull.w, kFull.h);
    {
        PanelWidgetHarness<ToolheadWidget> transient(test_screen(), state());
        transient.resize(1, 1, 55, 54);
        REQUIRE(rung_subject() == static_cast<int>(SizeRung::Glance));
        // The displaced tile does not act while the transient owns the subjects.
        live.widget().home_all();
        CHECK(last_sent() == "M117 sentinel");
    }

    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
    live.widget().home_all();
    CHECK(last_sent() == "G28");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: fits_at refuses what Glance cannot draw",
                 "[widget_size][toolhead]") {
    ToolheadWidget widget(state());
    CHECK(widget.fits_at(55, 54));
    CHECK(widget.fits_at(55, 113));
    CHECK(widget.fits_at(114, 54));
    CHECK_FALSE(widget.fits_at(20, 20));
    {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);
        CHECK(widget.fits_at(51, 53));
        CHECK(widget.fits_at(51, 112));
        CHECK(widget.fits_at(108, 53));
    }
    {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Micro);
        CHECK(widget.fits_at(70, 64));
    }
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: the default 2x2 span lands on its rung at each tier",
                 "[widget_size][toolhead]") {
    // The plate and its values need more room than the small tiers' 2x2 holds, so they
    // draw the smaller rung their pixels allow rather than clip.
    struct Tier {
        UiBreakpoint bp;
        int w;
        int h;
        SizeRung rung;
    };
    const Tier tiers[] = {
        {UiBreakpoint::Micro, 142, 131, SizeRung::Cell},
        {UiBreakpoint::Tiny, 166, 155, SizeRung::Wide},
        {UiBreakpoint::Small, 163, 152, SizeRung::Wide},
        {UiBreakpoint::Medium, 233, 230, SizeRung::Full},
        {UiBreakpoint::Large, 221, 230, SizeRung::Full},
        {UiBreakpoint::XLarge, 276, 275, SizeRung::Full},
    };
    for (const auto& t : tiers) {
        INFO(static_cast<int>(t.bp) << " " << t.w << "x" << t.h);
        helix::test::ScopedBreakpoint bp(t.bp);
        PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
        h.resize(1, 1, t.w, t.h);
        CHECK(rung_subject() == static_cast<int>(t.rung));
    }
}

TEST_CASE_METHOD(ToolheadFixture,
                 "toolhead: several tools on one extruder still name the active tool",
                 "[widget_size][toolhead]") {
    PrinterDiscovery hw;
    hw.parse_objects(
        json::array({"gcode_move", "toolhead", "extruder", "toolchanger", "tool T0", "tool T1"}));
    state().set_hardware(hw);
    ToolState::instance().init_tools(hw);
    drain();
    REQUIRE(ToolState::instance().is_multi_tool());
    REQUIRE_FALSE(ToolState::instance().has_multiple_extruders());
    set_homed("xyz");
    set_print_active(true);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    settle();
    CHECK_FALSE(subject_text("toolhead_tool_text").empty());
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: AMS slots on one extruder name no tool",
                 "[widget_size][toolhead]") {
    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    ToolState::instance().set_ams_topology(topo);
    drain();
    REQUIRE(ToolState::instance().is_multi_tool());
    set_homed("xyz");
    set_print_active(true);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    settle();
    CHECK(subject_text("toolhead_tool_text").empty());
    ToolState::instance().clear_ams_topology();
    drain();
}

TEST_CASE_METHOD(ToolheadFixture,
                 "toolhead: a new rung redraws from live state without a position event",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    set_position(10000, 5000, 1000);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    settle();

    h.resize(1, 1, kWide.w, kWide.h);
    settle();
    auto in = h.widget().drawn_plate_input();
    REQUIRE(in.has_value());
    CHECK(in->homed);
    CHECK(in->align_start);
    CHECK(in->x_mm == Catch::Approx(100.0f));
    CHECK(in->y_mm == Catch::Approx(50.0f));

    h.resize(1, 1, kFull.w, kFull.h);
    settle();
    in = h.widget().drawn_plate_input();
    REQUIRE(in.has_value());
    CHECK_FALSE(in->align_start);
}

TEST_CASE_METHOD(ToolheadFixture,
                 "toolhead: an offscreen panel does no redraw work until it returns",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    set_position(10000, 5000, 1000);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kWide.w, kWide.h);
    settle();
    REQUIRE(h.widget().drawn_plate_input()->x_mm == Catch::Approx(100.0f));

    h.widget().on_deactivate();
    set_position(15000, 5000, 1000);
    settle();
    CHECK(h.widget().drawn_plate_input()->x_mm == Catch::Approx(100.0f));

    h.widget().on_activate();
    settle();
    CHECK(h.widget().drawn_plate_input()->x_mm == Catch::Approx(150.0f));
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: only a delta draws a round plate",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    {
        PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
        h.resize(1, 1, kWide.w, kWide.h);
        settle();
        REQUIRE(h.widget().drawn_plate_input().has_value());
        CHECK_FALSE(h.widget().drawn_plate_input()->circular);
    }

    mock_api.hardware().set_kinematics("delta");
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kWide.w, kWide.h);
    settle();
    REQUIRE(h.widget().drawn_plate_input().has_value());
    CHECK(h.widget().drawn_plate_input()->circular);
    mock_api.hardware().set_kinematics("");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: any velocity at all counts as moving",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    set_position(10000, 10000, 1000);
    set_live_position(10500, 10500, 1500);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    settle();
    REQUIRE(subject_text("toolhead_x_text") == "100.0");

    set_velocity(1);
    settle();
    CHECK(subject_text("toolhead_x_text") == "105.0");
}

// ============================================================================
// The interactive plate
// ============================================================================

namespace {

/// A homed, idle, connected printer with a 0..250 x 0..250 x 0..200 envelope, the nozzle
/// at Z 0.2, a bed occupancy state of its own, and a widget at the default 2x2.
struct PlateFixture : public ToolheadFixture {
    PlateFixture() : occupancy(state(), nullptr) {
        std::array<lv_obj_t*, 7> panels{};
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());

        // The widget's XML binds bed_occupied, so the state exists before the tile.
        occupancy.init_subjects();
        drain();
        previous_occupancy = get_bed_occupancy_state();
        set_bed_occupancy_state(&occupancy);

        state().update_from_status(
            {{"toolhead", {{"axis_minimum", {0, 0, 0, 0}}, {"axis_maximum", {250, 250, 200, 0}}}}});
        lv_subject_set_int(state().network_state().get_nav_buttons_enabled_subject(), 1);
        set_homed("xyz");
        set_position(10000, 10000, 20);
        drain();
        mock.clear_gcode_script_history();
    }
    ~PlateFixture() override {
        set_bed_occupancy_state(previous_occupancy);
        lv_subject_set_int(state().network_state().get_nav_buttons_enabled_subject(), 0);
    }

    std::string all_scripts() const {
        std::string joined;
        for (const auto& script : mock.gcode_script_history())
            joined += script + "\n";
        return joined;
    }
    size_t xy_moves() const {
        size_t n = 0;
        for (const auto& script : mock.gcode_script_history()) {
            if (script.find("G0 X") != std::string::npos ||
                script.find("G1 X") != std::string::npos)
                ++n;
        }
        return n;
    }

    /// The screen point at the drawn plate's centre.
    static lv_point_t plate_centre(PanelWidgetHarness<ToolheadWidget>& h) {
        const auto g = h.widget().plate_geometry();
        REQUIRE(g.has_value());
        lv_area_t c;
        lv_obj_get_coords(part(h, "toolhead_plate"), &c);
        return {c.x1 + static_cast<int32_t>(g->plate.x + g->plate.w / 2),
                c.y1 + static_cast<int32_t>(g->plate.y + g->plate.h / 2)};
    }
    /// Press and release at @p at, then the CLICKED the indev sends after them.
    void tap_plate(PanelWidgetHarness<ToolheadWidget>& h, lv_point_t at) {
        h.widget().handle_plate_touch(BedTouch::Pressed, at);
        h.widget().handle_plate_touch(BedTouch::Released, at);
        lv_obj_send_event(part(h, "toolhead_plate"), LV_EVENT_CLICKED, nullptr);
        drain();
    }
    /// A Full tile, settled, with the plate on screen.
    void build(PanelWidgetHarness<ToolheadWidget>& h) {
        h.resize(2, 2, kFull.w, kFull.h);
        settle();
        REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));
        mock.clear_gcode_script_history();
    }

    BedOccupancyState occupancy;
    BedOccupancyState* previous_occupancy = nullptr;
};

} // namespace

TEST_CASE_METHOD(PlateFixture, "toolhead plate: a homed tap drops a target and moves nothing",
                 "[widget_size][toolhead][interactive]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build(h);
    REQUIRE(h.widget().plate_interactive());
    CHECK(lv_obj_has_flag(part(h, "toolhead_plate"), LV_OBJ_FLAG_CLICKABLE));

    tap_plate(h, plate_centre(h));
    process_lvgl(500);

    CHECK(subject_int("toolhead_ghost_active") == 1);
    REQUIRE(h.widget().ghost_target().has_value());
    CHECK(*h.widget().ghost_target()->x == Catch::Approx(125.0).margin(2.0));
    CHECK(*h.widget().ghost_target()->y == Catch::Approx(125.0).margin(2.0));
    CHECK(shown(part(h, "toolhead_move_here")));
    CHECK(subject_text("toolhead_ghost_text").rfind("-> X12", 0) == 0);
    // From Z 0.2 the move lifts to the 5mm clearance first, and says so.
    CHECK(subject_text("toolhead_ghost_text").find("will lift to 5mm") != std::string::npos);
    CHECK(all_scripts().empty());
    CHECK_FALSE(get_global_motion_panel().is_visible());
}

TEST_CASE_METHOD(PlateFixture, "toolhead plate: Move here lifts, then travels, then clears",
                 "[widget_size][toolhead][interactive]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build(h);
    tap_plate(h, plate_centre(h));
    REQUIRE(subject_int("toolhead_ghost_active") == 1);

    lv_obj_send_event(part(h, "toolhead_move_here"), LV_EVENT_CLICKED, nullptr);
    drain();

    const std::string scripts = all_scripts();
    INFO(scripts);
    const auto lift = scripts.find("Z5");
    const auto travel = scripts.find(" X12");
    REQUIRE(lift != std::string::npos);
    REQUIRE(travel != std::string::npos);
    CHECK(lift < travel);
    CHECK(xy_moves() == 1);
    CHECK(subject_int("toolhead_ghost_active") == 0);
    CHECK_FALSE(h.widget().ghost_target().has_value());
    CHECK_FALSE(get_global_motion_panel().is_visible());
}

TEST_CASE_METHOD(PlateFixture, "toolhead plate: dismiss clears the target and sends nothing",
                 "[widget_size][toolhead][interactive]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build(h);
    tap_plate(h, plate_centre(h));
    REQUIRE(subject_int("toolhead_ghost_active") == 1);
    CHECK_FALSE(lv_obj_has_flag(part(h, "toolhead_target_readout"), LV_OBJ_FLAG_HIDDEN));

    lv_obj_send_event(part(h, "toolhead_ghost_dismiss"), LV_EVENT_CLICKED, nullptr);
    drain();
    CHECK(subject_int("toolhead_ghost_active") == 0);
    CHECK(subject_int("toolhead_target_shown") == 0);
    CHECK(lv_obj_has_flag(part(h, "toolhead_target_readout"), LV_OBJ_FLAG_HIDDEN));
    CHECK(all_scripts().empty());
}

TEST_CASE_METHOD(PlateFixture, "toolhead plate: an unconfirmed target clears after its timeout",
                 "[widget_size][toolhead][interactive]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build(h);
    tap_plate(h, plate_centre(h));
    REQUIRE(subject_int("toolhead_ghost_active") == 1);

    process_lvgl(ToolheadWidget::kGhostTimeoutMs - 500);
    CHECK(subject_int("toolhead_ghost_active") == 1);
    process_lvgl(501);
    drain();
    CHECK(subject_int("toolhead_ghost_active") == 0);
    CHECK(all_scripts().empty());
}

TEST_CASE_METHOD(PlateFixture, "toolhead plate: unhomed or printing, a tap opens Motion",
                 "[widget_size][toolhead][interactive]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build(h);
    const lv_point_t centre = plate_centre(h);

    SECTION("unhomed") {
        set_homed("xy");
    }
    SECTION("printing") {
        set_print_active(true);
    }
    settle();
    CHECK_FALSE(h.widget().plate_interactive());
    CHECK(subject_int("toolhead_plate_interactive") == 0);
    // Not clickable, so a real tap lands on the tile itself.
    CHECK_FALSE(lv_obj_has_flag(part(h, "toolhead_plate"), LV_OBJ_FLAG_CLICKABLE));

    tap_plate(h, centre);
    process_lvgl(500);
    CHECK(subject_int("toolhead_ghost_active") == 0);
    CHECK(all_scripts().empty());
    CHECK(get_global_motion_panel().is_visible());
}

TEST_CASE_METHOD(PlateFixture, "toolhead plate: a tap on the Z bar opens Motion and sends nothing",
                 "[widget_size][toolhead][interactive]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build(h);
    REQUIRE(h.widget().plate_interactive());
    const auto g = h.widget().plate_geometry();
    REQUIRE(g.has_value());
    lv_area_t c;
    lv_obj_get_coords(part(h, "toolhead_plate"), &c);
    const lv_point_t bar{c.x1 + static_cast<int32_t>(g->z_bar.x + g->z_bar.w / 2),
                         c.y1 + static_cast<int32_t>(g->z_bar.y + g->z_bar.h / 2)};

    tap_plate(h, bar);
    process_lvgl(500);
    CHECK(subject_int("toolhead_ghost_active") == 0);
    CHECK(all_scripts().empty());
    CHECK(get_global_motion_panel().is_visible());
}

TEST_CASE_METHOD(PlateFixture, "toolhead plate: a drag moves the head live and drops no target",
                 "[widget_size][toolhead][interactive]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build(h);
    const lv_point_t from = plate_centre(h);
    const lv_point_t to{from.x + 30, from.y};

    h.widget().handle_plate_touch(BedTouch::Pressed, from);
    h.widget().handle_plate_touch(BedTouch::Pressing, to);
    drain();
    CHECK(subject_int("toolhead_target_shown") == 1);
    CHECK(subject_text("toolhead_ghost_text").rfind("-> X", 0) == 0);
    h.widget().handle_plate_touch(BedTouch::Released, to);
    lv_obj_send_event(part(h, "toolhead_plate"), LV_EVENT_CLICKED, nullptr);
    drain();

    INFO(all_scripts());
    CHECK(xy_moves() >= 1);
    CHECK(subject_int("toolhead_ghost_active") == 0);
    CHECK(subject_int("toolhead_target_shown") == 0);
    CHECK_FALSE(get_global_motion_panel().is_visible());
}

TEST_CASE_METHOD(PlateFixture,
                 "toolhead plate: a drag sample queued behind a move dies with the gate",
                 "[widget_size][toolhead][interactive]") {
    HoldingAPI held_api{mock, state()};
    set_moonraker_api(&held_api);
    auto& moves = held_api.holding_.held;
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build(h);
    const lv_point_t from = plate_centre(h);

    h.widget().handle_plate_touch(BedTouch::Pressed, from);
    h.widget().handle_plate_touch(BedTouch::Pressing, {from.x + 30, from.y});
    drain();
    REQUIRE(moves.size() == 1);
    h.widget().handle_plate_touch(BedTouch::Pressing, {from.x + 40, from.y + 10});
    drain();
    REQUIRE(moves.size() == 1); // the second sample waits behind the first

    // A print started from elsewhere closes the gate while the first move is in flight.
    lv_subject_set_int(state().network_state().get_nav_buttons_enabled_subject(), 0);
    drain();
    moves[0].on_success();
    drain();
    CHECK(moves.size() == 1);

    set_moonraker_api(&mock_api);
}

TEST_CASE_METHOD(PlateFixture, "toolhead plate: Cleared shows while a part may be on the bed",
                 "[widget_size][toolhead][interactive]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build(h);
    CHECK_FALSE(shown(part(h, "toolhead_bed_cleared")));

    BedOccupancyStateTestAccess::set_verdict(occupancy, {true, 38.0, "job-on-the-bed"});
    drain();
    CHECK(shown(part(h, "toolhead_bed_cleared")));
    // A target showing hides it, so the two never stack.
    tap_plate(h, plate_centre(h));
    CHECK_FALSE(shown(part(h, "toolhead_bed_cleared")));
    CHECK(subject_text("toolhead_ghost_text").find("last print is 38mm tall") != std::string::npos);
    h.widget().clear_ghost();
    drain();
    REQUIRE(shown(part(h, "toolhead_bed_cleared")));

    // The chip asks first: cancelling keeps the part, and its lift, on the bed.
    lv_obj_send_event(part(h, "toolhead_bed_cleared"), LV_EVENT_CLICKED, nullptr);
    drain();
    helix::test::answer_top_modal("btn_secondary");
    CHECK(lv_subject_get_int(occupancy.get_occupied_subject()) == 1);

    lv_obj_send_event(part(h, "toolhead_bed_cleared"), LV_EVENT_CLICKED, nullptr);
    drain();
    CHECK(lv_subject_get_int(occupancy.get_occupied_subject()) == 1);
    helix::test::answer_top_modal("btn_primary");
    CHECK(lv_subject_get_int(occupancy.get_occupied_subject()) == 0);
    CHECK_FALSE(get_global_motion_panel().is_visible());
}

TEST_CASE_METHOD(PlateFixture, "toolhead plate: a detached tile's target timer never fires",
                 "[widget_size][toolhead][interactive]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build(h);
    tap_plate(h, plate_centre(h));
    REQUIRE(subject_int("toolhead_ghost_active") == 1);

    h.widget().detach();
    CHECK_FALSE(h.widget().ghost_target().has_value());
    CHECK(subject_int("toolhead_ghost_active") == 0);
    process_lvgl(ToolheadWidget::kGhostTimeoutMs + 100);
    drain();
    CHECK(all_scripts().empty());
}

// ============================================================================
// Preset slots (a 2x3 tile)
// ============================================================================

namespace {

/// A 2x3 tile at the medium tier: two cells across, three down.
constexpr Box kTall{helix::test::kTileWideW, 3 * helix::test::kTileCellH + 12, SizeRung::Full};

struct PresetFixture : public PlateFixture {
    void build_tall(PanelWidgetHarness<ToolheadWidget>& h) {
        h.resize(2, 3, kTall.w, kTall.h);
        settle();
        REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));
        REQUIRE(subject_int("toolhead_show_presets") == 1);
        mock.clear_gcode_script_history();
    }
    void tap_slot(PanelWidgetHarness<ToolheadWidget>& h, int index) {
        const std::string name = "toolhead_preset_" + std::to_string(index);
        lv_obj_send_event(part(h, name.c_str()), LV_EVENT_CLICKED, nullptr);
        drain();
    }
    static json slots_config(std::initializer_list<json> entries) {
        json array = json::array();
        for (const auto& entry : entries)
            array.push_back(entry);
        return json{{"presets", array}};
    }
};

} // namespace

TEST_CASE_METHOD(PresetFixture, "toolhead presets: a 2x3 shows the row, a 2x2 does not",
                 "[widget_size][toolhead][presets]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build_tall(h);
    CHECK(shown(part(h, "toolhead_presets")));
    CHECK(shown(part(h, "toolhead_lift_line")));
    CHECK(subject_text("toolhead_preset_0_label") == "Front");
    CHECK(subject_text("toolhead_preset_1_label") == "Center");
    CHECK(subject_text("toolhead_preset_2_label") == "Park");
    // The row sits below the plate, and the plate still spans the tile.
    lv_obj_update_layout(part(h, "toolhead_plate"));
    lv_area_t plate;
    lv_area_t row;
    lv_obj_get_coords(part(h, "toolhead_plate"), &plate);
    lv_obj_get_coords(part(h, "toolhead_presets"), &row);
    CHECK(row.y1 > plate.y2);
    lv_area_t tile;
    lv_obj_get_coords(h.root(), &tile);
    CHECK(row.y2 <= tile.y2);

    h.resize(2, 2, kFull.w, kFull.h);
    settle();
    CHECK(subject_int("toolhead_show_presets") == 0);
    CHECK_FALSE(shown(part(h, "toolhead_presets")));
    CHECK_FALSE(shown(part(h, "toolhead_lift_line")));
}

TEST_CASE_METHOD(PresetFixture, "toolhead presets: a position lifts off the plate first",
                 "[widget_size][toolhead][presets]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build_tall(h);
    // From Z 0.2 with nothing on the bed the move rises to the 5mm clearance.
    CHECK(subject_text("toolhead_lift_text").find("will lift to 5mm") != std::string::npos);

    tap_slot(h, 0);
    const std::string scripts = all_scripts();
    INFO(scripts);
    const auto lift = scripts.find("Z5");
    REQUIRE(lift != std::string::npos);
    CHECK(xy_moves() == 1);
    CHECK_FALSE(get_global_motion_panel().is_visible());
}

TEST_CASE_METHOD(PresetFixture, "toolhead presets: a part too tall to clear is announced",
                 "[widget_size][toolhead][presets]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build_tall(h);
    // Z travel is 200mm; a 199.5mm part leaves under 1mm above it.
    BedOccupancy verdict;
    verdict.occupied = true;
    verdict.part_height_mm = 199.5;
    verdict.job_id = "job-on-the-bed";
    BedOccupancyStateTestAccess::set_verdict(occupancy, verdict);
    drain();
    CHECK(subject_text("toolhead_lift_text") == part_too_tall_text());
}

TEST_CASE_METHOD(PresetFixture, "toolhead presets: Z down never ends below a part on the bed",
                 "[widget_size][toolhead][presets]") {
    PanelWidgetHarness<ToolheadWidget> h(
        test_screen(),
        HarnessConfig{slots_config(
            {json{{"kind", "z_down"}}, json{{"kind", "z_up"}}, json{{"kind", "empty"}}})},
        state());
    set_position(10000, 10000, 2000);
    BedOccupancyStateTestAccess::set_verdict(occupancy, {true, 38.0, "job-on-the-bed"});
    drain();
    build_tall(h);
    CHECK(subject_text("toolhead_preset_0_label") == "Z -10");
    CHECK(subject_int("toolhead_preset_2_state") == 0);
    CHECK_FALSE(shown(part(h, "toolhead_preset_2")));

    tap_slot(h, 0);
    INFO(all_scripts());
    CHECK(all_scripts().find("Z43") != std::string::npos);
    CHECK(xy_moves() == 0);
}

TEST_CASE_METHOD(PresetFixture, "toolhead presets: Z up adds the step to the current height",
                 "[widget_size][toolhead][presets]") {
    PanelWidgetHarness<ToolheadWidget> h(
        test_screen(), HarnessConfig{slots_config({json{{"kind", "z_up"}}})}, state());
    set_position(10000, 10000, 2000);
    build_tall(h);
    tap_slot(h, 0);
    INFO(all_scripts());
    CHECK(all_scripts().find("Z30") != std::string::npos);
}

TEST_CASE_METHOD(PresetFixture, "toolhead presets: unhomed, the moving slots are inert",
                 "[widget_size][toolhead][presets]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build_tall(h);
    set_homed("");
    settle();
    mock.clear_gcode_script_history();
    for (int i = 0; i < 3; ++i) {
        const std::string name = "toolhead_preset_" + std::to_string(i);
        INFO(name);
        CHECK(lv_obj_has_state(part(h, name.c_str()), LV_STATE_DISABLED));
        tap_slot(h, i);
    }
    CHECK(all_scripts().empty());
    CHECK_FALSE(get_global_motion_panel().is_visible());
}

TEST_CASE_METHOD(PresetFixture, "toolhead presets: the gear's picker changes a slot",
                 "[widget_size][toolhead][presets]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build_tall(h);
    REQUIRE(h.widget().has_edit_configure());
    h.widget().on_edit_configure();
    ToolheadPresetConfigModal* modal = h.widget().preset_config_modal();
    REQUIRE(modal != nullptr);
    REQUIRE(modal->is_visible());

    modal->select_slot(2);
    // The choices run nine positions, Park, Z up, Z down, Macro, Empty.
    const auto choices = ToolheadPresetConfigModal::choices();
    REQUIRE(choices.size() == 14);
    REQUIRE(choices[10].kind == ToolheadPresetSlot::Kind::ZUp);
    modal->pick_choice(10);
    drain();
    CHECK(h.widget().preset_slots()[2].kind == ToolheadPresetSlot::Kind::ZUp);
    CHECK(subject_text("toolhead_preset_2_label") == "Z +10");

    // Macro opens the macro list; a macro picked there fills the slot.
    modal->select_slot(0);
    modal->pick_choice(12);
    CHECK(modal->showing_macros());
    CHECK(h.widget().preset_slots()[0].kind == ToolheadPresetSlot::Kind::Position);
    modal->pick_macro("CLEAN_NOZZLE");
    drain();
    CHECK_FALSE(modal->showing_macros());
    CHECK(h.widget().preset_slots()[0].kind == ToolheadPresetSlot::Kind::Macro);
    CHECK(h.widget().preset_slots()[0].macro == "CLEAN_NOZZLE");
    CHECK(subject_int("toolhead_preset_0_state") == 2);
    modal->hide();
    drain();
}

TEST_CASE_METHOD(PresetFixture, "toolhead presets: detaching the widget closes its picker",
                 "[widget_size][toolhead][presets]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    build_tall(h);
    h.widget().on_edit_configure();
    REQUIRE(h.widget().preset_config_modal() != nullptr);
    REQUIRE(h.widget().preset_config_modal()->is_visible());

    h.widget().detach();
    drain();
    CHECK(h.widget().preset_config_modal() == nullptr);
}

TEST_CASE_METHOD(PresetFixture, "toolhead plate: the Move-here chip fits the plate at every rung",
                 "[widget_size][toolhead][interactive]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    for (const Box& box : {kWide, kFull, kTall}) {
        INFO(box.w << "x" << box.h);
        h.resize(2, 2, box.w, box.h);
        settle();
        REQUIRE(subject_int("toolhead_show_plate") == 1);
        tap_plate(h, plate_centre(h));
        REQUIRE(subject_int("toolhead_ghost_active") == 1);
        lv_obj_t* chip = part(h, "toolhead_ghost_chip");
        lv_obj_update_layout(chip);
        CHECK(lv_obj_get_width(chip) <= lv_obj_get_width(part(h, "toolhead_plate")));
        h.widget().clear_ghost();
        drain();
    }
    // The wide tile's half-width plate takes the chip without its label.
    h.resize(2, 1, kWide.w, kWide.h);
    settle();
    CHECK(subject_int("toolhead_chip_icon_only") == 1);
    h.resize(2, 2, kFull.w, kFull.h);
    settle();
    CHECK(subject_int("toolhead_chip_icon_only") == 0);
}
