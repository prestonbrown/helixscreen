// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_nozzle_temps.cpp
 * @brief nozzle_temps rows take their label, width and font from the layout
 * subjects on_size_changed publishes — including rows built by a LATE
 * rebuild, which read the current subject values the moment they are created.
 *
 * `on_size_changed` (nozzle_temps_widget.cpp) measures text, computes
 * `decide_nozzle_layout()`, and publishes the whole verdict as three subjects
 * (`nozzle_row_label_mode`, `nozzle_row_columns`, `nozzle_row_compact`).
 * Everything the rows show is bound off them in XML: which of the three
 * spelling labels is hidden, the row width, the container flow, the compact
 * font. `PanelWidgetManager` always calls `on_size_changed()` synchronously
 * right after `attach()`, so a row built at attach() is correct before
 * anything paints. But `rebuild_rows()` also runs later, off
 * `extruder_version_subject` (late tool discovery, a reconnect) — and a real
 * touchscreen never gets a second `on_size_changed` after that. A row built
 * by that second rebuild is what these tests target: it must agree with its
 * siblings because its binds read the still-current subjects, not because
 * anything re-applied a decision to it.
 *
 * Each case resizes the widget with a colspan that *contradicts* the old
 * ">= 2" rule (colspan=1 with a wide grant; colspan=2 with a narrow one), so
 * a span-reading implementation fails here instead of passing by coincidence
 * — same technique as test_widget_size_fan_stack.cpp.
 *
 * The mock `--test` printer backend only ever exposes one extruder, so the
 * 2-row path below is unreachable by driving the real app; these tests
 * populate `ToolState` and `PrinterState` directly to reach it.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_state.h"
#include "panel_widget_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/nozzle_layout.h"
#include "src/ui/panel_widgets/nozzle_temps_widget.h"
#include "tool_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

/// Fixture teardown for the ToolState singleton: it is not part of
/// LVGLUITestFixture's own init/deinit chain (that fixture only owns
/// PrinterState-family subjects), so a test that populates it must clear it
/// itself or later test files in the same binary inherit stale tools.
///
/// AmsState::instance() is cleared for the same reason: ToolState's short
/// labels are the active AMS backend's lane_noun(), so a backend a prior file
/// left registered would leak into this file's expected "Slot N" strings.
struct NozzleTempsFixture : public LVGLUITestFixture {
    NozzleTempsFixture() {
        AmsState::instance().clear_backends();
        // The layout subjects register on SubjectInitializer's panel-subject
        // phase in the app; the harness builds widgets directly, so drive the
        // same phase by hand (test_widget_size_camera.cpp does the same).
        PanelWidgetManager::instance().init_widget_subjects();
    }
    ~NozzleTempsFixture() override {
        ToolState::instance().deinit_subjects();
        AmsState::instance().clear_backends();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

/// One extruder, one tool ("T0" -> "extruder"), matching the real mock
/// printer's single-nozzle topology.
void configure_one_extruder(PrinterState& state) {
    state.init_extruders({"extruder"});

    ToolState::instance().deinit_subjects();
    ToolState::instance().init_subjects(false);
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"extruder", "heater_bed", "gcode_move"}));
    ToolState::instance().init_tools(hw);
}

/// Adds a second physical extruder ("T1" -> "extruder1") behind a
/// toolchanger. ToolState::tools() is updated FIRST so that when
/// PrinterState::init_extruders() bumps extruder_version_subject, the
/// deferred rebuild it triggers reads the already-current tool list rather
/// than racing it. observe_int_sync's handler is queued via
/// helix::ui::queue_update() (NOT called inline from lv_subject_set_int —
/// "sync" describes when the *handler body* runs relative to the widget's
/// own state, not when it runs relative to this call), so the caller must
/// drain the UpdateQueue for NozzleTempsWidget::version_observer_ to fire.
void add_second_extruder(PrinterState& state) {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "extruder",
                                            "extruder1", "heater_bed", "gcode_move"}));
    ToolState::instance().init_tools(hw);

    state.init_extruders({"extruder", "extruder1"});
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}

/// Adds a third physical extruder, so a rebuild can be provoked after the
/// widget has already settled into its two-column layout. Same ordering rule as
/// add_second_extruder(): ToolState first, then the version bump, then drain.
void add_third_extruder(PrinterState& state) {
    PrinterDiscovery hw;
    hw.parse_objects(
        nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "tool T2", "extruder",
                               "extruder1", "extruder2", "heater_bed", "gcode_move"}));
    ToolState::instance().init_tools(hw);

    state.init_extruders({"extruder", "extruder1", "extruder2"});
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}

/// The nth row's declared width. Two-column layout resolves to lv_pct(48) via
/// the bound nozzle_row_half style; one-column to 100%.
int32_t nth_row_width(lv_obj_t* container, int index) {
    lv_obj_t* row = lv_obj_get_child(container, index);
    REQUIRE(row != nullptr);
    return lv_obj_get_style_width(row, LV_PART_MAIN);
}

/// One of the nth row's three spelling labels, found by container child index
/// (rows share label names, so a container-wide find_by_name would only ever
/// return the first row's).
lv_obj_t* nth_row_label(lv_obj_t* container, int index, const char* name) {
    lv_obj_t* row = lv_obj_get_child(container, index);
    REQUIRE(row != nullptr);
    lv_obj_t* label = lv_obj_find_by_name(row, name);
    REQUIRE(label != nullptr);
    return label;
}

/// The published label-mode subject. Requires the widget to have been
/// constructed at least once in this process (the ctor registers it).
int label_mode_subject_value() {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, "nozzle_row_label_mode");
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

/// Every spelling label except the one the mode selects must be hidden, and
/// the value's target half hides below the text rungs — the bound outcome,
/// not a cached decision.
void check_one_label_visible(lv_obj_t* container, int index, NozzleLabelMode mode) {
    lv_obj_t* long_lbl = nth_row_label(container, index, "tool_label_long");
    lv_obj_t* short_lbl = nth_row_label(container, index, "tool_label_short");
    lv_obj_t* number_lbl = nth_row_label(container, index, "tool_label_number");
    lv_obj_t* target_lbl = nth_row_label(container, index, "target_label");
    CHECK(lv_obj_has_flag(long_lbl, LV_OBJ_FLAG_HIDDEN) == (mode != NozzleLabelMode::Long));
    CHECK(lv_obj_has_flag(short_lbl, LV_OBJ_FLAG_HIDDEN) == (mode != NozzleLabelMode::Short));
    CHECK(lv_obj_has_flag(number_lbl, LV_OBJ_FLAG_HIDDEN) == (mode != NozzleLabelMode::Number));
    // The number/icon rungs budget the current-only value: the target half
    // is hidden there and drawn at the text rungs.
    const bool text_mode = mode == NozzleLabelMode::Long || mode == NozzleLabelMode::Short;
    CHECK(lv_obj_has_flag(target_lbl, LV_OBJ_FLAG_HIDDEN) != text_mode);
}

} // namespace

TEST_CASE_METHOD(
    NozzleTempsFixture,
    "nozzle_temps: a row built by a late rebuild reuses the wide-grant label decision, "
    "not a colspan=1 span",
    "[widget_size][nozzle_temps]") {
    configure_one_extruder(state());

    PanelWidgetHarness<NozzleTempsWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("nozzle_temps_container");
    REQUIRE(container != nullptr);
    REQUIRE(lv_obj_get_child_count(container) == 2); // 1 extruder row + bed row

    // colspan=1 (old rule: current_colspan_ >= 2 is false -> short label) but
    // generously wide pixels, so decide_nozzle_layout() picks the long form.
    h.resize(1, 1, 600, 300);

    // Sanity: the resize itself drove the subject to Long, and the row that
    // already existed shows it — proves the bind really applied the verdict.
    REQUIRE(label_mode_subject_value() == static_cast<int>(NozzleLabelMode::Long));
    REQUIRE(std::string(lv_label_get_text(nth_row_label(container, 0, "tool_label_long"))) ==
            "Nozzle");
    check_one_label_visible(container, 0, NozzleLabelMode::Long);

    // Late tool discovery: a second extruder appears well after the widget
    // already knows it is wide. No further on_size_changed() call happens —
    // a real touchscreen never gets one after initial layout.
    add_second_extruder(state());
    REQUIRE(lv_obj_get_child_count(container) == 3); // 2 extruder rows + bed row

    // The freshly created second row must show the long form too: its binds
    // read the still-current subjects at creation. A colspan=1 reading
    // implementation shows the short form here instead.
    CHECK(std::string(lv_label_get_text(nth_row_label(container, 1, "tool_label_long"))) ==
          "Nozzle 2");
    check_one_label_visible(container, 1, NozzleLabelMode::Long);

    // Rows occupy distinct cells: at this width the widget is two-column, so
    // the second row sits to the RIGHT of the first, not piled onto it.
    lv_obj_update_layout(container);
    const int32_t x0 = lv_obj_get_x(lv_obj_get_child(container, 0));
    const int32_t x1 = lv_obj_get_x(lv_obj_get_child(container, 1));
    INFO("rows at x " << x0 << " and " << x1);
    CHECK(x1 > x0);
}

TEST_CASE_METHOD(
    NozzleTempsFixture,
    "nozzle_temps: a row built by a late rebuild reuses the narrow-grant label decision, "
    "not a colspan=2 span",
    "[widget_size][nozzle_temps]") {
    configure_one_extruder(state());

    PanelWidgetHarness<NozzleTempsWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("nozzle_temps_container");
    REQUIRE(container != nullptr);
    REQUIRE(lv_obj_get_child_count(container) == 2);

    // colspan=2 (old rule: long label) but a 100px grant, which no spelling of
    // the label fits beside the value at the normal font — the ladder lands on
    // a narrower rung.
    h.resize(2, 1, 100, 300);

    const int mode = label_mode_subject_value();
    REQUIRE(mode != static_cast<int>(NozzleLabelMode::Long));
    check_one_label_visible(container, 0, static_cast<NozzleLabelMode>(mode));

    add_second_extruder(state());
    REQUIRE(lv_obj_get_child_count(container) == 3);

    // The freshly created row shows the same rung as its sibling. A colspan=2
    // reading implementation leaves the new row's long label visible here.
    CHECK(label_mode_subject_value() == mode);
    check_one_label_visible(container, 1, static_cast<NozzleLabelMode>(mode));
    CHECK(lv_obj_has_flag(nth_row_label(container, 1, "tool_label_long"), LV_OBJ_FLAG_HIDDEN));

    // One column at this width, so rows stack DOWNWARD: the second row's Y is
    // past the first's. A container whose bound style never activated a layout
    // leaves every row at y=0, piled onto the first.
    CHECK(lv_obj_get_style_layout(container, LV_PART_MAIN) == LV_LAYOUT_FLEX);
    lv_obj_update_layout(container);
    const int32_t y0 = lv_obj_get_y(lv_obj_get_child(container, 0));
    const int32_t y1 = lv_obj_get_y(lv_obj_get_child(container, 1));
    INFO("rows at y " << y0 << " and " << y1);
    CHECK(y1 > y0);

    // The bed row follows the same mode subject: its label hides for the icon
    // and number rungs (the radiator glyph already identifies it) and returns
    // for the text rungs.
    lv_obj_t* bed_row = lv_obj_get_child(container, 2);
    lv_obj_t* bed_label = lv_obj_find_by_name(bed_row, "bed_label");
    lv_obj_t* bed_target = lv_obj_find_by_name(bed_row, "bed_target_label");
    REQUIRE(bed_label != nullptr);
    REQUIRE(bed_target != nullptr);
    lv_subject_t* mode_subject = lv_xml_get_subject(nullptr, "nozzle_row_label_mode");
    REQUIRE(mode_subject != nullptr);
    lv_subject_set_int(mode_subject, 0);
    CHECK(lv_obj_has_flag(bed_label, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(bed_target, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(mode_subject, 1);
    CHECK(lv_obj_has_flag(bed_label, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(bed_target, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(mode_subject, 2);
    CHECK_FALSE(lv_obj_has_flag(bed_label, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(bed_target, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(mode_subject, 3);
    CHECK_FALSE(lv_obj_has_flag(bed_label, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(bed_target, LV_OBJ_FLAG_HIDDEN));
}

/**
 * decide_nozzle_layout()'s columns == 2 branch requires row_count >= 2, which
 * the real `--test` mock printer can never supply — it only ever exposes one
 * extruder. This test proves the branch is still reachable through the widget
 * itself (as opposed to only through decide_nozzle_layout()'s own pure-function
 * tests) by driving ToolState to two extruders the same way the tests above
 * do, then resizing generously enough for two short-label columns to fit.
 */
TEST_CASE_METHOD(NozzleTempsFixture,
                 "nozzle_temps: two extruders reach the two-column layout through the widget",
                 "[widget_size][nozzle_temps]") {
    configure_one_extruder(state());

    PanelWidgetHarness<NozzleTempsWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("nozzle_temps_container");
    REQUIRE(container != nullptr);

    add_second_extruder(state());
    REQUIRE(lv_obj_get_child_count(container) == 3);

    // Wide enough that even two short-label columns plus the inter-row gap
    // fit (nozzle_layout.h: avail_px >= 2*short_row_px + gap_px).
    h.resize(1, 1, 600, 300);

    lv_subject_t* columns = lv_xml_get_subject(nullptr, "nozzle_row_columns");
    REQUIRE(columns != nullptr);
    REQUIRE(lv_subject_get_int(columns) == 2);
    // The layout must be ACTIVE, not just the flow style value: a style
    // carrying flex_flow without layout="flex" leaves the container with no
    // layout, every row piles onto the first, and get_style_flex_flow still
    // reports the flow. Two-column rows sit side by side, so the second row's
    // X is past the first's.
    CHECK(lv_obj_get_style_layout(container, LV_PART_MAIN) == LV_LAYOUT_FLEX);
    CHECK(lv_obj_get_style_flex_flow(container, LV_PART_MAIN) == LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_update_layout(container);
    const int32_t x0 = lv_obj_get_x(lv_obj_get_child(container, 0));
    const int32_t x1 = lv_obj_get_x(lv_obj_get_child(container, 1));
    INFO("rows at x " << x0 << " and " << x1);
    CHECK(x1 > x0);
}

/**
 * A two-column layout halves every row's width, and the width is a bound
 * style reading the same columns subject: a row created at any point joins
 * the layout its siblings already have. A row that kept a template default
 * of 100% would span the container and overlap its neighbour
 * (prestonbrown/helixscreen#1490).
 */
TEST_CASE_METHOD(NozzleTempsFixture,
                 "nozzle_temps: a row built by a late rebuild joins the two-column layout",
                 "[widget_size][nozzle_temps][1490]") {
    configure_one_extruder(state());
    PanelWidgetHarness<NozzleTempsWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("nozzle_temps_container");
    REQUIRE(container != nullptr);

    // Two extruders BEFORE the size pass, so the layout really is two-column
    // when the late rebuild lands.
    add_second_extruder(state());
    REQUIRE(lv_obj_get_child_count(container) == 3); // 2 extruders + bed

    h.resize(1, 1, 600, 300);
    REQUIRE(lv_obj_get_style_layout(container, LV_PART_MAIN) == LV_LAYOUT_FLEX);
    REQUIRE(lv_obj_get_style_flex_flow(container, LV_PART_MAIN) == LV_FLEX_FLOW_ROW_WRAP);
    const int32_t half = lv_obj_get_style_width(lv_obj_get_child(container, 0), LV_PART_MAIN);
    REQUIRE(half == lv_pct(48)); // sanity: the two-column width really was applied

    // Late tool discovery. No second on_size_changed() follows, exactly as on a
    // real screen.
    add_third_extruder(state());
    REQUIRE(lv_obj_get_child_count(container) == 4); // 3 extruders + bed

    // Every row, including the one that did not exist when the size arrived,
    // must carry the two-column width. A row that kept a template default of
    // 100% would overlap its neighbour.
    for (int i = 0; i < 4; i++) {
        INFO("row " << i << " width " << nth_row_width(container, i) << " want " << half);
        CHECK(nth_row_width(container, i) == half);
    }
    CHECK(lv_obj_get_style_flex_flow(container, LV_PART_MAIN) == LV_FLEX_FLOW_ROW_WRAP);
}

TEST_CASE_METHOD(NozzleTempsFixture,
                 "nozzle_temps: setting a target re-decides the ladder — the verdict is not frozen "
                 "at the values the tile was sized under",
                 "[widget_size][nozzle_temps][1613]") {
    configure_one_extruder(state());

    PanelWidgetHarness<NozzleTempsWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("nozzle_temps_container");
    REQUIRE(container != nullptr);

    // Sized while idle: the nozzle-name spelling fits the 137px column at
    // the normal font.
    h.resize(1, 1, 147, 300);
    REQUIRE(label_mode_subject_value() == static_cast<int>(NozzleLabelMode::Long));

    // A print sets an extruder target: the row's value grows by its target
    // half, the spelling no longer fits, and the ladder re-decides to the
    // number rung. Without the re-decide the tile keeps drawing the wide
    // value in the rung chosen for the narrow one.
    state().update_from_status(nlohmann::json{{"extruder", {{"target", 210.0}}}});
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    CHECK(label_mode_subject_value() == static_cast<int>(NozzleLabelMode::Number));

    // The print ends and the target clears: the value narrows again and the
    // ladder re-decides back. The transition fires both ways.
    state().update_from_status(nlohmann::json{{"extruder", {{"target", 0.0}}}});
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    CHECK(label_mode_subject_value() == static_cast<int>(NozzleLabelMode::Long));

    // The bed row's value is part of the same budget, and its observer
    // carries the same duty: with the extruder idle, a wide bed target alone
    // pushes the spelling out.
    h.resize(1, 1, 160, 300);
    REQUIRE(label_mode_subject_value() == static_cast<int>(NozzleLabelMode::Long));
    state().update_from_status(nlohmann::json{{"heater_bed", {{"target", 200.0}}}});
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    CHECK(label_mode_subject_value() == static_cast<int>(NozzleLabelMode::Number));
}
