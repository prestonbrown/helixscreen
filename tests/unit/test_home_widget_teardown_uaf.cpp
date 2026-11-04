// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_home_widget_teardown_uaf.cpp
 * @brief Home-panel widgets must not touch their tile trees after those trees are deleted
 *
 * The home-panel C++ widgets (NozzleTempsWidget, ToolSwitcherWidget,
 * ThermistorWidget) cache raw pointers — row labels, the tile root,
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

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/discovery_of_objects.h"
#include "../test_helpers/nozzle_temps_test_access.h"
#include "../test_helpers/thermistor_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "panel_widget_manager.h"
#if HELIX_HAS_CAMERA
#include "../test_helpers/camera_widget_test_access.h"
#include "src/ui/panel_widgets/camera_widget.h"
#endif
#include "../test_helpers/network_widget_test_access.h"
#include "../test_helpers/print_stats_test_access.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/shutdown_widget_test_access.h"
#include "../test_helpers/tool_switcher_test_access.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "print_lifecycle_state.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/network_widget.h"
#include "src/ui/panel_widgets/nozzle_temps_widget.h"
#include "src/ui/panel_widgets/print_stats_widget.h"
#include "src/ui/panel_widgets/shutdown_widget.h"
#include "src/ui/panel_widgets/thermistor_widget.h"
#include "src/ui/panel_widgets/tool_switcher_widget.h"
#include "temperature_sensor_manager.h"
#include "tool_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::UpdateQueue;

namespace {

constexpr const char* kThermistorSensorA = "temperature_sensor helix_teardown_a";
constexpr const char* kThermistorSensorB = "temperature_sensor helix_teardown_b";

/// LVGLUITestFixture does not own ToolState's subjects, and the tests below
/// seed it (tool_switcher needs tools to publish; nozzle_temps needs the
/// extruder mapping). Clear it on the way out or later test files in the same
/// binary inherit stale tools (same trap as test_widget_size_tool_switcher.cpp).
struct HomeWidgetTeardownFixture : LVGLUITestFixture {
    ~HomeWidgetTeardownFixture() override {
        ToolState::instance().deinit_subjects();
        UpdateQueue::instance().drain_until_empty();
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
    state.temperature_state().init_extruders({"extruder"});

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

    // An extruder-temp change fires the row's observe<int> observer
    // synchronously; the handler body is queued. This is the pending lambda a
    // screen teardown can drain after the tree is already gone.
    lv_subject_t* temp = state().temperature_state().get_extruder_temp_subject("extruder");
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
/// publish its heads. detach() is NOT called: the page is raw-deleted underneath a
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

        // A rail-sized tile, so every publish path has run once.
        lv_obj_set_size(tile, 400, 400);
        lv_obj_update_layout(tile);
        widget->on_size_changed(2, 2, 400, 400);
        UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "tool_switcher drops its tile root when the page tree is deleted raw",
                 "[tool_switcher][teardown][uaf]") {
    configure_tools(3);
    AttachedToolSwitcher attached(*this);
    REQUIRE(ToolSwitcherTestAccess::tile_root(*attached.widget) == attached.tile);

    lv_obj_delete(attached.page);

    CHECK(ToolSwitcherTestAccess::tile_root(*attached.widget) == nullptr);
}

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "tool_switcher queued print-gating publish survives a drain after the tree dies",
                 "[tool_switcher][teardown][uaf]") {
    configure_tools(3);
    AttachedToolSwitcher attached(*this);

    // A print-lifecycle transition fires print_state_observer_; its handler
    // is queued.
    lv_subject_t* lifecycle = state().print_state().get_print_lifecycle_subject();
    REQUIRE(lifecycle != nullptr);
    lv_subject_set_int(lifecycle, static_cast<int>(PrintState::Printing));

    // Tree dies while the handler is still queued: the expired token turns it
    // into a no-op.
    lv_obj_delete(attached.page);
    UpdateQueue::instance().drain();

    CHECK(ToolSwitcherTestAccess::tile_root(*attached.widget) == nullptr);
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

    // Recycle: attach() the same instance onto a fresh tile, then the old
    // tile's delete arrives late.
    lv_obj_t* new_tile = make_tile(attached.page, "panel_widget_tool_switcher");
    attached.widget->attach(new_tile, test_screen());
    UpdateQueue::instance().drain();
    lv_obj_set_size(new_tile, 400, 400);
    lv_obj_update_layout(new_tile);
    attached.widget->on_size_changed(2, 2, 400, 400);
    UpdateQueue::instance().drain();
    REQUIRE(ToolSwitcherTestAccess::tile_root(*attached.widget) == new_tile);

    lv_obj_delete(attached.tile);
    UpdateQueue::instance().drain();

    CHECK(ToolSwitcherTestAccess::tile_root(*attached.widget) == new_tile);
}

// ============================================================================
// ThermistorWidget
// ============================================================================

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "thermistor drops cached labels when the page tree is deleted raw",
                 "[thermistor][teardown][uaf]") {
    helix::sensors::TemperatureSensorManager::instance().discover(
        helix::test::discovery_of({kThermistorSensorA}));

    auto widget = std::make_unique<ThermistorWidget>("thermistor");
    widget->set_config({{"sensors", std::vector<std::string>{kThermistorSensorA}}});
    lv_obj_t* page = make_page();
    widget->attach(make_tile(page, "panel_widget_thermistor"), test_screen());
    UpdateQueue::instance().drain();

    REQUIRE(ThermistorTestAccess::name_label(*widget) != nullptr);

    lv_obj_delete(page);

    CHECK(ThermistorTestAccess::name_label(*widget) == nullptr);
}

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "thermistor queued temp apply survives a drain after the tree dies",
                 "[thermistor][teardown][uaf]") {
    helix::sensors::TemperatureSensorManager::instance().discover(
        helix::test::discovery_of({kThermistorSensorA}));

    auto widget = std::make_unique<ThermistorWidget>("thermistor");
    widget->set_config({{"sensors", std::vector<std::string>{kThermistorSensorA}}});
    lv_obj_t* page = make_page();
    widget->attach(make_tile(page, "panel_widget_thermistor"), test_screen());
    UpdateQueue::instance().drain();
    REQUIRE(ThermistorTestAccess::name_label(*widget) != nullptr);

    // The sensor's temp subject fires temp_observer_ synchronously; the handler
    // (on_temp_changed) is queued.
    lv_subject_t* subject =
        helix::sensors::TemperatureSensorManager::instance().get_temp_subject(kThermistorSensorA);
    REQUIRE(subject != nullptr);
    lv_subject_set_int(subject, 425);

    // Tree dies while the handler is still queued: the drained handler must
    // touch nothing in the freed tree.
    lv_obj_delete(page);
    UpdateQueue::instance().drain();

    CHECK(ThermistorTestAccess::name_label(*widget) == nullptr);
}

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "thermistor uninstalls its delete hook when destroyed before its tree",
                 "[thermistor][teardown][uaf]") {
    helix::sensors::TemperatureSensorManager::instance().discover(
        helix::test::discovery_of({kThermistorSensorA}));

    auto widget = std::make_unique<ThermistorWidget>("thermistor");
    widget->set_config({{"sensors", std::vector<std::string>{kThermistorSensorA}}});
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
        helix::test::discovery_of({kThermistorSensorA, kThermistorSensorB}));

    auto widget = std::make_unique<ThermistorWidget>("thermistor");
    widget->set_config(
        {{"sensors", std::vector<std::string>{kThermistorSensorA, kThermistorSensorB}},
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
// CameraWidget
// ============================================================================
//
// CameraWidget is the only one of these widgets that deliberately does not
// expire its guard on detach: the MJPEG stream keeps running across a
// detach->reattach gap, and the frame callbacks stay safe purely because
// camera_image_ is null in between. That makes the pointer drop load-bearing
// rather than defensive - a raw lv_obj_delete() of the page tree calls no
// detach(), and without the hook the stream's next frame (10-30 per second)
// writes lv_image_set_src() into a freed lv_image.

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

// The fullscreen overlay is a child of the screen, not the tile, and a screen
// teardown erases its NavigationManager close callback without running it. The
// frame deferral prefers fullscreen_image_ over the tile image, and the
// destructor deletes fullscreen_overlay_, so both must drop with the overlay
// (#1430).
TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "camera drops its fullscreen pointers when the overlay is deleted raw",
                 "[camera][teardown][uaf]") {
    using helix::CameraWidgetTestAccess;
    AttachedCamera attached(*this);

    CameraWidgetTestAccess::show_fullscreen_overlay(*attached.widget);
    UpdateQueue::instance().drain();
    lv_obj_t* overlay = CameraWidgetTestAccess::fullscreen_overlay(*attached.widget);
    REQUIRE(overlay != nullptr);
    REQUIRE(CameraWidgetTestAccess::fullscreen_image(*attached.widget) != nullptr);
    REQUIRE(CameraWidgetTestAccess::fullscreen_spinner(*attached.widget) != nullptr);
    REQUIRE(attached.widget->has_overlay_open());

    lv_obj_delete(overlay);
    UpdateQueue::instance().drain();

    CHECK(CameraWidgetTestAccess::fullscreen_overlay(*attached.widget) == nullptr);
    CHECK(CameraWidgetTestAccess::fullscreen_image(*attached.widget) == nullptr);
    CHECK(CameraWidgetTestAccess::fullscreen_spinner(*attached.widget) == nullptr);
    CHECK_FALSE(attached.widget->has_overlay_open());

    // The single-owner slot is free again: a second overlay opens.
    CameraWidgetTestAccess::show_fullscreen_overlay(*attached.widget);
    UpdateQueue::instance().drain();
    REQUIRE(CameraWidgetTestAccess::fullscreen_overlay(*attached.widget) != nullptr);

    // detach() deletes the live overlay and nothing else.
    attached.widget.reset();
    UpdateQueue::instance().drain();
    SUCCEED("widget destroyed after a raw overlay delete without touching freed memory");
}

#endif // HELIX_HAS_CAMERA

// --------------------------------------------------------------------------
// PrintStatsWidget
//
// This one publishes through static lv_subject_t values rather than into its
// tile tree, so a drained totals callback is harmless by itself and the visible
// UI state cannot tell a dropped pointer from a dangling one. The exposure is
// the cached root: detach() calls lv_obj_set_user_data(widget_obj_, nullptr),
// so a widget_obj_ that outlives its object writes into freed memory on the
// next teardown.
// --------------------------------------------------------------------------

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "print_stats drops its cached root when the page tree is deleted raw",
                 "[print_stats][teardown][uaf]") {
    auto widget = std::make_unique<PrintStatsWidget>();
    lv_obj_t* page = make_page();
    lv_obj_t* tile = make_tile(page, "panel_widget_print_stats");
    widget->attach(tile, test_screen());
    UpdateQueue::instance().drain();

    // Or a null read below would prove nothing.
    REQUIRE(PrintStatsTestAccess::widget_obj(*widget) == tile);

    lv_obj_delete(page);
    UpdateQueue::instance().drain();

    CHECK(PrintStatsTestAccess::widget_obj(*widget) == nullptr);
    CHECK(PrintStatsTestAccess::parent_screen(*widget) == nullptr);

    // The path that would have written into the freed tile.
    widget->detach();
    SUCCEED("detach after a raw page delete touched no freed memory");
}

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "print_stats uninstalls its delete hook when destroyed before its tree",
                 "[print_stats][teardown][uaf]") {
    auto widget = std::make_unique<PrintStatsWidget>();
    lv_obj_t* page = make_page();
    lv_obj_t* tile = make_tile(page, "panel_widget_print_stats");
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

// --------------------------------------------------------------------------
// ShutdownWidget
//
// Its verification timer carries a heap VerifyCtx*, not `this`, and its
// tok.defer() bodies call free functions, so neither reaches back into the
// widget. What is left is the same cached-root exposure: detach() calls
// lv_obj_set_user_data(widget_obj_, nullptr), and it also holds the button
// pointer it registered a click handler on.
// --------------------------------------------------------------------------

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "shutdown drops its cached root and button when the page tree is deleted raw",
                 "[shutdown][teardown][uaf]") {
    // No API needed: nothing on the teardown path calls one.
    auto widget = std::make_unique<ShutdownWidget>(nullptr);
    lv_obj_t* page = make_page();
    lv_obj_t* tile = make_tile(page, "panel_widget_shutdown");
    widget->attach(tile, test_screen());
    UpdateQueue::instance().drain();

    // Or the null reads below would prove nothing.
    REQUIRE(ShutdownWidgetTestAccess::widget_obj(*widget) == tile);

    lv_obj_delete(page);
    UpdateQueue::instance().drain();

    CHECK(ShutdownWidgetTestAccess::widget_obj(*widget) == nullptr);
    CHECK(ShutdownWidgetTestAccess::shutdown_btn(*widget) == nullptr);
    CHECK(ShutdownWidgetTestAccess::parent_screen(*widget) == nullptr);

    // The path that would have written into the freed tile.
    widget->detach();
    SUCCEED("detach after a raw page delete touched no freed memory");
}

TEST_CASE_METHOD(HomeWidgetTeardownFixture,
                 "shutdown uninstalls its delete hook when destroyed before its tree",
                 "[shutdown][teardown][uaf]") {
    auto widget = std::make_unique<ShutdownWidget>(nullptr);
    lv_obj_t* page = make_page();
    lv_obj_t* tile = make_tile(page, "panel_widget_shutdown");
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

// --------------------------------------------------------------------------
// NetworkWidget
//
// The one here that needs more than pointer drops. Its signal-poll timer holds
// `this` and the widget outlives the tree on this path, so the callback's own
// `self` check still passes and a null root does not stop it. The body writes a
// module-owned static subject rather than the tree, so it is not a UAF by
// itself — but it keeps polling a screen that is gone, and at process teardown
// that subject dies first.
// --------------------------------------------------------------------------

/// Pins the WiFi backend to its mock for the two cases below.
///
/// NetworkWidget::attach() reaches get_wifi_manager(), the process-lifetime
/// WiFi singleton, whose backend is chosen once for the whole process. With
/// test_mode off, WifiBackend::create() builds the real platform backend, and
/// its start_async() worker probes this machine's live network state through
/// nmcli/wpa_cli. That thread has no join site: the singleton is deliberately
/// never destroyed, and ~WiFiManager -> backend_->stop() is the only place the
/// backend's threads are joined, so the sanitizer sees a leaked thread at exit.
/// The mock backend runs its init on the caller's thread and spawns none.
struct NetworkWidgetTeardownFixture : HomeWidgetTeardownFixture {
    ScopedRuntimeConfig scoped_config_;

    NetworkWidgetTeardownFixture() {
        auto* rc = get_runtime_config();
        rc->test_mode = true;
        rc->use_real_wifi = false;
    }
};

TEST_CASE_METHOD(NetworkWidgetTeardownFixture,
                 "network drops its cached root when the page tree is deleted raw",
                 "[network_widget][teardown][uaf]") {
    auto widget = std::make_unique<NetworkWidget>();
    lv_obj_t* page = make_page();
    lv_obj_t* tile = make_tile(page, "panel_widget_network");
    widget->attach(tile, test_screen());
    UpdateQueue::instance().drain();

    // Or the null reads below would prove nothing.
    REQUIRE(NetworkWidgetTestAccess::widget_obj(*widget) == tile);

    lv_obj_delete(page);
    UpdateQueue::instance().drain();

    CHECK(NetworkWidgetTestAccess::widget_obj(*widget) == nullptr);
    CHECK(NetworkWidgetTestAccess::parent_screen(*widget) == nullptr);
    // Whether the poll timer was ever started depends on the detected network,
    // so this pins the invariant rather than the transition: nothing may still
    // be polling once the tree is gone.
    CHECK(NetworkWidgetTestAccess::signal_poll_timer(*widget) == nullptr);

    // The path that would have written into the freed tile.
    widget->detach();
    SUCCEED("detach after a raw page delete touched no freed memory");
}

TEST_CASE_METHOD(NetworkWidgetTeardownFixture,
                 "network uninstalls its delete hook when destroyed before its tree",
                 "[network_widget][teardown][uaf]") {
    auto widget = std::make_unique<NetworkWidget>();
    lv_obj_t* page = make_page();
    lv_obj_t* tile = make_tile(page, "panel_widget_network");
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
