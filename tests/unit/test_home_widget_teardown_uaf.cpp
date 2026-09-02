// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_home_widget_teardown_uaf.cpp
 * @brief Home-panel widgets must not touch their tile trees after those trees are deleted
 *
 * The home-panel C++ widgets (NozzleTempsWidget, ToolSwitcherWidget,
 * ThermistorWidget) cache raw child pointers — row labels, pill buttons,
 * carousel page labels — guarded only by their AsyncLifetimeGuard tokens,
 * which expire in detach(). detach() runs on every CURRENT deletion path
 * before the tree is condemned, but a raw lv_obj_delete() of the home page
 * container (screen teardown) gives the widget no call at all: the tokens
 * stay live, the pointers dangle, and the next UpdateQueue drain runs
 * queued observer applies against freed widgets.
 *
 * Field shape: bundle 7F94FHC8 (v0.99.117, Snapmaker U1, four toolheads +
 * AFC) aborted with "malloc(): invalid size (unsorted)" — a write into
 * freed heap bracketed by two audits to the home-panel widget fan-out.
 *
 * Same hole PowerPanel and PrintStatusPanel had (#776 family), closed with
 * an LV_EVENT_DELETE hook on the tree root that drops the cached pointers
 * and expires the guard. These tests pin that contract per widget family:
 * populate, delete the page tree, then drain.
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/nozzle_temps_test_access.h"
#include "../test_helpers/thermistor_test_access.h"
#include "app_globals.h"
#include "panel_widget_manager.h"
#if HELIX_HAS_CAMERA
#include "../test_helpers/camera_widget_test_access.h"
#include "src/ui/panel_widgets/camera_widget.h"
#endif
#include "../test_helpers/tool_switcher_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "print_lifecycle_state.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/nozzle_temps_widget.h"
#include "src/ui/panel_widgets/thermistor_widget.h"
#include "src/ui/panel_widgets/tool_switcher_widget.h"
#include "temperature_sensor_manager.h"
#include "tool_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::UpdateQueue;

namespace {

constexpr const char* THERMISTOR_SENSOR_A = "temperature_sensor helix_teardown_a";
constexpr const char* THERMISTOR_SENSOR_B = "temperature_sensor helix_teardown_b";

/// LVGLUITestFixture does not own ToolState's subjects, and the tests below
/// seed it (tool_switcher needs tools for pills; nozzle_temps needs the
/// extruder mapping). Clear it on the way out or later test files in the same
/// binary inherit stale tools (same trap as test_widget_size_tool_switcher.cpp).
struct HomeWidgetTeardownFixture : LVGLUITestFixture {
    ~HomeWidgetTeardownFixture() override {
        ToolState::instance().deinit_subjects();
        helix::ui::UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
    }

    /// A stand-in for the home panel's widget-page container. Raw-deleting it
    /// is how a screen teardown frees the tiles: no detach(), no panel call,
    /// only LVGL's own delete events.
    lv_obj_t* make_page() {
        lv_obj_t* page = lv_obj_create(test_screen());
        lv_obj_set_size(page, 800, 600);
        return page;
    }

    /// The widget's real XML tile, the object PanelWidgetManager hands to
    /// attach() in production.
    lv_obj_t* make_tile(lv_obj_t* parent, const char* component) {
        auto* tile = static_cast<lv_obj_t*>(lv_xml_create(parent, component, nullptr));
        REQUIRE(tile != nullptr);
        return tile;
    }
};

/// Is the widget's delete hook still installed on @p obj for @p widget?
///
/// Reaches into LVGL's event list rather than inferring from behaviour: the
/// point is that the hook must be gone *before* anything can fire it, and a
/// behavioural probe would have to trigger the very use-after-free under test.
/// Same probe as test_print_status_teardown_uaf.cpp.
bool delete_hook_installed(lv_obj_t* obj, const void* widget) {
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
        if (dsc != nullptr && lv_event_dsc_get_user_data(dsc) == widget) {
            return true;
        }
    }
    return false;
}

/// One extruder ("T0" -> "extruder") plus the bed — the real mock printer's
/// topology (same seeding as test_widget_size_nozzle_temps.cpp).
void configure_one_extruder(PrinterState& state) {
    state.init_extruders({"extruder"});

    ToolState::instance().deinit_subjects();
    ToolState::instance().init_subjects(false);
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"extruder", "heater_bed", "gcode_move"}));
    ToolState::instance().init_tools(hw);
}

/// n tools ("T0".."Tn-1") via the AMS-topology path — the simplest way to get
/// an arbitrary tool count without going through PrinterDiscovery/JSON (same
/// seeding as test_widget_size_tool_switcher.cpp).
void configure_tools(int count, int active_index = 0) {
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    ToolTopology topo;
    topo.tool_count = count;
    topo.active_tool = active_index;
    topo.tool_name_prefix = "T";
    ts.set_ams_topology(topo);
}

} // namespace

// ============================================================================
// NozzleTempsWidget
// ============================================================================

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "nozzle_temps drops cached row and bed pointers when the page tree is deleted raw",
                 "[nozzle_temps][teardown][uaf]") {
    configure_one_extruder(state());

    auto widget = std::make_unique<NozzleTempsWidget>(state());
    lv_obj_t* page = make_page();
    widget->attach(make_tile(page, "panel_widget_nozzle_temps"), test_screen());
    UpdateQueue::instance().drain();

    // The tree must have actually populated the pointers under test, or the
    // null checks below would pass for the wrong reason.
    REQUIRE(NozzleTempsTestAccess::row_count(*widget) == 1);
    REQUIRE(NozzleTempsTestAccess::row_temp_label(*widget, 0) != nullptr);
    REQUIRE(NozzleTempsTestAccess::row_target_label(*widget, 0) != nullptr);
    REQUIRE(NozzleTempsTestAccess::bed_temp_label(*widget) != nullptr);

    lv_obj_delete(page);

    CHECK(NozzleTempsTestAccess::row_temp_label(*widget, 0) == nullptr);
    CHECK(NozzleTempsTestAccess::row_target_label(*widget, 0) == nullptr);
    CHECK(NozzleTempsTestAccess::bed_temp_label(*widget) == nullptr);
    CHECK(NozzleTempsTestAccess::bed_icon(*widget) == nullptr);
}

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "nozzle_temps queued extruder-temp apply survives a drain after the tree dies",
                 "[nozzle_temps][teardown][uaf]") {
    configure_one_extruder(state());

    auto widget = std::make_unique<NozzleTempsWidget>(state());
    lv_obj_t* page = make_page();
    widget->attach(make_tile(page, "panel_widget_nozzle_temps"), test_screen());
    UpdateQueue::instance().drain();
    REQUIRE(NozzleTempsTestAccess::row_temp_label(*widget, 0) != nullptr);

    // An extruder-temp change fires the row's observe_int_sync observer
    // synchronously; the handler body is queued. This is the pending lambda a
    // screen teardown can drain after the tree is already gone.
    lv_subject_t* temp = state().get_extruder_temp_subject("extruder");
    REQUIRE(temp != nullptr);
    lv_subject_set_int(temp, 2450);

    // Tree dies while the handler is still sitting in the queue. Pre-fix the
    // row token never expired, so update_row_display() ran
    // lv_label_set_text_fmt() on the freed row labels.
    lv_obj_delete(page);
    UpdateQueue::instance().drain();

    CHECK(NozzleTempsTestAccess::row_temp_label(*widget, 0) == nullptr);
}

// PanelWidget instances can be destroyed while their tile tree is still alive
// (the manager drops non-reused instances; app shutdown destroys panels before
// lv_deinit()). If the destructor leaves its LV_EVENT_DELETE hook installed,
// that teardown calls back into freed memory.
TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "nozzle_temps uninstalls its delete hook when destroyed before its tree",
                 "[nozzle_temps][teardown][uaf]") {
    configure_one_extruder(state());

    auto widget = std::make_unique<NozzleTempsWidget>(state());
    lv_obj_t* page = make_page();
    lv_obj_t* tile = make_tile(page, "panel_widget_nozzle_temps");
    widget->attach(tile, test_screen());
    UpdateQueue::instance().drain();

    // Guards against the test passing for the wrong reason: if attach() never
    // installed the hook, its absence after destruction would prove nothing.
    REQUIRE(delete_hook_installed(tile, widget.get()));

    const void* dead = widget.get();
    widget.reset();
    CHECK_FALSE(delete_hook_installed(tile, dead));

    // The tree outlives the widget exactly as it does under lv_deinit(). With
    // the hook still installed this dereferences freed memory.
    lv_obj_delete(tile);
    UpdateQueue::instance().drain();
    SUCCEED("tile torn down after the widget without touching freed memory");
}

// PanelWidgetManager recycles instances across rebuilds: attach() reuses the
// C++ object for a fresh tile while the replaced tile is condemned. That late
// delete must not blank the successor's cached pointers.
TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "nozzle_temps ignores a replaced root's late delete event",
                 "[nozzle_temps][teardown][uaf]") {
    configure_one_extruder(state());

    auto widget = std::make_unique<NozzleTempsWidget>(state());
    lv_obj_t* page = make_page();
    lv_obj_t* old_tile = make_tile(page, "panel_widget_nozzle_temps");
    widget->attach(old_tile, test_screen());
    UpdateQueue::instance().drain();
    REQUIRE(NozzleTempsTestAccess::row_temp_label(*widget, 0) != nullptr);

    widget->attach(make_tile(page, "panel_widget_nozzle_temps"), test_screen());
    UpdateQueue::instance().drain();
    REQUIRE(NozzleTempsTestAccess::row_temp_label(*widget, 0) != nullptr);

    lv_obj_delete(old_tile);
    UpdateQueue::instance().drain();

    CHECK(NozzleTempsTestAccess::row_temp_label(*widget, 0) != nullptr);
    CHECK(NozzleTempsTestAccess::bed_temp_label(*widget) != nullptr);
}

// ============================================================================
// ToolSwitcherWidget
// ============================================================================

namespace {

/// Attach a ToolSwitcherWidget to its real XML tile on a page container and
/// build pills. detach() is NOT called: the page is raw-deleted underneath a
/// live, attached widget, the shape detach() never covers.
struct AttachedToolSwitcher {
    HomeWidgetTeardownFixture& fixture;
    std::unique_ptr<ToolSwitcherWidget> widget =
        std::make_unique<ToolSwitcherWidget>(fixture.state());
    lv_obj_t* page = fixture.make_page();
    lv_obj_t* tile = nullptr;

    explicit AttachedToolSwitcher(HomeWidgetTeardownFixture& f) : fixture(f) {
        tile = fixture.make_tile(page, "panel_widget_tool_switcher");
        widget->attach(tile, fixture.test_screen());
        UpdateQueue::instance().drain();

        // Pill mode: wide and tall enough that neither compact nor the narrow
        // tall column applies. The tile's own size must settle first —
        // rebuild_pills() measures the container (see tests/CLAUDE.md's LVGL
        // traps section).
        lv_obj_set_size(tile, 400, 400);
        lv_obj_update_layout(tile);
        widget->on_size_changed(2, 2, 400, 400);
        UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "tool_switcher drops cached pills when the page tree is deleted raw",
                 "[tool_switcher][teardown][uaf]") {
    configure_tools(3);
    AttachedToolSwitcher attached(*this);

    std::vector<lv_obj_t*> pills = ToolSwitcherTestAccess::pills(*attached.widget);
    REQUIRE(pills.size() == 3); // populated, or the empty check proves nothing

    lv_obj_delete(attached.page);

    CHECK(ToolSwitcherTestAccess::pills(*attached.widget).empty());
    CHECK(ToolSwitcherTestAccess::compact_label(*attached.widget) == nullptr);
}

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "tool_switcher queued print-gating apply survives a drain after the tree dies",
                 "[tool_switcher][teardown][uaf]") {
    configure_tools(3);
    AttachedToolSwitcher attached(*this);
    REQUIRE(ToolSwitcherTestAccess::pills(*attached.widget).size() == 3);

    // A print-lifecycle transition fires print_state_observer_; its handler
    // (refresh_print_gating over pill_buttons_) is queued.
    lv_subject_t* lifecycle = state().get_print_lifecycle_subject();
    REQUIRE(lifecycle != nullptr);
    lv_subject_set_int(lifecycle, static_cast<int>(PrintState::Printing));

    // Tree dies while the handler is still queued. Pre-fix the token never
    // expired and pill_buttons_ still held freed pills, so refresh_print_gating()
    // ran lv_obj_add_state()/lv_obj_remove_state() over them.
    lv_obj_delete(attached.page);
    UpdateQueue::instance().drain();

    CHECK(ToolSwitcherTestAccess::pills(*attached.widget).empty());
}

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "tool_switcher uninstalls its delete hook when destroyed before its tree",
                 "[tool_switcher][teardown][uaf]") {
    configure_tools(3);
    AttachedToolSwitcher attached(*this);

    REQUIRE(delete_hook_installed(attached.tile, attached.widget.get()));

    const void* dead = attached.widget.get();
    attached.widget.reset();
    CHECK_FALSE(delete_hook_installed(attached.tile, dead));

    lv_obj_delete(attached.tile);
    UpdateQueue::instance().drain();
    SUCCEED("tile torn down after the widget without touching freed memory");
}

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "tool_switcher ignores a replaced root's late delete event",
                 "[tool_switcher][teardown][uaf]") {
    configure_tools(3);
    AttachedToolSwitcher attached(*this);
    REQUIRE(ToolSwitcherTestAccess::pills(*attached.widget).size() == 3);

    // Recycle: attach() the same instance onto a fresh tile; the old tile's
    // children were condemned by the rebuild's safe_clean_children.
    lv_obj_t* new_tile = make_tile(attached.page, "panel_widget_tool_switcher");
    attached.widget->attach(new_tile, test_screen());
    UpdateQueue::instance().drain();
    lv_obj_set_size(new_tile, 400, 400);
    lv_obj_update_layout(new_tile);
    attached.widget->on_size_changed(2, 2, 400, 400);
    UpdateQueue::instance().drain();
    REQUIRE(ToolSwitcherTestAccess::pills(*attached.widget).size() == 3);

    lv_obj_delete(attached.tile);
    UpdateQueue::instance().drain();

    CHECK(ToolSwitcherTestAccess::pills(*attached.widget).size() == 3);
}

// ============================================================================
// ThermistorWidget
// ============================================================================

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "thermistor drops cached labels when the page tree is deleted raw",
                 "[thermistor][teardown][uaf]") {
    helix::sensors::TemperatureSensorManager::instance().discover({THERMISTOR_SENSOR_A});

    auto widget = std::make_unique<ThermistorWidget>("thermistor");
    widget->set_config({{"sensors", std::vector<std::string>{THERMISTOR_SENSOR_A}}});
    lv_obj_t* page = make_page();
    widget->attach(make_tile(page, "panel_widget_thermistor"), test_screen());
    UpdateQueue::instance().drain();

    REQUIRE(ThermistorTestAccess::temp_label(*widget) != nullptr);
    REQUIRE(ThermistorTestAccess::name_label(*widget) != nullptr);

    lv_obj_delete(page);

    CHECK(ThermistorTestAccess::temp_label(*widget) == nullptr);
    CHECK(ThermistorTestAccess::name_label(*widget) == nullptr);
}

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "thermistor queued temp apply survives a drain after the tree dies",
                 "[thermistor][teardown][uaf]") {
    helix::sensors::TemperatureSensorManager::instance().discover({THERMISTOR_SENSOR_A});

    auto widget = std::make_unique<ThermistorWidget>("thermistor");
    widget->set_config({{"sensors", std::vector<std::string>{THERMISTOR_SENSOR_A}}});
    lv_obj_t* page = make_page();
    widget->attach(make_tile(page, "panel_widget_thermistor"), test_screen());
    UpdateQueue::instance().drain();
    REQUIRE(ThermistorTestAccess::temp_label(*widget) != nullptr);

    // The sensor's temp subject fires temp_observer_ synchronously; the handler
    // (on_temp_changed -> lv_label_set_text(temp_label_)) is queued.
    lv_subject_t* subject =
        helix::sensors::TemperatureSensorManager::instance().get_temp_subject(THERMISTOR_SENSOR_A);
    REQUIRE(subject != nullptr);
    lv_subject_set_int(subject, 425);

    // Tree dies while the handler is still queued. Pre-fix temp_label_ still
    // pointed into the freed tree, so on_temp_changed() wrote to freed memory.
    lv_obj_delete(page);
    UpdateQueue::instance().drain();

    CHECK(ThermistorTestAccess::temp_label(*widget) == nullptr);
}

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "thermistor uninstalls its delete hook when destroyed before its tree",
                 "[thermistor][teardown][uaf]") {
    helix::sensors::TemperatureSensorManager::instance().discover({THERMISTOR_SENSOR_A});

    auto widget = std::make_unique<ThermistorWidget>("thermistor");
    widget->set_config({{"sensors", std::vector<std::string>{THERMISTOR_SENSOR_A}}});
    lv_obj_t* page = make_page();
    lv_obj_t* tile = make_tile(page, "panel_widget_thermistor");
    widget->attach(tile, test_screen());
    UpdateQueue::instance().drain();

    REQUIRE(delete_hook_installed(tile, widget.get()));

    const void* dead = widget.get();
    widget.reset();
    CHECK_FALSE(delete_hook_installed(tile, dead));

    lv_obj_delete(tile);
    UpdateQueue::instance().drain();
    SUCCEED("tile torn down after the widget without touching freed memory");
}

// Carousel mode caches per-page label pointers (carousel_pages_). The pages
// are reparented into the carousel's scroll container by
// ui_carousel_add_item(), so they die with the tile tree like every other
// cached pointer.
TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "thermistor drops carousel page labels when the page tree is deleted raw",
                 "[thermistor][teardown][uaf][carousel]") {
    helix::sensors::TemperatureSensorManager::instance().discover(
        {THERMISTOR_SENSOR_A, THERMISTOR_SENSOR_B});

    auto widget = std::make_unique<ThermistorWidget>("thermistor");
    widget->set_config(
        {{"sensors", std::vector<std::string>{THERMISTOR_SENSOR_A, THERMISTOR_SENSOR_B}},
         {"display_mode", "carousel"}});
    lv_obj_t* page = make_page();
    widget->attach(make_tile(page, "panel_widget_thermistor_carousel"), test_screen());
    UpdateQueue::instance().drain();

    REQUIRE(ThermistorTestAccess::carousel_page_count(*widget) == 2);
    REQUIRE(ThermistorTestAccess::carousel_temp_label(*widget, 0) != nullptr);
    REQUIRE(ThermistorTestAccess::carousel_name_label(*widget, 1) != nullptr);

    lv_obj_delete(page);

    CHECK(ThermistorTestAccess::carousel_temp_label(*widget, 0) == nullptr);
    CHECK(ThermistorTestAccess::carousel_temp_label(*widget, 1) == nullptr);
    CHECK(ThermistorTestAccess::carousel_name_label(*widget, 0) == nullptr);
    CHECK(ThermistorTestAccess::carousel_name_label(*widget, 1) == nullptr);
}

// ============================================================================
// Rebuild early-returns must drop the cached pill/label containers first
// ============================================================================

// rebuild_pills() early-returns on "container not found" BEFORE clearing
// pill_buttons_. If the lookup fails while widget_obj_ is still non-null, the
// list keeps pointers to widgets a previous rebuild already condemned, and
// refresh_print_gating() runs unchecked lv_obj_add_state()/lv_obj_remove_state()
// over them — same freed-heap write the field crash bracketed, reached without
// a tree deletion at all.
TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "tool_switcher rebuild_pills with an unresolvable container leaves pill_buttons_ "
                 "empty",
                 "[tool_switcher][teardown]") {
    configure_tools(3);
    AttachedToolSwitcher attached(*this);
    REQUIRE(ToolSwitcherTestAccess::pills(*attached.widget).size() == 3);

    // Break the container lookup while widget_obj_ stays non-null.
    lv_obj_t* container = lv_obj_find_by_name(attached.tile, "tool_switcher_container");
    REQUIRE(container != nullptr);
    lv_obj_delete(container);

    attached.widget->on_size_changed(2, 2, 400, 400); // -> rebuild_pills()

    CHECK(ToolSwitcherTestAccess::pills(*attached.widget).empty());
}

// rebuild_compact() has the same early return and left compact_label_ pointing
// at the previous build's label, which refresh_print_gating() then restyles.
TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "tool_switcher rebuild_compact with an unresolvable container drops the compact "
                 "label",
                 "[tool_switcher][teardown]") {
    configure_tools(3);
    auto widget = std::make_unique<ToolSwitcherWidget>(state());
    lv_obj_t* page = make_page();
    lv_obj_t* tile = make_tile(page, "panel_widget_tool_switcher");
    widget->attach(tile, test_screen());
    UpdateQueue::instance().drain();

    // Compact mode (both axes under the W_NORMAL/H_TALL floors) caches the
    // label rebuild_compact() goes on to restyle.
    lv_obj_set_size(tile, 100, 100);
    lv_obj_update_layout(tile);
    widget->on_size_changed(1, 1, 100, 100);
    UpdateQueue::instance().drain();
    REQUIRE(ToolSwitcherTestAccess::compact_label(*widget) != nullptr);

    lv_obj_t* container = lv_obj_find_by_name(tile, "tool_switcher_container");
    REQUIRE(container != nullptr);
    lv_obj_delete(container);

    widget->on_size_changed(1, 1, 100, 100); // -> rebuild_compact()

    CHECK(ToolSwitcherTestAccess::compact_label(*widget) == nullptr);
    CHECK(ToolSwitcherTestAccess::pills(*widget).empty());
}

// ============================================================================
// Grid descriptors must not outlive the container that reads them
// ============================================================================

// lv_obj_set_grid_dsc_array() stores the descriptor pointers WITHOUT copying,
// so the container keeps reading grid_col_dsc_/grid_row_dsc_ out of the widget.
// rebuild_pills() .assign()s those vectors, freeing the old buffer — which a
// CONDEMNED container from a previous attach (reparented to lv_layer_top by
// safe_clean_children, deleted async) still holds in LV_LAYOUT_GRID. The
// in-rebuild LV_LAYOUT_NONE mitigation does not cover that cross-attach window;
// stripping the layout in detach() — which every reachable condemnation
// precedes — makes a condemned container structurally unable to read the
// descriptors again (the #983 mitigation PanelWidgetManager applies to the page
// container).
TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "tool_switcher detaches its container from the grid before it is condemned",
                 "[tool_switcher][teardown]") {
    configure_tools(3);
    AttachedToolSwitcher attached(*this);

    lv_obj_t* container = lv_obj_find_by_name(attached.tile, "tool_switcher_container");
    REQUIRE(container != nullptr);
    // Two pill rows for three tools at 400px: the rebuild must have left the
    // container in LV_LAYOUT_GRID holding the widget's descriptor buffers, or
    // the detach check below would pass for the wrong reason.
    REQUIRE(lv_obj_get_style_layout(container, LV_PART_MAIN) == LV_LAYOUT_GRID);

    attached.widget->detach();

    CHECK(lv_obj_get_style_layout(container, LV_PART_MAIN) == LV_LAYOUT_NONE);
}

namespace {

/// Layout the probed container was in when LVGL got round to deleting it — the
/// last moment the grid descriptors could still be read out of the widget.
uint16_t g_layout_at_container_delete = 0xFFFF;

void record_layout_at_delete(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    g_layout_at_container_delete = lv_obj_get_style_layout(obj, LV_PART_MAIN);
}

} // namespace

// The strip cannot live in detach() alone. detach() precedes every HomePanel
// condemnation today (ui_panel_home.cpp:83, 367, 461), but that is a property of
// the callers, not of the widget: PanelWidgetManager::populate_page() reaches
// safe_clean_children() with no detach of its own, and the raw-delete path
// (on_hooked_root_deleted -> forget_tile_widgets) nulls size_watch_container_
// without touching the layout, leaving a condemned container in LV_LAYOUT_GRID
// still pointed at the widget's descriptor buffers. Stripping it where the
// pointer is dropped covers both paths and removes the reasoning dependency.
TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "tool_switcher detaches its container from the grid on the raw-delete path too",
                 "[tool_switcher][teardown][uaf]") {
    configure_tools(3);
    AttachedToolSwitcher attached(*this);

    lv_obj_t* container = lv_obj_find_by_name(attached.tile, "tool_switcher_container");
    REQUIRE(container != nullptr);
    REQUIRE(lv_obj_get_style_layout(container, LV_PART_MAIN) == LV_LAYOUT_GRID);

    // The widget's hook is on the tile, and LVGL sends a subtree's LV_EVENT_DELETE
    // top-down before recursing into children — so a probe on the container
    // itself reads it after the widget has had its chance to strip the layout,
    // and while the container is still allocated.
    g_layout_at_container_delete = 0xFFFF;
    lv_obj_add_event_cb(container, record_layout_at_delete, LV_EVENT_DELETE, nullptr);

    // Raw page delete: no detach(), the widget hears only on_hooked_root_deleted().
    lv_obj_delete(attached.page);

    REQUIRE(g_layout_at_container_delete != 0xFFFF); // the probe must have run
    CHECK(g_layout_at_container_delete == LV_LAYOUT_NONE);
}

// ============================================================================
// CameraWidget
// ============================================================================
//
// The camera is the fourth widget with these mechanics and the only one that
// deliberately opts out of expiring its guard on detach: the MJPEG stream is
// meant to keep running across a detach->reattach gap, and the frame callbacks
// stay safe purely because camera_image_ is null in between. That makes the
// pointer drop load-bearing rather than defensive - a raw lv_obj_delete() of
// the page tree calls no detach(), so pre-fix the stream's next frame (10-30
// per second) wrote lv_image_set_src() into a freed lv_image.

#if HELIX_HAS_CAMERA

namespace {

/// Attach a CameraWidget to its real XML tile on a page container, with no
/// webcam configured so nothing ever opens a socket.
struct AttachedCamera {
    HomeWidgetTeardownFixture& fixture;
    std::unique_ptr<helix::CameraWidget> widget = std::make_unique<helix::CameraWidget>();
    lv_obj_t* page = nullptr;
    lv_obj_t* tile = nullptr;

    explicit AttachedCamera(HomeWidgetTeardownFixture& f) : fixture(f) {
        PanelWidgetManager::instance().init_widget_subjects();
        page = fixture.make_page();
        tile = fixture.make_tile(page, "panel_widget_camera");
        widget->attach(tile, fixture.test_screen());
        UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "camera drops its cached tile pointers when the page tree is deleted raw",
                 "[camera][teardown][uaf]") {
    AttachedCamera attached(*this);

    // The tile must have actually populated the pointers under test, or the
    // null checks below would pass for the wrong reason.
    REQUIRE(helix::CameraWidgetTestAccess::camera_image(*attached.widget) != nullptr);
    REQUIRE(helix::CameraWidgetTestAccess::camera_overlay(*attached.widget) != nullptr);
    REQUIRE(helix::CameraWidgetTestAccess::camera_status(*attached.widget) != nullptr);

    // No detach(): the widget hears only on_hooked_root_deleted().
    lv_obj_delete(attached.page);

    CHECK(helix::CameraWidgetTestAccess::camera_image(*attached.widget) == nullptr);
    CHECK(helix::CameraWidgetTestAccess::camera_overlay(*attached.widget) == nullptr);
    CHECK(helix::CameraWidgetTestAccess::camera_status(*attached.widget) == nullptr);
    CHECK(helix::CameraWidgetTestAccess::widget_obj(*attached.widget) == nullptr);
}

// The camera's divergence from the other three home widgets, pinned so nobody
// "completes" the fix by copying their lifetime_.invalidate() in here. A token
// the running CameraStream captured at start_stream() compares its snapshot
// against the guard's generation counter, so an invalidate() drops every
// subsequent frame for the life of that stream - and the fullscreen overlay is
// a child of the screen, not of the tile, so a page-container delete leaves a
// fullscreen view running that would freeze.
TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "camera keeps its stream lifetime valid across a raw tile delete",
                 "[camera][teardown][uaf]") {
    AttachedCamera attached(*this);

    helix::LifetimeToken token = helix::CameraWidgetTestAccess::stream_token(*attached.widget);
    REQUIRE_FALSE(token.expired());

    lv_obj_delete(attached.page);

    CHECK_FALSE(token.expired());
}

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "camera uninstalls its delete hook when destroyed before its tree",
                 "[camera][teardown][uaf]") {
    AttachedCamera attached(*this);

    // Guards against passing for the wrong reason: if attach() never installed
    // the hook, its absence after destruction would prove nothing.
    REQUIRE(delete_hook_installed(attached.tile, attached.widget.get()));

    const void* dead = attached.widget.get();
    attached.widget.reset();
    CHECK_FALSE(delete_hook_installed(attached.tile, dead));

    // The tree outlives the widget exactly as it does under lv_deinit().
    lv_obj_delete(attached.tile);
    UpdateQueue::instance().drain();
    SUCCEED("tile torn down after the widget without touching freed memory");
}

TEST_CASE_METHOD(HomeWidgetTeardownFixture, "camera ignores a replaced root's late delete event",
                 "[camera][teardown][uaf]") {
    AttachedCamera attached(*this);
    lv_obj_t* old_tile = attached.tile;

    attached.widget->attach(make_tile(attached.page, "panel_widget_camera"), test_screen());
    UpdateQueue::instance().drain();
    REQUIRE(helix::CameraWidgetTestAccess::camera_image(*attached.widget) != nullptr);

    lv_obj_delete(old_tile);
    UpdateQueue::instance().drain();

    CHECK(helix::CameraWidgetTestAccess::camera_image(*attached.widget) != nullptr);
}

#endif // HELIX_HAS_CAMERA
