// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_nozzle_temps.cpp
 * @brief nozzle_temps picks a fresh row's label from the same pixel-based
 * decision decide_nozzle_layout() already applied to its siblings, not from
 * the grid colspan the row happens to be created under.
 *
 * `on_size_changed` (nozzle_temps_widget.cpp) is already pixel-driven for
 * rows that already exist when it runs: it measures text, computes
 * `decide_nozzle_layout()`, and rewrites every existing row's `tool_label`
 * text from `decision.use_long_label`. The one span read left was in
 * `create_extruder_row()` — the text a *freshly created* row starts with.
 * `PanelWidgetManager` always calls `on_size_changed()` synchronously right
 * after `attach()` (panel_widget_manager.cpp:862-868), so a row built at
 * attach() is corrected before anything paints. But `rebuild_rows()` also
 * runs later, off `extruder_version_subject` (late tool discovery, a
 * reconnect) — and a real touchscreen never gets a second `on_size_changed`
 * after that (screens don't resize at runtime). A row built by that second
 * rebuild is what these tests target: the label it's created with is the
 * label it keeps.
 *
 * Each case resizes the widget with a colspan that *contradicts* the old
 * ">= 2" rule (colspan=1 with a wide grant; colspan=2 with a narrow one), so
 * a span-reading implementation fails here instead of passing by
 * coincidence — same technique as test_widget_size_fan_stack.cpp.
 *
 * The mock `--test` printer backend only ever exposes one extruder, so the
 * 2-row path below is unreachable by driving the real app; these tests
 * populate `ToolState` and `PrinterState` directly to reach it. See the
 * last test in this file for whether the widget's `decision.columns == 2`
 * branch is reachable at all under that constraint.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_state.h"
#include "printer_discovery.h"
#include "printer_state.h"
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

/// The nth row's declared width. Two-column layout sets lv_pct(48); the XML
/// template's own default is 100%, which is what an un-laid-out row keeps.
int32_t nth_row_width(lv_obj_t* container, int index) {
    lv_obj_t* row = lv_obj_get_child(container, index);
    REQUIRE(row != nullptr);
    return lv_obj_get_style_width(row, LV_PART_MAIN);
}

/// The nth extruder/bed row's tool_label, found by container child index
/// (rows share the "tool_label" name, so a container-wide find_by_name would
/// only ever return the first).
lv_obj_t* nth_row_tool_label(lv_obj_t* container, int index) {
    lv_obj_t* row = lv_obj_get_child(container, index);
    REQUIRE(row != nullptr);
    lv_obj_t* label = lv_obj_find_by_name(row, "tool_label");
    REQUIRE(label != nullptr);
    return label;
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

    // Sanity: the resize itself corrected the row that already existed —
    // proves the pixel decision really did come out "long" here.
    REQUIRE(std::string(lv_label_get_text(nth_row_tool_label(container, 0))) == "Nozzle");

    // Late tool discovery: a second extruder appears well after the widget
    // already knows it is wide. No further on_size_changed() call happens —
    // a real touchscreen never gets one after initial layout.
    add_second_extruder(state());
    REQUIRE(lv_obj_get_child_count(container) == 3); // 2 extruder rows + bed row

    // The freshly created second row must show the long form too: the
    // widget already knows it is wide. A colspan=1 reading implementation
    // shows "T1" (short) here instead.
    CHECK(std::string(lv_label_get_text(nth_row_tool_label(container, 1))) == "Nozzle 2");
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

    // colspan=2 (old rule: current_colspan_ >= 2 is true -> long label) but
    // a narrow grant, so decide_nozzle_layout() picks the short form.
    h.resize(2, 1, 100, 300);

    // Sanity: the resize corrected the existing row to short.
    REQUIRE(std::string(lv_label_get_text(nth_row_tool_label(container, 0))) == "Slot 1");

    add_second_extruder(state());
    REQUIRE(lv_obj_get_child_count(container) == 3);

    // The freshly created row must stay short: the widget already knows it
    // is narrow. A colspan=2 reading implementation shows "Nozzle 2" here.
    CHECK(std::string(lv_label_get_text(nth_row_tool_label(container, 1))) == "Slot 2");
}

/**
 * decide_nozzle_layout()'s columns == 2 branch requires row_count >= 2
 * (nozzle_layout.h:37), which the real `--test` mock printer can never
 * supply — it only ever exposes one extruder. This test proves the branch
 * is still reachable through the widget itself (as opposed to only through
 * decide_nozzle_layout()'s own pure-function tests) by driving ToolState to
 * two extruders the same way the tests above do, then resizing generously
 * enough for two short-label columns to fit.
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

    CHECK(lv_obj_get_style_flex_flow(container, LV_PART_MAIN) == LV_FLEX_FLOW_ROW_WRAP);
}

/**
 * The label decision already survives a late rebuild (the two cases above). The
 * COLUMN decision did not: on_size_changed() sets each row's width to lv_pct(48)
 * for a two-column layout, but a row created afterwards comes from the XML
 * template at 100% and spans the whole container, overlapping its neighbour.
 *
 * A real touchscreen never issues a second on_size_changed (screens do not
 * resize at runtime), so late tool discovery on a toolchanger is exactly when
 * this happens: the reporter's 2x2 tile "sometimes" expanded, depending on
 * whether the tools arrived before or after the grid announced the size
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
    REQUIRE(lv_obj_get_style_flex_flow(container, LV_PART_MAIN) == LV_FLEX_FLOW_ROW_WRAP);
    const int32_t half = lv_obj_get_style_width(lv_obj_get_child(container, 0), LV_PART_MAIN);
    REQUIRE(half == lv_pct(48)); // sanity: the two-column width really was applied

    // Late tool discovery. No second on_size_changed() follows, exactly as on a
    // real screen.
    add_third_extruder(state());
    REQUIRE(lv_obj_get_child_count(container) == 4); // 3 extruders + bed

    // Every row, including the one that did not exist when the size arrived,
    // must carry the two-column width. Without the replay the new row keeps the
    // template's 100% and overlaps.
    for (int i = 0; i < 4; i++) {
        INFO("row " << i << " width " << nth_row_width(container, i) << " want " << half);
        CHECK(nth_row_width(container, i) == half);
    }
    CHECK(lv_obj_get_style_flex_flow(container, LV_PART_MAIN) == LV_FLEX_FLOW_ROW_WRAP);
}
