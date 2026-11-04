// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_speed_flow.cpp
 * @brief The speed_flow widget draws what its ladder promises at each rung: which parts
 * carry HIDDEN, which text they show, and the re-decision when print_active or the
 * physical-units setting changes.
 *
 * Boxes are the pixel budgets measured on a live grid (55x54, 114x54, 55x113, 114x113 at
 * medium; 51x53 / 51x112 / 108x53 at large; 70x64 at micro). The decision is
 * speed_flow_layout.h's; these tests pin its wiring to the real fonts and XML.
 *
 * Run with: ./build/bin/helix-tests "[widget_size][speed_flow]"
 */

#include "ui_print_tune_overlay.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/scoped_pointer_indev.h"
#include "app_globals.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "home_edit_mode_test_helpers.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/speed_flow_widget.h"
#include "tune_controller.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct SpeedFlowFixture : public LVGLUITestFixture {
    SpeedFlowFixture() {
        PanelWidgetManager::instance().init_widget_subjects();
        set_physical_units(false);
        set_print_active(false);
        set_percentages(100, 100);
    }
    ~SpeedFlowFixture() override {
        set_physical_units(false);
        set_print_active(false);
        set_percentages(100, 100);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    void set_physical_units(bool on) {
        DisplaySettingsManager::instance().set_speed_flow_physical_units(on);
        drain();
    }
    void set_print_active(bool on) {
        lv_subject_set_int(state().print_state().get_print_active_subject(), on ? 1 : 0);
        drain();
    }
    void set_percentages(int speed, int flow) {
        lv_subject_set_int(state().motion_state().get_speed_factor_subject(), speed);
        lv_subject_set_int(state().motion_state().get_flow_factor_subject(), flow);
        drain();
    }
    void set_live(int velocity_mm_s, int extruder_centimm_s) {
        lv_subject_set_int(state().motion_state().get_live_velocity_subject(), velocity_mm_s);
        lv_subject_set_int(state().motion_state().get_live_extruder_velocity_subject(),
                           extruder_centimm_s);
        drain();
    }
    void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

int rung_subject() {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, "speed_flow_rung");
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

/// A named part of one axis ("speed" or "flow").
lv_obj_t* part(PanelWidgetHarness<SpeedFlowWidget>& h, const char* axis, const char* name) {
    lv_obj_t* axis_part = h.child(std::string("speed_flow_" + std::string(axis) + "_part").c_str());
    REQUIRE(axis_part != nullptr);
    lv_obj_t* obj = lv_obj_find_by_name(axis_part, name);
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

struct Box {
    int w;
    int h;
    SizeRung rung;
};

} // namespace

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: medium-tier budgets land on their rung",
                 "[widget_size][speed_flow]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    // Half cells, a whole cell, then 2x1 and 2x2 (two cells plus the gutter).
    const Box boxes[] = {
        {55, 54, SizeRung::Glance}, {114, 54, SizeRung::Strip}, {55, 113, SizeRung::Column},
        {114, 113, SizeRung::Cell}, {234, 113, SizeRung::Wide}, {234, 232, SizeRung::Full},
    };
    for (const auto& b : boxes) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(rung_subject() == static_cast<int>(b.rung));
    }
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: a box draws the same parts whatever it was before",
                 "[widget_size][speed_flow]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    CHECK(h.unsettled_boxes(box_sweep(55, 55, 20, 240)) == std::vector<std::string>{});
    CHECK(h.unsettled_boxes(box_sweep(114, 114, 20, 240)) == std::vector<std::string>{});
    CHECK(h.unsettled_boxes(box_sweep(20, 300, 113, 113)) == std::vector<std::string>{});
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: each rung shows exactly its parts",
                 "[widget_size][speed_flow]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());

    h.resize(1, 1, 55, 54);
    CHECK(hidden(h.child("speed_flow_flow_part")));
    CHECK(shown(part(h, "speed", "speed_flow_icon")));
    CHECK(shown(part(h, "speed", "speed_flow_value")));
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_label")));

    h.resize(1, 1, 114, 54);
    CHECK(shown(h.child("speed_flow_flow_part")));
    CHECK(shown(part(h, "flow", "speed_flow_icon")));
    CHECK_FALSE(shown(part(h, "flow", "speed_flow_label")));

    h.resize(1, 1, 114, 113);
    CHECK(shown(part(h, "speed", "speed_flow_icon")));
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_measured_below"))); // idle

    h.resize(1, 1, 234, 113);
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_icon")));
    CHECK(shown(part(h, "speed", "speed_flow_label")));
    CHECK(shown(part(h, "flow", "speed_flow_label")));
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_measured_right"))); // idle

    h.resize(1, 1, 234, 232);
    CHECK(shown(part(h, "speed", "speed_flow_full_label")));
    CHECK(shown(part(h, "speed", "speed_flow_full_value")));
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_main")));
    // Idle at Full is read-only.
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_minus")));
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_plus")));
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: printing adds the measured readings and steppers",
                 "[widget_size][speed_flow]") {
    set_print_active(true);
    set_live(60, 500);
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());

    h.resize(1, 1, 114, 113);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Cell));
    CHECK(shown(part(h, "speed", "speed_flow_measured_below")));
    CHECK(subject_text("speed_flow_speed_measured").find("mm/s") != std::string::npos);
    CHECK(subject_text("speed_flow_speed_text") == "100%");

    h.resize(1, 1, 234, 113);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Wide));
    CHECK(shown(part(h, "speed", "speed_flow_measured_right")));
    CHECK(shown(part(h, "flow", "speed_flow_measured_right")));

    h.resize(1, 1, 234, 232);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));
    CHECK(subject_int("speed_flow_show_steppers") == 1);
    CHECK(shown(part(h, "speed", "speed_flow_minus")));
    CHECK(shown(part(h, "flow", "speed_flow_plus")));
    CHECK(shown(part(h, "speed", "speed_flow_full_measured")));
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: print_active re-decides the rung",
                 "[widget_size][speed_flow]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    // Room for the Full box idle but not printing: the steppers cost width.
    h.resize(1, 1, 234, 232);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));
    CHECK(subject_int("speed_flow_show_steppers") == 0);

    set_print_active(true);
    CHECK(subject_int("speed_flow_show_steppers") == 1);
    set_print_active(false);
    CHECK(subject_int("speed_flow_show_steppers") == 0);
}

TEST_CASE_METHOD(SpeedFlowFixture,
                 "speed_flow: physical units swap the value only below Cell, where it fits",
                 "[widget_size][speed_flow]") {
    set_print_active(true);
    set_live(45, 300);
    set_physical_units(true);
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());

    // The measured reading keeps its unit, so it needs more width than the percentage:
    // a half cell keeps the percentage rather than give up its rung.
    h.resize(1, 1, 55, 54);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Glance));
    CHECK(subject_text("speed_flow_speed_text") == "100%");
    h.resize(1, 1, 114, 54);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Strip));
    CHECK(subject_text("speed_flow_speed_text") == "100");

    // A strip wide enough for both readings swaps, units included.
    h.resize(1, 1, 233, 53);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Strip));
    CHECK(subject_text("speed_flow_speed_text") == "45 mm/s");
    CHECK(subject_text("speed_flow_flow_text").find("mm\xC2\xB3/s") != std::string::npos);

    // With room, the percentage stays primary whatever the setting.
    h.resize(1, 1, 114, 113);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Cell));
    CHECK(subject_text("speed_flow_speed_text") == "100%");

    // Idle never swaps.
    set_print_active(false);
    h.resize(1, 1, 233, 53);
    CHECK(subject_text("speed_flow_speed_text") == "100");
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: the physical-units setting never moves fits_at",
                 "[widget_size][speed_flow]") {
    SpeedFlowWidget widget(state());
    std::vector<bool> without;
    for (int w = 20; w <= 120; w += 4)
        without.push_back(widget.fits_at(w, 50));
    set_physical_units(true);
    set_print_active(true);
    std::vector<bool> with;
    for (int w = 20; w <= 120; w += 4)
        with.push_back(widget.fits_at(w, 50));
    CHECK(with == without);
}

TEST_CASE_METHOD(SpeedFlowFixture,
                 "speed_flow: a transient instance does not publish over the live tile",
                 "[widget_size][speed_flow]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 232);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));

    SpeedFlowWidget transient(state());
    transient.notify_size_changed(1, 1, 55, 54);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
}

TEST_CASE_METHOD(SpeedFlowFixture,
                 "speed_flow: velocity ticks only reformat while a measured reading shows",
                 "[widget_size][speed_flow]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Cell));
    drain(); // the observers' initial notifications
    const std::string idle_text = subject_text("speed_flow_speed_measured");

    set_live(77, 400); // idle: nothing measured is drawn
    CHECK(subject_text("speed_flow_speed_measured") == idle_text);

    set_print_active(true);
    set_live(78, 400);
    CHECK(subject_text("speed_flow_speed_measured") == "78 mm/s");
}

namespace {

/// The speed_flow fixture with a mock API installed as the app's, so a stepper tap's
/// gcode can be read back.
struct SpeedFlowActionFixture : public SpeedFlowFixture {
    SpeedFlowActionFixture()
        : mock(MoonrakerClientMock::PrinterType::VORON_24), mock_api(mock, state()) {
        state().set_klippy_state_sync(KlippyState::READY);
        mock.connect("ws://mock/websocket", []() {}, []() {});
        previous_api = get_moonraker_api();
        set_moonraker_api(&mock_api);
        set_print_active(true);
        sentinel();
    }
    ~SpeedFlowActionFixture() override {
        set_moonraker_api(previous_api);
        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    /// A gcode no action sends, so "nothing was sent" is distinguishable.
    void sentinel() {
        helix::tune::set_speed_percent(&mock_api, 77);
        REQUIRE(last_sent() == "M220 S77");
    }
    const std::string& last_sent() const {
        return mock.last_send_script();
    }
    void tap(lv_obj_t* obj) {
        helix::ui::send_tap(obj);
    }

    MoonrakerClientMock mock;
    MoonrakerAPI mock_api;
    IMoonrakerAPI* previous_api = nullptr;
};

} // namespace

TEST_CASE_METHOD(SpeedFlowActionFixture,
                 "speed_flow: speed steppers step 5 percent, flow 1 percent, clamped",
                 "[widget_size][speed_flow][actions]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 232);
    REQUIRE(subject_int("speed_flow_show_steppers") == 1);

    set_percentages(100, 100);
    tap(part(h, "speed", "speed_flow_plus"));
    CHECK(last_sent() == "M220 S105");
    tap(part(h, "speed", "speed_flow_minus"));
    CHECK(last_sent() == "M220 S95");
    tap(part(h, "flow", "speed_flow_plus"));
    CHECK(last_sent() == "M221 S101");
    tap(part(h, "flow", "speed_flow_minus"));
    CHECK(last_sent() == "M221 S99");

    set_percentages(helix::tune::kSpeedMaxPct, helix::tune::kFlowMaxPct);
    tap(part(h, "speed", "speed_flow_plus"));
    CHECK(last_sent() == "M220 S200");
    tap(part(h, "flow", "speed_flow_plus"));
    CHECK(last_sent() == "M221 S125");

    set_percentages(helix::tune::kSpeedMinPct, helix::tune::kFlowMinPct);
    tap(part(h, "speed", "speed_flow_minus"));
    CHECK(last_sent() == "M220 S50");
    tap(part(h, "flow", "speed_flow_minus"));
    CHECK(last_sent() == "M221 S75");
}

TEST_CASE_METHOD(SpeedFlowActionFixture, "speed_flow: long-press resets that axis to 100",
                 "[widget_size][speed_flow][actions]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 232);
    set_percentages(130, 80);

    lv_obj_send_event(part(h, "speed", "speed_flow_full_value"), LV_EVENT_LONG_PRESSED, nullptr);
    CHECK(last_sent() == "M220 S100");
    lv_obj_send_event(part(h, "flow", "speed_flow_full_value"), LV_EVENT_LONG_PRESSED, nullptr);
    CHECK(last_sent() == "M221 S100");
}

TEST_CASE_METHOD(SpeedFlowActionFixture,
                 "speed_flow: a long-press while the steppers are hidden sends nothing",
                 "[widget_size][speed_flow][actions]") {
    set_print_active(false);
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 232);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));
    REQUIRE(subject_int("speed_flow_show_steppers") == 0);
    set_percentages(130, 80);
    sentinel();

    lv_obj_send_event(part(h, "speed", "speed_flow_full_value"), LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_send_event(part(h, "flow", "speed_flow_full_value"), LV_EVENT_LONG_PRESSED, nullptr);
    CHECK(last_sent() == "M220 S77");
}

TEST_CASE_METHOD(SpeedFlowActionFixture,
                 "speed_flow: a stepper tap does not open the Tune overlay, a tile tap does",
                 "[widget_size][speed_flow][actions]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 232);
    set_percentages(100, 100);

    tap(part(h, "speed", "speed_flow_plus"));
    process_lvgl(50);
    CHECK_FALSE(get_print_tune_overlay().is_visible());

    tap(h.root());
    process_lvgl(500);
    CHECK(get_print_tune_overlay().is_visible());
}

TEST_CASE_METHOD(SpeedFlowActionFixture,
                 "speed_flow: a held value resets its axis and its release opens nothing",
                 "[widget_size][speed_flow][actions][tap_hold]") {
    // The overlay is a lazily created singleton an earlier case may have left
    // open; drop it so this case starts with none.
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(get_print_tune_overlay().is_visible());

    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 232);
    set_percentages(130, 100);
    helix::ui::set_event_bubble_recursive(h.root());
    lv_obj_update_layout(test_screen());

    const auto centre = [](lv_obj_t* obj) {
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        return lv_point_t{(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
    };
    helix_test::ScopedPointerIndev finger;

    lv_obj_t* value = part(h, "speed", "speed_flow_full_value");
    lv_point_t p = centre(value);
    REQUIRE(lv_indev_search_obj(test_screen(), &p) == value);
    finger.press(p.x, p.y);
    finger.hold(finger.long_press_hold_ms());
    finger.release(p.x, p.y);
    process_lvgl(500);
    CHECK(last_sent() == "M220 S100");
    CHECK_FALSE(get_print_tune_overlay().is_visible());

    // A quick tap on a stepper steps, and stops short of the tile's tap.
    set_percentages(100, 100);
    lv_obj_t* plus = part(h, "speed", "speed_flow_plus");
    p = centre(plus);
    REQUIRE(lv_indev_search_obj(test_screen(), &p) == plus);
    finger.press(p.x, p.y);
    finger.release(p.x, p.y);
    process_lvgl(500);
    CHECK(last_sent() == "M220 S105");
    CHECK_FALSE(get_print_tune_overlay().is_visible());
}

TEST_CASE_METHOD(SpeedFlowActionFixture,
                 "speed_flow: a slow tap on a stepper steps unless the home grid takes the hold",
                 "[widget_size][speed_flow][actions][tap_hold][home][grid_edit]") {
    helix_test::ScopedLockState lock_state;
    HomePanel& panel = get_global_home_panel();
    lv_obj_t* page_container = helix_test::make_long_press_container(test_screen());
    helix_test::ScopedHomePanelPage page(panel, page_container);

    PanelWidgetHarness<SpeedFlowWidget> h(page_container, state());
    h.resize(1, 1, 234, 232);
    set_percentages(100, 100);
    helix::ui::set_event_bubble_recursive(page_container);
    lv_obj_update_layout(test_screen());

    lv_obj_t* plus = part(h, "speed", "speed_flow_plus");
    lv_area_t a;
    lv_obj_get_coords(plus, &a);
    lv_point_t p{(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
    REQUIRE(lv_indev_search_obj(test_screen(), &p) == plus);
    helix_test::ScopedPointerIndev finger;
    const auto slow_tap = [&] {
        finger.press(p.x, p.y);
        finger.hold(finger.long_press_hold_ms());
        finger.release(p.x, p.y);
    };

    SECTION("edit mode off: the grid declines the hold and the stepper steps") {
        helix_test::ScopedInputSettings input_settings(false);
        slow_tap();
        CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel));
        CHECK(last_sent() == "M220 S105");
    }

    SECTION("edit mode on: the hold enters edit mode and the stepper does not step") {
        helix_test::ScopedInputSettings input_settings(true);
        slow_tap();
        CHECK(HomePanelTestAccess::edit_mode_active(panel));
        CHECK(last_sent() == "M220 S77");
    }

    SECTION("edit mode on: the entry hold kept down longer never becomes a second hold") {
        helix_test::ScopedInputSettings input_settings(true);
        int holds = 0;
        lv_obj_add_event_cb(
            page_container, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
            LV_EVENT_LONG_PRESSED, &holds);
        finger.press(p.x, p.y);
        finger.hold(3 * finger.long_press_hold_ms());
        finger.release(p.x, p.y);
        CHECK(HomePanelTestAccess::edit_mode_active(panel));
        // A second hold inside edit mode would grab the widget the entry only selected.
        CHECK(holds == 1);
    }
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: an override off 100 draws amber at every rung",
                 "[widget_size][speed_flow]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 113);
    const lv_color_t warning = theme_manager_get_color("warning");
    const lv_color_t normal =
        lv_obj_get_style_text_color(part(h, "flow", "speed_flow_value"), LV_PART_MAIN);

    set_percentages(120, 100);
    for (const auto& b : std::initializer_list<Box>{{55, 54, SizeRung::Glance},
                                                    {114, 54, SizeRung::Strip},
                                                    {55, 113, SizeRung::Column},
                                                    {114, 113, SizeRung::Cell},
                                                    {234, 113, SizeRung::Wide},
                                                    {234, 232, SizeRung::Full}}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        REQUIRE(rung_subject() == static_cast<int>(b.rung));
        REQUIRE(subject_int("speed_flow_speed_status") == 1);
        REQUIRE(subject_int("speed_flow_flow_status") == 0);
        const char* value = b.rung == SizeRung::Full ? "speed_flow_full_value" : "speed_flow_value";
        const lv_color_t speed_color =
            lv_obj_get_style_text_color(part(h, "speed", value), LV_PART_MAIN);
        const lv_color_t flow_color =
            lv_obj_get_style_text_color(part(h, "flow", value), LV_PART_MAIN);
        if (b.rung == SizeRung::Glance)
            continue; // the flow axis is hidden here
        CHECK(lv_color_eq(speed_color, warning));
        CHECK_FALSE(lv_color_eq(flow_color, warning));
    }
    (void)normal;
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: fits_at refuses what Glance cannot draw",
                 "[widget_size][speed_flow]") {
    SpeedFlowWidget widget(state());
    // The measured half-cell budgets at each tier must hold the Glance content.
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

    // Monotonic: once a width fits, every wider one does.
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

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: half-cell boxes at large and micro land on a rung",
                 "[widget_size][speed_flow]") {
    {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);
        PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
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

TEST_CASE_METHOD(SpeedFlowActionFixture,
                 "speed_flow: a transient instance leaving hands the tile back",
                 "[widget_size][speed_flow][actions]") {
    PanelWidgetHarness<SpeedFlowWidget> live(test_screen(), state());
    live.resize(1, 1, 234, 232);
    {
        PanelWidgetHarness<SpeedFlowWidget> transient(test_screen(), state());
        transient.resize(1, 1, 55, 54);
        REQUIRE(rung_subject() == static_cast<int>(SizeRung::Glance));
    }

    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
    set_percentages(120, 100);
    CHECK(subject_text("speed_flow_speed_text") == "120%");
    tap(part(live, "speed", "speed_flow_plus"));
    CHECK(last_sent() == "M220 S125");
}
