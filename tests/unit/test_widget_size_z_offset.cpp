// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_z_offset.cpp
 * @brief The z_offset widget draws what its ladder promises at each rung: which parts
 * carry HIDDEN, which text they show, and what its buttons send.
 *
 * Boxes are the pixel budgets measured on a live grid (55x54, 114x54, 55x113, 114x113 at
 * medium; 51x53 / 51x112 / 108x53 at large; 70x64 at micro). The decision is
 * z_offset_layout.h's; these tests pin its wiring to the real fonts and XML.
 *
 * Run with: ./build/bin/helix-tests "[widget_size][z_offset]"
 */

#include "ui_modal.h"
#include "ui_print_tune_overlay.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/tile_boxes.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/z_offset_widget.h"
#include "static_panel_registry.h"
#include "tool_state.h"
#include "z_offset_utils.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::test::Box;
using helix::test::kFull;
using helix::test::kWide;
using nlohmann::json;

namespace {

struct ZOffsetFixture : public LVGLUITestFixture {
    ZOffsetFixture() : mock(MoonrakerClientMock::PrinterType::VORON_24), mock_api(mock, state()) {
        // The Tune overlay is a lazily-created singleton; a case that opened it must not
        // leave it visible for the next.
        StaticPanelRegistry::instance().destroy_all();
        state().set_klippy_state_sync(KlippyState::READY);
        mock.connect("ws://mock/websocket", []() {}, []() {});
        previous_api = get_moonraker_api();
        set_moonraker_api(&mock_api);
        PrinterStateTestAccess::pin_z_offset_strategy(state(),
                                                      ZOffsetCalibrationStrategy::PROBE_CALIBRATE);

        ToolState::instance().deinit_subjects();
        ToolState::instance().init_subjects(true);
        PrinterDiscovery hw;
        hw.parse_objects(json::array({"gcode_move"}));
        ToolState::instance().init_tools(hw);
        zoffset::deinit_save_available_subject();
        zoffset::init_save_available_subject(true);

        PanelWidgetManager::instance().init_widget_subjects();
        zoffset::set_persisted_step_index(zoffset::kZStepDefaultIndex);
        reset_state();
        sentinel();
    }
    ~ZOffsetFixture() override {
        ModalStack::instance().clear();
        StaticPanelRegistry::instance().destroy_all();
        reset_state();
        zoffset::set_persisted_step_index(zoffset::kZStepDefaultIndex);
        set_moonraker_api(previous_api);
        mock.stop_temperature_simulation();
        mock.disconnect();
        zoffset::deinit_save_available_subject();
        ToolState::instance().deinit_subjects();
        drain();
    }

    void reset_state() {
        set_print_active(false);
        set_offset_microns(0);
        state().motion_state().clear_pending_z_offset_delta();
        lv_subject_set_int(ToolState::instance().get_per_tool_axis_supported_subject(Axis::Z), 0);
        drain();
    }
    void set_print_active(bool on) {
        lv_subject_set_int(state().print_state().get_print_active_subject(), on ? 1 : 0);
        drain();
    }
    void set_machine_blocked(bool on) {
        lv_subject_set_int(state().print_state().get_machine_motion_blocked_subject(), on ? 1 : 0);
        drain();
    }
    void set_offset_microns(int microns) {
        lv_subject_set_int(state().motion_state().get_gcode_z_offset_subject(), microns);
        drain();
    }
    void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
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

    /// A tool changer with two tools, the first reporting @p microns.
    void make_tool_changer(int microns) {
        PrinterDiscovery hw;
        hw.parse_objects(json::array(
            {"gcode_move", "toolhead", "extruder", "toolchanger", "tool T0", "tool T1"}));
        state().set_hardware(hw);
        ToolState::instance().init_tools(hw);
        lv_subject_set_int(ToolState::instance().get_per_tool_axis_supported_subject(Axis::Z), 1);
        ToolState::instance().set_tool_offset_local(0, Axis::Z, microns);
        drain();
    }

    MoonrakerClientMock mock;
    MoonrakerAPI mock_api;
    IMoonrakerAPI* previous_api = nullptr;
};

int rung_subject() {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, "z_offset_rung");
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

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

lv_obj_t* part(PanelWidgetHarness<ZOffsetWidget>& h, const char* name) {
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

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: medium-tier budgets land on their rung",
                 "[widget_size][z_offset]") {
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    const auto& boxes = test::kMediumLadder;
    for (const auto& b : boxes) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(rung_subject() == static_cast<int>(b.rung));
    }
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: a box draws the same parts whatever it was before",
                 "[widget_size][z_offset]") {
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    CHECK(h.unsettled_boxes(box_sweep(234, 234, 40, 260)) == std::vector<std::string>{});
    CHECK(h.unsettled_boxes(box_sweep(20, 300, 113, 113)) == std::vector<std::string>{});
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: each rung shows exactly its parts",
                 "[widget_size][z_offset]") {
    set_offset_microns(-100); // something to save, so Save may show
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());

    h.resize(1, 1, 55, 54);
    CHECK(shown(part(h, "z_offset_icon")));
    CHECK(shown(part(h, "z_offset_value")));
    CHECK_FALSE(shown(part(h, "z_offset_unit")));
    CHECK_FALSE(shown(part(h, "z_offset_caption")));
    CHECK_FALSE(shown(part(h, "z_offset_indicator")));
    CHECK_FALSE(shown(part(h, "z_offset_save_wide")));
    CHECK_FALSE(shown(part(h, "z_offset_steppers")));

    // The tall half cell has room for the unit, the caption is a Cell thing.
    h.resize(1, 1, 55, 113);
    CHECK(shown(part(h, "z_offset_unit")));
    CHECK_FALSE(shown(part(h, "z_offset_caption")));

    h.resize(1, 1, 114, 113);
    CHECK_FALSE(shown(part(h, "z_offset_unit")));
    CHECK(shown(part(h, "z_offset_icon")));
    CHECK(shown(part(h, "z_offset_value")));
    CHECK(shown(part(h, "z_offset_caption")));
    CHECK_FALSE(shown(part(h, "z_offset_indicator")));
    CHECK_FALSE(shown(part(h, "z_offset_save_wide")));

    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(shown(part(h, "z_offset_caption")));
    CHECK(shown(part(h, "z_offset_indicator")));
    CHECK(shown(part(h, "z_offset_save_wide")));
    CHECK_FALSE(shown(part(h, "z_offset_save_full")));
    CHECK_FALSE(shown(part(h, "z_offset_steppers")));

    h.resize(1, 1, kFull.w, kFull.h);
    CHECK(shown(part(h, "z_offset_indicator")));
    CHECK(shown(part(h, "z_offset_steppers")));
    CHECK(shown(part(h, "z_offset_save_full")));
    CHECK_FALSE(shown(part(h, "z_offset_save_wide")));
    CHECK_FALSE(shown(part(h, "z_offset_tool")));
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: Save shows only while there is something to save",
                 "[widget_size][z_offset]") {
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, kWide.w, kWide.h);
    CHECK_FALSE(shown(part(h, "z_offset_save_wide")));
    set_offset_microns(-100);
    CHECK(shown(part(h, "z_offset_save_wide")));
    set_offset_microns(0);
    CHECK_FALSE(shown(part(h, "z_offset_save_wide")));

    h.resize(1, 1, kFull.w, kFull.h);
    CHECK_FALSE(shown(part(h, "z_offset_save_full")));
    set_offset_microns(-100);
    CHECK(shown(part(h, "z_offset_save_full")));
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: Save disables while a job holds the machine",
                 "[widget_size][z_offset]") {
    set_offset_microns(-100);
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    CHECK_FALSE(lv_obj_has_state(part(h, "z_offset_save_full"), LV_STATE_DISABLED));
    set_machine_blocked(true);
    CHECK(lv_obj_has_state(part(h, "z_offset_save_full"), LV_STATE_DISABLED));
    set_machine_blocked(false);
    CHECK_FALSE(lv_obj_has_state(part(h, "z_offset_save_full"), LV_STATE_DISABLED));
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: the value reads two decimals at Glance, three above",
                 "[widget_size][z_offset]") {
    set_offset_microns(-126);
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());

    h.resize(1, 1, 55, 54);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Glance));
    CHECK(subject_text("z_offset_value_text") == "-0.13");
    h.resize(1, 1, 114, 54);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Strip));
    CHECK(subject_text("z_offset_value_text") == "-0.126");
    h.resize(1, 1, 114, 113);
    CHECK(subject_text("z_offset_value_text") == "-0.126");
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: the value follows the persisted offset while idle",
                 "[widget_size][z_offset]") {
    // Firmware that persists the offset itself reads 0 live at idle; the value shown is
    // the stored one, and the live one takes over mid-print.
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    auto& motion = state().motion_state();
    lv_subject_set_int(motion.get_persisted_z_offset_subject(), -250);
    lv_subject_set_int(motion.get_persisted_z_offset_valid_subject(), 1);
    drain();
    CHECK(subject_text("z_offset_value_text") == "-0.250");

    set_offset_microns(-300);
    set_print_active(true);
    CHECK(subject_text("z_offset_value_text") == "-0.300");

    lv_subject_set_int(motion.get_persisted_z_offset_valid_subject(), 0);
    set_print_active(false);
    drain();
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: the caption is the unit, or the unsaved session delta",
                 "[widget_size][z_offset]") {
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    CHECK(subject_text("z_offset_caption_text") == "mm Z-offset");

    state().motion_state().add_pending_z_offset_delta(50);
    drain();
    CHECK(subject_text("z_offset_caption_text") == "+0.050 unsaved");

    state().motion_state().clear_pending_z_offset_delta();
    drain();
    CHECK(subject_text("z_offset_caption_text") == "mm Z-offset");
}

TEST_CASE_METHOD(ZOffsetFixture,
                 "z_offset: the value draws amber at every rung while a save is due",
                 "[widget_size][z_offset]") {
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    const lv_color_t warning = theme_manager_get_color("warning");
    const auto value_color = [&] {
        return lv_obj_get_style_text_color(part(h, "z_offset_value"), LV_PART_MAIN);
    };
    CHECK_FALSE(lv_color_eq(value_color(), warning));

    set_offset_microns(-100);
    REQUIRE(subject_int("z_offset_status") == 1);
    for (const Box& b : test::kMediumLadder) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        REQUIRE(rung_subject() == static_cast<int>(b.rung));
        CHECK(lv_color_eq(value_color(), warning));
    }

    set_offset_microns(0);
    CHECK(subject_int("z_offset_status") == 0);
    CHECK_FALSE(lv_color_eq(value_color(), warning));
}

TEST_CASE_METHOD(ZOffsetFixture,
                 "z_offset: a transient instance does not publish over the live tile",
                 "[widget_size][z_offset]") {
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));

    ZOffsetWidget transient(state());
    transient.notify_size_changed(1, 1, 55, 54);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: fits_at refuses what Glance cannot draw",
                 "[widget_size][z_offset]") {
    ZOffsetWidget widget(state());
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

    bool fitted = false;
    for (int w = 0; w <= 200; ++w) {
        const bool now = widget.fits_at(w, 113);
        if (fitted)
            CHECK(now);
        fitted = fitted || now;
    }
    bool fitted_h = false;
    for (int hgt = 0; hgt <= 200; ++hgt) {
        const bool now = widget.fits_at(114, hgt);
        if (fitted_h)
            CHECK(now);
        fitted_h = fitted_h || now;
    }
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: half-cell boxes at large and micro land on a rung",
                 "[widget_size][z_offset]") {
    {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);
        PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
        h.resize(1, 1, 51, 53);
        CHECK(rung_subject() == static_cast<int>(SizeRung::Glance));
        h.resize(1, 1, 108, 53);
        CHECK(rung_subject() == static_cast<int>(SizeRung::Strip));
        h.resize(1, 1, 51, 112);
        CHECK(rung_subject() == static_cast<int>(SizeRung::Column));
        h.resize(1, 1, 108, 112);
        CHECK(rung_subject() == static_cast<int>(SizeRung::Cell));
    }
}

TEST_CASE_METHOD(ZOffsetFixture,
                 "z_offset: tool changer shows its tool's offset and name from Full",
                 "[widget_size][z_offset]") {
    make_tool_changer(-100);
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());

    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(subject_text("z_offset_value_text") == "-0.100");
    CHECK_FALSE(shown(part(h, "z_offset_tool")));

    h.resize(1, 1, 234, 260);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));
    CHECK(subject_int("z_offset_show_tool_label") == 1);
    CHECK(shown(part(h, "z_offset_tool")));
    CHECK(subject_text("z_offset_tool_text").find("1") != std::string::npos);

    // The machine-wide offset does not move the tool's value.
    set_offset_microns(-500);
    CHECK(subject_text("z_offset_value_text") == "-0.100");
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: a tool that has reported nothing reads --",
                 "[widget_size][z_offset]") {
    make_tool_changer(0);
    lv_subject_set_int(ToolState::instance().get_active_tool_subject(), 1);
    drain();
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    CHECK(subject_text("z_offset_value_text") == "--");
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: steppers send the persisted step, idle and printing",
                 "[widget_size][z_offset][actions]") {
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(subject_int("z_offset_show_steppers") == 1);

    tap(part(h, "z_offset_farther"));
    CHECK(last_sent().find("SET_GCODE_OFFSET Z_ADJUST=0.010") == 0);
    tap(part(h, "z_offset_closer"));
    CHECK(last_sent().find("SET_GCODE_OFFSET Z_ADJUST=-0.010") == 0);

    zoffset::set_persisted_step_index(0);
    drain();
    tap(part(h, "z_offset_farther"));
    CHECK(last_sent().find("SET_GCODE_OFFSET Z_ADJUST=0.050") == 0);

    set_print_active(true);
    REQUIRE(subject_int("z_offset_show_steppers") == 1);
    zoffset::set_persisted_step_index(3);
    tap(part(h, "z_offset_closer"));
    CHECK(last_sent().find("SET_GCODE_OFFSET Z_ADJUST=-0.005") == 0);
}

TEST_CASE_METHOD(ZOffsetFixture,
                 "z_offset: a stepper press moves the value and arms the unsaved caption",
                 "[widget_size][z_offset][actions]") {
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    tap(part(h, "z_offset_closer"));
    drain();
    CHECK(subject_text("z_offset_value_text") == "-0.010");
    CHECK(subject_text("z_offset_caption_text") == "-0.010 unsaved");
    CHECK(subject_int("z_offset_status") == 1);
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: the step chip cycles the shared step table and wraps",
                 "[widget_size][z_offset][actions]") {
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    CHECK(subject_text("z_offset_step_text") == "0.01");

    tap(part(h, "z_offset_step_chip"));
    CHECK(zoffset::persisted_step_index() == 3);
    CHECK(subject_text("z_offset_step_text") == "0.005");
    tap(part(h, "z_offset_step_chip"));
    CHECK(zoffset::persisted_step_index() == 0);
    CHECK(subject_text("z_offset_step_text") == "0.05");
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: session travel is bounded from where the steps began",
                 "[widget_size][z_offset][actions]") {
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    zoffset::set_persisted_step_index(0); // 0.05mm: forty steps reach the 2mm limit

    for (int i = 0; i < 40; ++i)
        tap(part(h, "z_offset_farther"));
    CHECK(subject_text("z_offset_value_text") == "2.000");
    sentinel();
    tap(part(h, "z_offset_farther"));
    CHECK(last_sent() == "M117 sentinel");

    // A saved session starts the next run from where it ended.
    state().motion_state().clear_pending_z_offset_delta();
    tap(part(h, "z_offset_farther"));
    CHECK(last_sent().find("SET_GCODE_OFFSET") == 0);
}

TEST_CASE_METHOD(ZOffsetFixture,
                 "z_offset: a tool changer's steppers adjust the tool, not the machine",
                 "[widget_size][z_offset][actions]") {
    make_tool_changer(-100);
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 260);
    REQUIRE(subject_int("z_offset_show_steppers") == 1);

    tap(part(h, "z_offset_farther"));
    CHECK(last_sent() == "SET_TOOL_PARAMETER T=0 PARAMETER=gcode_z_offset VALUE=-0.090");
    CHECK(ToolState::instance().tool_offset_mm(0, Axis::Z) == Catch::Approx(-0.09));
    CHECK(subject_text("z_offset_value_text") == "-0.090");
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: hidden controls send nothing",
                 "[widget_size][z_offset][actions]") {
    set_offset_microns(-100);
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    // Wide has no steppers and Cell has no Save; the objects still exist, so a stray
    // event must not reach the printer.
    h.resize(1, 1, kWide.w, kWide.h);
    REQUIRE(subject_int("z_offset_show_steppers") == 0);
    tap(part(h, "z_offset_farther"));
    tap(part(h, "z_offset_closer"));
    tap(part(h, "z_offset_step_chip"));
    CHECK(last_sent() == "M117 sentinel");
    CHECK(zoffset::persisted_step_index() == zoffset::kZStepDefaultIndex);

    h.resize(1, 1, 114, 113);
    REQUIRE(subject_int("z_offset_show_save") == 0);
    tap(part(h, "z_offset_save_wide"));
    tap(part(h, "z_offset_save_full"));
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(ZOffsetFixture,
                 "z_offset: Save starts the shared save only when one is due and allowed",
                 "[widget_size][z_offset][actions]") {
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);

    // Nothing to save.
    tap(part(h, "z_offset_save_full"));
    CHECK(ModalStack::instance().stack_empty());

    // A job holds the machine.
    set_offset_microns(-100);
    set_machine_blocked(true);
    tap(part(h, "z_offset_save_full"));
    CHECK(ModalStack::instance().stack_empty());

    // The shared save warns before it restarts Klipper; nothing is sent until the
    // user confirms.
    set_machine_blocked(false);
    tap(part(h, "z_offset_save_full"));
    CHECK(ModalStack::instance().top_dialog() != nullptr);
    CHECK(last_sent() == "M117 sentinel");

    ModalStack::instance().clear();
    zoffset::reset_shared_save();
}

TEST_CASE_METHOD(ZOffsetFixture,
                 "z_offset: a stepper tap does not open the Tune overlay, a tile tap does",
                 "[widget_size][z_offset][actions]") {
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);

    tap(part(h, "z_offset_farther"));
    process_lvgl(50);
    CHECK_FALSE(get_print_tune_overlay().is_visible());

    tap(h.root());
    process_lvgl(500);
    CHECK(get_print_tune_overlay().is_visible());
}

TEST_CASE_METHOD(ZOffsetFixture,
                 "z_offset: a tile built over a pending delta bounds travel from its offset",
                 "[widget_size][z_offset][actions]") {
    // A delta left pending by an earlier surface must not leave the base at zero: the
    // 2mm window would then sit around zero and clamp a step taken at 1.98mm.
    state().motion_state().add_pending_z_offset_delta(50);
    set_offset_microns(1980);
    PanelWidgetHarness<ZOffsetWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    zoffset::set_persisted_step_index(0);

    tap(part(h, "z_offset_farther"));
    CHECK(last_sent().find("SET_GCODE_OFFSET Z_ADJUST=0.050") == 0);
}

TEST_CASE_METHOD(ZOffsetFixture, "z_offset: a transient instance leaving hands the tile back",
                 "[widget_size][z_offset][actions]") {
    PanelWidgetHarness<ZOffsetWidget> live(test_screen(), state());
    live.resize(1, 1, kFull.w, kFull.h);
    {
        PanelWidgetHarness<ZOffsetWidget> transient(test_screen(), state());
        transient.resize(1, 1, 55, 54);
        REQUIRE(rung_subject() == static_cast<int>(SizeRung::Glance));
    }

    // The live tile owns its subjects again and still acts.
    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
    set_offset_microns(-100);
    CHECK(subject_text("z_offset_value_text") == "-0.100");
    tap(part(live, "z_offset_farther"));
    CHECK(last_sent().find("SET_GCODE_OFFSET Z_ADJUST=0.010") == 0);
}
