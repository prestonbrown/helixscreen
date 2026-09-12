// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_tool_switcher.cpp
 * @brief tool_switcher picks compact vs. pill layout — and, within pills, the
 * single-column-vs-row shape — from physical pixels, not colspan/rowspan.
 *
 * Unlike widgets whose on_size_changed() reads spans once and returns, three
 * separate sites read the granted size: on_size_changed() itself picks
 * compact vs. pills; rebuild_pills() (called again later from two different
 * observers) picks the vertical-column shape from the same size; and
 * on_active_tool_changed() re-derives which rebuild function to call, again
 * from the same cached size. The last two never receive a size argument —
 * they only see whatever on_size_changed() cached last time it ran. Each
 * test below drives one of the three consumers and pairs its target pixels
 * with a colspan/rowspan the *old* span-based predicate would resolve
 * differently, so an implementation that still reads spans fails here
 * instead of passing by coincidence.
 *
 * Tool data is seeded directly on the ToolState singleton via
 * ToolTopology/set_ams_topology() (mirrors nozzle_temps's use of
 * PrinterDiscovery+init_tools) — the mock `--test` printer backend never
 * exposes more than one tool, so multi-pill scenarios are unreachable by
 * driving the real app.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/tool_switcher_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "panel_widget_size.h"
#include "printer_discovery.h"
#include "src/ui/panel_widgets/tool_switcher_widget.h"
#include "tool_state.h"

#include <cstdlib>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

namespace {

/// ToolState is a singleton outside LVGLUITestFixture's own init/deinit
/// chain, so a test that seeds it must clear it on the way out or later
/// test files in the same binary inherit stale tools (see
/// test_widget_size_nozzle_temps.cpp's NozzleTempsFixture, same trap).
struct ToolSwitcherFixture : public LVGLUITestFixture {
    ~ToolSwitcherFixture() override {
        ToolState::instance().deinit_subjects();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

/// Seeds ToolState with `count` generated tools ("T0".."Tn-1") and the given
/// active index via the AMS-topology path — the simplest way to get an
/// arbitrary tool count without going through PrinterDiscovery/JSON.
void configure_tools(int count, int active_index = 0) {
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    ToolTopology topo;
    topo.tool_count = count;
    topo.active_tool = active_index;
    ts.set_ams_topology(topo);
}

/// Re-applies the topology with a new tool_count/active_tool so a later
/// call only flips the ONE subject the test cares about (tool_count_ or
/// active_tool_), isolating which observer fires.
void update_tools(int count, int active_index) {
    ToolTopology topo;
    topo.tool_count = count;
    topo.active_tool = active_index;
    ToolState::instance().set_ams_topology(topo);
}

/// Mirrors tool_switcher_widget.cpp's (anonymous-namespace, unexported)
/// resolve_space_token() so the row-count thresholds below are derived from
/// whatever breakpoint tier is actually active in this test run, not a
/// hardcoded guess at "button_height_sm"/"space_xs"'s resolved pixel value.
int resolve_space_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

} // namespace

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: compact, pill-row and pill-column forms follow pixels, "
                 "not spans",
                 "[widget_size][tool_switcher]") {
    configure_tools(3);

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    REQUIRE(container != nullptr);

    // LVGL fires a freshly-added observer once immediately on subscribe
    // (lv_subject_add_observer_obj), so attach() — via observe_int_sync —
    // queues ONE deferred rebuild from each of tool_count_observer_ and
    // active_tool_observer_ before any resize() ever runs, using whatever
    // current_width_px_/current_height_px_ hold at that point (still the
    // 0,0 default — compact). Drain that now, while it is a no-op (compact
    // in, compact out). Left undrained, it would instead fire on the FIRST
    // process_lvgl() below — by which point on_size_changed() has already
    // cached the real resize() pixels — and silently re-derive the correct
    // layout from those, masking a broken on_size_changed() for exactly one
    // case: whichever resize happens to land first.
    process_lvgl(30);

    // --- Compact: both axes below floor. Contradicting span: 2x2 (old rule:
    // colspan==1 && rowspan==1 is false -> pills).
    h.resize(2, 2, w_normal() - 1, h_tall() - 1);
    process_lvgl(30);

    CHECK(lv_obj_get_child_count(container) == 2); // icon + label
    CHECK(lv_obj_has_flag(container, LV_OBJ_FLAG_CLICKABLE));
    CHECK(lv_obj_get_style_flex_flow(container, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);

    // --- Pill row: width at/over the floor. Height is deliberately short
    // (well under two pill rows' worth of content height) so rebuild_pills()'s
    // own row-count measurement — a real-geometry heuristic independent of
    // the colspan/rowspan migration this test targets — keeps a single flex
    // row instead of switching to its 2-row grid. Contradicting span: 1x1
    // (old rule -> compact).
    h.resize(1, 1, w_normal(), 60);
    process_lvgl(30);

    REQUIRE(lv_obj_get_child_count(container) == 3); // one ui_button per tool
    CHECK(lv_obj_get_style_flex_flow(container, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);

    // --- Pill column: narrow but tall — the legacy 1x2 vertical-stack shape
    // (tool_switcher_widget.cpp's rebuild_pills(), the current_rowspan_>=2
    // branch). Contradicting span: 1x1 (old rule -> compact, not even pills).
    h.resize(1, 2, w_normal() - 1, h_tall());
    process_lvgl(30);

    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(lv_obj_get_style_flex_flow(container, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: on_active_tool_changed rebuilds in pill form using the "
                 "cached size, not a fresh span",
                 "[widget_size][tool_switcher]") {
    configure_tools(3, /*active_index=*/0);

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    REQUIRE(container != nullptr);

    // Drain attach()'s immediate-on-subscribe observer notifications (see
    // the longer comment in the compact/pill-row/pill-column test above)
    // before the first resize(), so it can't fire on a later process_lvgl()
    // and mask a broken on_size_changed().
    process_lvgl(30);

    // Grant wide pixels (pills, either row or grid sub-shape — that choice
    // is a separate real-geometry heuristic this test doesn't target).
    // on_active_tool_changed() never receives a size — it only ever sees
    // whatever this resize cached.
    h.resize(1, 1, w_wide(), h_tall() - 1);
    process_lvgl(30);
    REQUIRE(lv_obj_get_child_count(container) == 3);

    // T0 active: pill 0 opaque (ButtonPrimary), pill 1 transparent (ButtonGhost).
    CHECK(lv_obj_get_style_bg_opa(lv_obj_get_child(container, 0), LV_PART_MAIN) == LV_OPA_COVER);
    CHECK(lv_obj_get_style_bg_opa(lv_obj_get_child(container, 1), LV_PART_MAIN) == LV_OPA_0);

    // Change the active tool WITHOUT any further on_size_changed() call —
    // exactly what a real touchscreen does (screens don't resize at
    // runtime). Only active_tool_ changes; tool_count_ stays at 3.
    update_tools(3, /*active_index=*/1);
    process_lvgl(30);

    // Still pill form (3 buttons) — on_active_tool_changed read the cached
    // wide/short size and picked rebuild_pills(), not rebuild_compact(). A
    // stale-span implementation (colspan/rowspan defaulting to 1x1) would
    // instead collapse this to the 2-child compact form.
    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(lv_obj_get_style_bg_opa(lv_obj_get_child(container, 0), LV_PART_MAIN) == LV_OPA_0);
    CHECK(lv_obj_get_style_bg_opa(lv_obj_get_child(container, 1), LV_PART_MAIN) == LV_OPA_COVER);
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: tool_count observer rebuilds in pill form using the "
                 "cached size, not a fresh span",
                 "[widget_size][tool_switcher]") {
    configure_tools(1);

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    REQUIRE(container != nullptr);

    // Drain attach()'s immediate-on-subscribe observer notifications (see
    // the longer comment in the compact/pill-row/pill-column test above)
    // before the first resize().
    process_lvgl(30);

    // Grant pill-row pixels with a single tool present.
    h.resize(1, 1, w_wide(), h_tall() - 1);
    process_lvgl(30);
    REQUIRE(lv_obj_get_child_count(container) == 1);

    // Tool count jumps 1 -> 3 with no further on_size_changed() call. The
    // tool_count_ observer (tool_switcher_widget.cpp:66-76) fires and must
    // read the cached wide/short size to pick rebuild_pills(). A
    // stale-span implementation defaults to rebuild_compact(), which would
    // produce 2 children (icon + label) regardless of tool count — a
    // different, distinguishable number from the 3 pills expected here.
    update_tools(3, /*active_index=*/0);
    process_lvgl(30);

    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(lv_obj_get_style_flex_flow(container, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
    CHECK(lv_obj_get_style_bg_opa(lv_obj_get_child(container, 0), LV_PART_MAIN) == LV_OPA_COVER);
    CHECK(lv_obj_get_style_bg_opa(lv_obj_get_child(container, 1), LV_PART_MAIN) == LV_OPA_0);
    CHECK(lv_obj_get_style_bg_opa(lv_obj_get_child(container, 2), LV_PART_MAIN) == LV_OPA_0);
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: pre-grid oversized self-measurement is corrected once the "
                 "real grid cell settles",
                 "[widget_size][tool_switcher]") {
    // PanelWidgetManager calls on_size_changed() BEFORE activating the grid
    // layout (panel_widget_manager.cpp:901-903, deliberately — activating
    // early crashed, #983). Until the grid activates, widget_obj_'s XML
    // 100%/100% sizing resolves against the outer panel's whole content box,
    // not its eventual grid cell — so the FIRST time rebuild_pills()
    // self-measures tool_switcher_container's height to pick a row count, it
    // reads that oversized box, not the real cell.
    configure_tools(3);

    int pill_min_h = resolve_space_token("button_height_sm", 40);
    int row_gap = resolve_space_token("space_xs", 4);

    // Pre-grid: tall enough that fit_rows saturates well past rebuild_pills()'s
    // 2-row cap, regardless of which breakpoint tier resolved the tokens above.
    int pregrid_h = 20 * (pill_min_h + row_gap);
    // Real cell: exactly enough height for one pill row (fit_rows == 1
    // exactly) — the real cell that only fits a single row, per the bug
    // report's "590px measured, 66px real" repro.
    int real_h = pill_min_h;
    int real_w = w_wide();

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    REQUIRE(container != nullptr);

    // Drain attach()'s immediate-on-subscribe observer notifications (see the
    // longer comment in the compact/pill-row/pill-column test above).
    process_lvgl(30);

    // Step 1: pre-grid state — widget_obj_ (h.root()) still reports the
    // oversized whole-content-box size. Deliberately NOT using h.resize()
    // here: it keeps widget_obj_'s actual size and on_size_changed()'s
    // arguments in lockstep, which is exactly what production does NOT do
    // pre-grid.
    lv_obj_set_size(h.root(), real_w, pregrid_h);
    lv_obj_update_layout(h.root());

    // Step 2: on_size_changed() receives the CORRECT target cell pixels (as
    // PanelWidgetManager's grid_track_extent() computes them in production)
    // even though widget_obj_'s on-screen size hasn't caught up to them yet.
    h.widget().on_size_changed(1, 1, real_w, real_h);
    process_lvgl(30);

    // Sanity check on the reproduction itself: rebuild_pills() self-measured
    // the still-oversized container and baked the 2-row grid — this holds
    // both before AND after the fix, since the fix doesn't change what
    // happens here, only what happens once the grid actually settles below.
    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(lv_obj_get_style_layout(container, LV_PART_MAIN) == LV_LAYOUT_GRID);

    // Step 3: grid activation — widget_obj_ actually shrinks to its real
    // cell size, firing LV_EVENT_SIZE_CHANGED. Before the fix, nothing
    // listens for this and the 2-row grid from step 2 is never revisited.
    lv_obj_set_size(h.root(), real_w, real_h);
    lv_obj_update_layout(h.root());
    process_lvgl(30);

    // The real cell only fits one pill row (real_h == pill_min_h exactly),
    // so the layout must have collapsed to the single flex row — not stayed
    // on the 2-row grid baked from the oversized pre-grid box.
    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(lv_obj_get_style_layout(container, LV_PART_MAIN) == LV_LAYOUT_FLEX);
    CHECK(lv_obj_get_style_flex_flow(container, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
}

TEST_CASE_METHOD(ToolSwitcherFixture, "tool_switcher: compact mode marks an unknown active tool",
                 "[widget_size][tool_switcher]") {
    // Klipper's toolchanger.tool_number is taken as reported, so a printer that
    // names a tool the discovery never listed leaves active_tool_index() past
    // the end of the tool list.
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    helix::PrinterDiscovery disc;
    nlohmann::json objects = {"toolchanger", "tool T0",   "tool T1",  "tool T2",
                              "extruder",    "extruder1", "extruder2"};
    disc.parse_objects(objects);
    ts.init_tools(disc);
    REQUIRE(ts.tool_count() == 3);

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    REQUIRE(h.child("tool_switcher_container") != nullptr);
    process_lvgl(30);

    // Both axes below the floor: the compact icon-plus-label form.
    h.resize(2, 2, w_normal() - 1, h_tall() - 1);
    process_lvgl(30);

    lv_obj_t* label = ToolSwitcherTestAccess::compact_label(h.widget());
    REQUIRE(label != nullptr);
    REQUIRE(std::string(lv_label_get_text(label)) == ts.tools()[0].display_label);

    ts.update_from_status(nlohmann::json{{"toolchanger", {{"tool_number", 7}}}});
    process_lvgl(30);
    REQUIRE(ts.active_tool_index() == 7);

    label = ToolSwitcherTestAccess::compact_label(h.widget());
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_label_get_text(label)) == "?");
}
