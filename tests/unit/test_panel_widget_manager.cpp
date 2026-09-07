// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../ui_test_utils.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "misc/lv_timer_private.h"
#include "moonraker_client.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Drain LVGL's async list (lv_async_call queue) by calling lv_timer_handler
/// repeatedly until no one-shot timer fires. lv_async_call schedules its
/// callback as a one-shot timer; lv_timer_handler dispatches it.
void process_async_calls() {
    for (int safety = 0; safety < 50; ++safety) {
        bool fired = false;
        lv_timer_t* t = lv_timer_get_next(nullptr);
        while (t) {
            lv_timer_t* next = lv_timer_get_next(t);
            if (t->repeat_count > 0 && t->timer_cb) {
                t->timer_cb(t);
                fired = true;
                break;
            }
            t = next;
        }
        if (!fired)
            break;
    }
}

} // namespace

TEST_CASE("PanelWidget: supports_reuse defaults to true", "[panel_widget]") {
    struct TestWidget : PanelWidget {
        void attach(lv_obj_t*, lv_obj_t*) override {}
        void detach() override {}
        const char* id() const override {
            return "test";
        }
    };
    TestWidget w;
    REQUIRE(w.supports_reuse() == true);
}

TEST_CASE("PanelWidgetManager singleton access", "[panel_widget][manager]") {
    auto& mgr = PanelWidgetManager::instance();
    auto& mgr2 = PanelWidgetManager::instance();
    REQUIRE(&mgr == &mgr2);
}

TEST_CASE("PanelWidgetManager shared resources", "[panel_widget][manager]") {
    auto& mgr = PanelWidgetManager::instance();
    mgr.clear_shared_resources();

    SECTION("returns nullptr for unregistered type") {
        REQUIRE(mgr.shared_resource<int>() == nullptr);
    }

    SECTION("register and retrieve") {
        auto val = std::make_shared<int>(42);
        mgr.register_shared_resource<int>(val);
        REQUIRE(mgr.shared_resource<int>() != nullptr);
        REQUIRE(*mgr.shared_resource<int>() == 42);
    }

    SECTION("clear removes all resources") {
        auto val = std::make_shared<int>(99);
        mgr.register_shared_resource<int>(val);
        mgr.clear_shared_resources();
        REQUIRE(mgr.shared_resource<int>() == nullptr);
    }

    SECTION("multiple types coexist") {
        auto i = std::make_shared<int>(10);
        auto s = std::make_shared<std::string>("hello");
        mgr.register_shared_resource<int>(i);
        mgr.register_shared_resource<std::string>(s);
        REQUIRE(*mgr.shared_resource<int>() == 10);
        REQUIRE(*mgr.shared_resource<std::string>() == "hello");
        mgr.clear_shared_resources();
    }
}

TEST_CASE("PanelWidgetManager config change callbacks", "[panel_widget][manager]") {
    auto& mgr = PanelWidgetManager::instance();

    SECTION("callback is invoked on notify") {
        bool called = false;
        mgr.register_rebuild_callback("test_panel", [&called]() { called = true; });
        mgr.notify_config_changed("test_panel");
        REQUIRE(called);
        mgr.unregister_rebuild_callback("test_panel");
    }

    SECTION("notify for nonexistent panel does not crash") {
        mgr.notify_config_changed("nonexistent");
    }

    SECTION("unregister removes callback") {
        int count = 0;
        mgr.register_rebuild_callback("counting", [&count]() { count++; });
        mgr.notify_config_changed("counting");
        REQUIRE(count == 1);
        mgr.unregister_rebuild_callback("counting");
        mgr.notify_config_changed("counting");
        REQUIRE(count == 1);
    }
}

TEST_CASE("PanelWidgetManager populate with null container", "[panel_widget][manager]") {
    auto& mgr = PanelWidgetManager::instance();
    auto widgets = mgr.populate_widgets("home", nullptr);
    REQUIRE(widgets.empty());
}

TEST_CASE("Widget factories are self-registered", "[panel_widget][self_registration]") {
    lv_init_safe(); // Widget registration requires LVGL for XML event callbacks
    helix::init_widget_registrations();

    const char* expected[] = {"temperature", "temp_stack", "led",      "power_device",
                              "network",     "thermistor", "fan_stack"};
    for (const auto* id : expected) {
        INFO("Checking widget factory: " << id);
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        REQUIRE(def->factory != nullptr);
    }
}

TEST_CASE("PanelWidgetManager raw pointer shared resources", "[panel_widget][manager]") {
    auto& mgr = PanelWidgetManager::instance();
    mgr.clear_shared_resources();

    int stack_val = 77;
    mgr.register_shared_resource<int>(&stack_val);
    REQUIRE(mgr.shared_resource<int>() != nullptr);
    REQUIRE(*mgr.shared_resource<int>() == 77);
    mgr.clear_shared_resources();
}

// Regression test for AD5X bundles XG9QJ3V9 / PFEHDEXF (v0.99.49):
// SIGBUS in unsubscribe_on_delete_cb -> lv_obj_remove_event_cb_with_user_data
// during early startup, after a burst of 8 hardware-gate subjects fired in
// ~150 ms. Each firing triggered populate_page synchronously in the same
// UpdateQueue tick; the resulting backlog of N×children async deletes
// corrupted LVGL's event list (L081 family).
//
// Fix: setup_gate_observers now coalesces multiple firings within a tick
// into a single async-deferred rebuild via lv_async_call. This test
// verifies the coalescing invariant: regardless of how many gate observers
// fire in one drain, rebuild_cb runs at most once.
TEST_CASE("PanelWidgetManager coalesces multiple gate firings into one rebuild",
          "[panel_widget][manager][regression][L081]") {
    lv_init_safe();
    helix::init_widget_registrations();

    auto& mgr = PanelWidgetManager::instance();

    // Register klippy_state (always observed by setup_gate_observers) plus
    // the hardware_gate_subject for every registered widget def. Any subject
    // that doesn't already exist gets created here so the observer chain has
    // something to attach to. nullptr scope = global.
    static lv_subject_t klippy_state_subj;
    if (!lv_xml_get_subject(nullptr, "klippy_state")) {
        lv_subject_init_int(&klippy_state_subj, 0);
        lv_xml_register_subject(nullptr, "klippy_state", &klippy_state_subj);
    }

    // Static so registrations persist across SECTION re-entries; otherwise the
    // second SECTION sees the global registrations from the first but its
    // local vector is empty and we can't iterate to set values.
    static std::vector<std::pair<std::string, lv_subject_t*>> all_gate_subjs = []() {
        std::vector<std::pair<std::string, lv_subject_t*>> out;
        for (const auto& def : helix::get_all_widget_defs()) {
            if (!def.hardware_gate_subject)
                continue;
            const char* name = def.hardware_gate_subject;
            // De-dup
            bool dup = false;
            for (const auto& kv : out) {
                if (kv.first == name) {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;
            if (auto* existing = lv_xml_get_subject(nullptr, name)) {
                out.emplace_back(name, existing);
            } else {
                auto* subj = new lv_subject_t;
                lv_subject_init_int(subj, 0);
                lv_xml_register_subject(nullptr, name, subj);
                out.emplace_back(name, subj);
            }
        }
        return out;
    }();
    REQUIRE(all_gate_subjs.size() >= 2); // need at least 2 to test coalescing

    int rebuild_count = 0;
    mgr.setup_gate_observers("test_panel", [&rebuild_count]() { ++rebuild_count; });

    auto& q = helix::ui::UpdateQueue::instance();

    SECTION("burst of N firings produces 1 rebuild") {
        // Set every gate subject to a new value. Each set fires the observer's
        // queue_update; combined with the immediate-fire from registration,
        // we get 2N callbacks queued. Without coalescing, each would fire its
        // own rebuild_cb — causing the L081 backlog corruption seen on AD5X.
        if (auto* ks = lv_xml_get_subject(nullptr, "klippy_state"))
            lv_subject_set_int(ks, 1);
        for (auto& [name, subj] : all_gate_subjs) {
            lv_subject_set_int(subj, 1);
        }

        // Drain the UpdateQueue — delivers all observer callbacks.
        // Each callback either schedules a new lv_async_call (the first one
        // in the tick) or coalesces (the rest).
        q.drain();

        // No rebuild has run yet — it's queued via lv_async_call.
        REQUIRE(rebuild_count == 0);

        // Run LVGL's async list. Should fire exactly one rebuild.
        process_async_calls();
        REQUIRE(rebuild_count == 1);
    }

    SECTION("late-arriving gate after rebuild starts queues another rebuild") {
        // First burst → 1 rebuild
        if (auto* ks = lv_xml_get_subject(nullptr, "klippy_state"))
            lv_subject_set_int(ks, 1);
        for (auto& [name, subj] : all_gate_subjs) {
            lv_subject_set_int(subj, 1);
        }
        q.drain();
        process_async_calls();
        REQUIRE(rebuild_count == 1);

        // Second burst (different values) after first rebuild completed →
        // pending flag was cleared, so this queues another rebuild.
        for (auto& [name, subj] : all_gate_subjs) {
            lv_subject_set_int(subj, 2);
        }
        q.drain();
        process_async_calls();
        REQUIRE(rebuild_count == 2);
    }

    // Cleanup observers so the test fixture's reset_all() doesn't see stale
    // state. setup_gate_observers stores the new vector under panel_id; the
    // destructor of ObserverGuard will call lv_observer_remove which is safe
    // because the subjects are still alive at this point.
    mgr.clear_gate_observers("test_panel");
}

namespace {

// Spy PanelWidget used by the grid-build-race regression test. Records the
// layout that the page container had at the moment its attach() ran, so the
// test can assert children are created BEFORE the container becomes a grid.
struct GridSpyWidget : helix::PanelWidget {
    static int s_layout_at_attach; // lv_obj_get_style_layout of parent at attach
    static int s_attach_count;
    static lv_obj_t* s_attached_widget;

    void attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) override {
        s_attached_widget = widget_obj;
        ++s_attach_count;
        lv_obj_t* parent = widget_obj ? lv_obj_get_parent(widget_obj) : nullptr;
        s_layout_at_attach =
            parent ? static_cast<int>(lv_obj_get_style_layout(parent, LV_PART_MAIN)) : -1;
    }
    void detach() override {}
    const char* id() const override {
        return "clock";
    }
    // Use a private, dependency-free inline XML component so we exercise the
    // real lv_xml_create() + attach() path without pulling in the clock's
    // subjects/bindings.
    std::string get_component_name() const override {
        return "test_grid_spy_widget";
    }
};

int GridSpyWidget::s_layout_at_attach = -2;
int GridSpyWidget::s_attach_count = 0;
lv_obj_t* GridSpyWidget::s_attached_widget = nullptr;

} // namespace

// Regression test for #983: SIGSEGV in LVGL grid_update() while
// PanelWidgetManager::populate_widgets() builds the home grid. The crash
// happened because the page container had LV_LAYOUT_GRID activated BEFORE its
// children (card backgrounds + widgets) were created; a widget whose attach()
// synchronously triggered lv_obj_update_layout (e.g. PrintStatusWidget ->
// lv_image_set_src -> update_align, see print_status_widget.cpp:331) cascaded a
// grid_update on a half-built grid. The fix defers grid-layout activation until
// all children exist, then runs one clean layout pass.
//
// Invariant captured here: at the moment any widget's attach() runs, the
// container's layout is NOT yet LV_LAYOUT_GRID; after populate_widgets()
// returns, the container's layout IS LV_LAYOUT_GRID and the child exists.
// This FAILS before the fix (grid active during build) and PASSES after.
TEST_CASE_METHOD(XMLTestFixture,
                 "PanelWidgetManager activates grid layout after children are built",
                 "[panel_widget][manager][regression]") {
    helix::init_widget_registrations();

    // Register a minimal, dependency-free XML component for the spy widget so
    // lv_xml_create() succeeds without needing the real widget's subjects.
    lv_xml_register_component_from_data(
        "test_grid_spy_widget",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");

    // Override the 'clock' factory with our spy factory. 'clock' is a plain
    // non-gated widget; restored at the end of the test.
    const auto* clock_def = helix::find_widget_def("clock");
    REQUIRE(clock_def != nullptr);
    WidgetFactory original_clock_factory = clock_def->factory;
    helix::register_widget_factory("clock", [](const std::string&) -> std::unique_ptr<PanelWidget> {
        return std::make_unique<GridSpyWidget>();
    });

    GridSpyWidget::s_layout_at_attach = -2;
    GridSpyWidget::s_attach_count = 0;
    GridSpyWidget::s_attached_widget = nullptr;

    const std::string panel_id = "test_grid_race";

    // Write a 2-page config: page 0 is empty, page 1 (a secondary page, which
    // does NOT get registry-default widgets appended) holds exactly one
    // enabled spy widget with an explicit 1x1 grid position. Driving
    // populate_widgets on page 1 isolates the build to our spy widget.
    auto* cfg = Config::get_instance();
    nlohmann::json widget_cfg = {{"main_page_index", 0},
                                 {"next_page_id", 2},
                                 {"pages",
                                  {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                                   {{"id", "spy"},
                                    {"widgets",
                                     {{{"id", "clock"},
                                       {"enabled", true},
                                       {"col", 0},
                                       {"row", 0},
                                       {"colspan", 1},
                                       {"rowspan", 1}}}}}}}};
    cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/" + panel_id, widget_cfg);

    // Force a reload + clear any cached active config / grid descriptors so the
    // populate path does a full rebuild.
    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);

    // A real on-screen container with a definite size so cell math is sane.
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 300);
    process_lvgl(10);

    REQUIRE(lv_obj_get_style_layout(container, LV_PART_MAIN) != LV_LAYOUT_GRID);

    auto widgets = mgr.populate_widgets(panel_id, container, /*page_index=*/1);

    // The spy widget's attach() must have run, and observed a non-grid layout.
    REQUIRE(GridSpyWidget::s_attach_count == 1);
    INFO("layout at attach (LV_LAYOUT_GRID=" << static_cast<int>(LV_LAYOUT_GRID)
                                             << ") was: " << GridSpyWidget::s_layout_at_attach);
    REQUIRE(GridSpyWidget::s_layout_at_attach != static_cast<int>(LV_LAYOUT_GRID));

    // After populate_widgets returns the grid IS active and the child exists.
    REQUIRE(lv_obj_get_style_layout(container, LV_PART_MAIN) == LV_LAYOUT_GRID);
    REQUIRE(lv_obj_get_child_count(container) > 0);
    REQUIRE(GridSpyWidget::s_attached_widget != nullptr);
    // The attached widget is parented into the page container.
    REQUIRE(lv_obj_get_parent(GridSpyWidget::s_attached_widget) == container);

    // Restore global registry state for subsequent tests.
    helix::register_widget_factory("clock", original_clock_factory);
    mgr.clear_panel_config(panel_id);
}

namespace {

// Spy PanelWidget for the grid *rebuild* race (#983, bundle VDJ3J9UV). Unlike
// GridSpyWidget it actively FORCES a synchronous layout from inside attach() —
// exactly what PrintStatusWidget does (resize_and_publish -> lv_obj_update_layout,
// ui_progress_arc.cpp). On a rebuild the page container is reused and is still in
// LV_LAYOUT_GRID from the previous pass, its grid style pointing at the old
// dsc.col_dsc buffer; populate_widgets() move-reassigns that vector (freeing the
// buffer) before reinstalling a fresh descriptor at the end. If the grid is still
// active when this forced layout runs, grid_update -> count_tracks walks the freed
// descriptor off the heap end -> SIGSEGV. The fix deactivates the grid
// (LV_LAYOUT_NONE) at the start of every (re)build so the container is not a live
// grid while children attach.
struct GridRebuildSpyWidget : helix::PanelWidget {
    static int s_layout_at_last_attach; // container layout when the most recent attach() ran
    static int s_attach_count;

    void attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) override {
        ++s_attach_count;
        lv_obj_t* parent = widget_obj ? lv_obj_get_parent(widget_obj) : nullptr;
        s_layout_at_last_attach =
            parent ? static_cast<int>(lv_obj_get_style_layout(parent, LV_PART_MAIN)) : -1;
        // Force the synchronous layout that triggers the crash pre-fix. With the
        // fix the parent is LV_LAYOUT_NONE here, so this is a harmless no-op walk.
        if (parent)
            lv_obj_update_layout(parent);
    }
    void detach() override {}
    const char* id() const override {
        return "clock";
    }
    std::string get_component_name() const override {
        return "test_grid_rebuild_spy_widget";
    }
};

int GridRebuildSpyWidget::s_layout_at_last_attach = -2;
int GridRebuildSpyWidget::s_attach_count = 0;

} // namespace

// Regression test for #983 rebuild path (bundle VDJ3J9UV, v0.99.75, Pi): SIGSEGV
// in grid_update -> count_tracks while REPOPULATING the home grid. The build-time
// fix (#983, commit 69e9923dd) activates the grid last, which only covers the
// first build — on a rebuild the reused container enters populate_widgets already
// in LV_LAYOUT_GRID holding the previous build's descriptor pointer, which the
// move-assignment `dsc.col_dsc = make_col_dsc(...)` then frees. A child whose
// attach() forces a layout walks the freed descriptor and crashes.
//
// Invariant: on the SECOND populate_widgets (rebuild) of the same container, the
// container's layout is NOT LV_LAYOUT_GRID at the moment a child attaches (the fix
// deactivated it), and the process survives the attach-forced layout. FAILS
// pre-fix (container still grid during rebuild) and PASSES after.
TEST_CASE_METHOD(XMLTestFixture,
                 "PanelWidgetManager deactivates grid before rebuilding a reused container",
                 "[panel_widget][manager][regression]") {
    helix::init_widget_registrations();

    lv_xml_register_component_from_data(
        "test_grid_rebuild_spy_widget",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");

    const auto* clock_def = helix::find_widget_def("clock");
    REQUIRE(clock_def != nullptr);
    WidgetFactory original_clock_factory = clock_def->factory;
    helix::register_widget_factory("clock", [](const std::string&) -> std::unique_ptr<PanelWidget> {
        return std::make_unique<GridRebuildSpyWidget>();
    });

    const std::string panel_id = "test_grid_rebuild_race";

    auto* cfg = Config::get_instance();
    nlohmann::json widget_cfg = {{"main_page_index", 0},
                                 {"next_page_id", 2},
                                 {"pages",
                                  {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                                   {{"id", "spy"},
                                    {"widgets",
                                     {{{"id", "clock"},
                                       {"enabled", true},
                                       {"col", 0},
                                       {"row", 0},
                                       {"colspan", 1},
                                       {"rowspan", 1}}}}}}}};
    cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/" + panel_id, widget_cfg);

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);

    // One reused container — the crux of the rebuild race.
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 300);
    process_lvgl(10);

    // First build: leaves the container in LV_LAYOUT_GRID with a live descriptor.
    auto widgets1 = mgr.populate_widgets(panel_id, container, /*page_index=*/1);
    REQUIRE(GridRebuildSpyWidget::s_attach_count == 1);
    REQUIRE(lv_obj_get_style_layout(container, LV_PART_MAIN) == LV_LAYOUT_GRID);

    // Second build (rebuild) of the SAME container. clear_panel_config() is the
    // production grid-edit rebuild sequence (GridEditMode): it erases this panel's
    // active_configs_ (so populate_widgets does a full rebuild instead of taking
    // the "widget list unchanged" early-out) AND erases grid_descriptors_, which
    // *frees the descriptor buffer the reused container's grid style still points
    // at* — exactly the dangling pointer that count_tracks walks off pre-fix.
    GridRebuildSpyWidget::s_layout_at_last_attach = -2;
    widgets1.clear(); // release the first build's widget instances
    mgr.clear_panel_config(panel_id);
    auto widgets2 = mgr.populate_widgets(panel_id, container, /*page_index=*/1);

    REQUIRE(GridRebuildSpyWidget::s_attach_count == 2);
    INFO("container layout at rebuild attach (LV_LAYOUT_GRID="
         << static_cast<int>(LV_LAYOUT_GRID)
         << ") was: " << GridRebuildSpyWidget::s_layout_at_last_attach);
    REQUIRE(GridRebuildSpyWidget::s_layout_at_last_attach != static_cast<int>(LV_LAYOUT_GRID));

    // After the rebuild the grid is active again with a fresh descriptor.
    REQUIRE(lv_obj_get_style_layout(container, LV_PART_MAIN) == LV_LAYOUT_GRID);
    REQUIRE(lv_obj_get_child_count(container) > 0);

    helix::register_widget_factory("clock", original_clock_factory);
    mgr.clear_panel_config(panel_id);
}

namespace {

// Spy PanelWidget whose set_config() throws — models a widget with a malformed
// persisted config (e.g. a temp_graph entry that hits a JSON type_error). Used
// by the collection-loop guard regression test below.
struct ThrowingConfigWidget : helix::PanelWidget {
    void set_config(const nlohmann::json&) override {
        throw std::runtime_error("simulated malformed widget config");
    }
    void attach(lv_obj_t*, lv_obj_t*) override {}
    void detach() override {}
    const char* id() const override {
        return "clock";
    }
    std::string get_component_name() const override {
        return "test_grid_spy_widget";
    }
};

} // namespace

// Regression: a widget whose factory/set_config throws during the populate_widgets
// COLLECTION loop used to let the exception escape the Home Panel dashboard
// rebuild -> std::terminate -> SIGABRT, taking every OTHER widget on the page down
// with it. The collection loop now guards each iteration: a throwing widget is
// logged and skipped, and well-formed siblings on the same page still build.
TEST_CASE_METHOD(XMLTestFixture,
                 "PanelWidgetManager skips a widget whose config throws and keeps the rest",
                 "[panel_widget][manager][regression][crash-safety]") {
    helix::init_widget_registrations();

    lv_xml_register_component_from_data(
        "test_grid_spy_widget",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");

    // Route "clock" -> a widget that throws in set_config (the bad entry), and
    // "shutdown" -> a well-formed spy widget (must still attach).
    const auto* clock_def = helix::find_widget_def("clock");
    const auto* shutdown_def = helix::find_widget_def("shutdown");
    REQUIRE(clock_def != nullptr);
    REQUIRE(shutdown_def != nullptr);
    WidgetFactory orig_clock = clock_def->factory;
    WidgetFactory orig_shutdown = shutdown_def->factory;
    helix::register_widget_factory("clock", [](const std::string&) -> std::unique_ptr<PanelWidget> {
        return std::make_unique<ThrowingConfigWidget>();
    });
    helix::register_widget_factory("shutdown",
                                   [](const std::string&) -> std::unique_ptr<PanelWidget> {
                                       return std::make_unique<GridSpyWidget>();
                                   });

    GridSpyWidget::s_attach_count = 0;
    GridSpyWidget::s_attached_widget = nullptr;

    // A populate while Klipper is not READY does not persist its layout, so keep
    // it READY for tests that assert on what reached disk.
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_connection_state"),
                       static_cast<int>(ConnectionState::CONNECTED));
    lv_subject_set_int(lv_xml_get_subject(nullptr, "klippy_state"),
                       static_cast<int>(KlippyState::READY));

    const std::string panel_id = "test_throwing_widget";

    // Page 1 (secondary page, no registry-default append): the throwing "clock"
    // at 0,0 and the well-formed "shutdown" at 1,0.
    auto* cfg = Config::get_instance();
    nlohmann::json widget_cfg = {{"main_page_index", 0},
                                 {"next_page_id", 2},
                                 {"pages",
                                  {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                                   {{"id", "spy"},
                                    {"widgets",
                                     {{{"id", "clock"},
                                       {"enabled", true},
                                       {"col", 0},
                                       {"row", 0},
                                       {"colspan", 1},
                                       {"rowspan", 1}},
                                      {{"id", "shutdown"},
                                       {"enabled", true},
                                       {"col", 1},
                                       {"row", 0},
                                       {"colspan", 1},
                                       {"rowspan", 1}}}}}}}};
    cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/" + panel_id, widget_cfg);

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 300);
    process_lvgl(10);

    // The throwing widget must NOT bring the rebuild down — populate returns.
    std::vector<std::unique_ptr<PanelWidget>> widgets;
    REQUIRE_NOTHROW(widgets = mgr.populate_widgets(panel_id, container, /*page_index=*/1));

    // The well-formed sibling still attached exactly once; the throwing one was
    // skipped (only one PanelWidget instance survives in the result).
    REQUIRE(GridSpyWidget::s_attach_count == 1);
    REQUIRE(GridSpyWidget::s_attached_widget != nullptr);
    REQUIRE(widgets.size() == 1);

    helix::register_widget_factory("clock", orig_clock);
    helix::register_widget_factory("shutdown", orig_shutdown);
    mgr.clear_panel_config(panel_id);
}

// ============================================================================
// Per-printer config cache invalidation on printer switch (#804 regression)
// ============================================================================
//
// PanelWidgetConfig instances are cached process-wide inside PanelWidgetManager
// and only reload from disk when marked dirty. Switching the active printer
// changes Config::df() (the /printers/<id>/ prefix the config reads from), so
// the manager must invalidate every cached panel via clear_all_panel_configs()
// — otherwise it keeps serving the previous printer's layout.
TEST_CASE_METHOD(HelixTestFixture,
                 "PanelWidgetManager: clear_all_panel_configs reloads after printer switch",
                 "[panel_widget][manager]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    auto& mgr = PanelWidgetManager::instance();

    // Build two printers whose "home" panel enables a DIFFERENT, non-default
    // widget. "network" and "shutdown" both default to disabled in the registry,
    // so the registry-default append path can't muddy these assertions.
    auto make_home_layout = [](const char* enabled_id) {
        nlohmann::json widgets = nlohmann::json::array();
        widgets.push_back({{"id", enabled_id}, {"enabled", true}});
        nlohmann::json page = {{"id", "main"}, {"widgets", std::move(widgets)}};
        nlohmann::json root;
        root["pages"] = nlohmann::json::array({std::move(page)});
        root["main_page_index"] = 0;
        root["next_page_id"] = 1;
        return root;
    };

    cfg->add_printer("printer-A", nlohmann::json::object());
    cfg->add_printer("printer-B", nlohmann::json::object());
    cfg->set<nlohmann::json>("/printers/printer-A/panel_widgets/home", make_home_layout("network"));
    cfg->set<nlohmann::json>("/printers/printer-B/panel_widgets/home",
                             make_home_layout("shutdown"));

    // Active printer = A -> home reflects A's layout.
    REQUIRE(cfg->set_active_printer("printer-A"));
    {
        auto& wc = mgr.get_widget_config("home");
        REQUIRE(wc.is_enabled("network"));
        REQUIRE_FALSE(wc.is_enabled("shutdown"));
    }

    // Simulate Application::switch_printer(): change the active printer, then
    // invalidate every cached panel config so the next access re-reads df().
    REQUIRE(cfg->set_active_printer("printer-B"));
    mgr.clear_all_panel_configs();

    // Active printer = B -> home must now reflect B's layout, not the stale A cache.
    {
        auto& wc = mgr.get_widget_config("home");
        REQUIRE(wc.is_enabled("shutdown"));
        REQUIRE_FALSE(wc.is_enabled("network"));
    }

    // Clean up so the cached "home" config doesn't leak the temp printers into
    // later tests sharing the process-wide cache.
    mgr.clear_all_panel_configs();
}

// ============================================================================
// Klipper-not-READY layout clobber (raza616 report): the home/tile layout
// reverts to a previous arrangement after ANY reset — power cycle, Klipper
// restart, or FIRMWARE_RESTART.
// ============================================================================
//
// Root cause: while Klipper is not READY (but connected), populate_widgets()
// injects a *temporary* `firmware_restart` widget that occupies a grid cell
// (panel_widget_manager.cpp ~line 186), then runs its auto-placement pass and
// PERSISTS the computed positions to disk (~line 444:
// `if (any_written) widget_config.save()`). The placement computed while the
// temporary widget is consuming grid space is NOT the user's intended layout,
// yet it gets frozen to disk. On the next READY populate (or next boot) the
// user sees a different, "previous" layout — exactly the reported symptom, and
// it recurs on every reset because the gate observer rebuilds on each
// klippy_state transition.
//
// Invariant: a populate that runs while klippy_state != READY must NOT mutate
// the persisted panel_widgets layout on disk. Persistence of auto-placed
// positions must wait until Klipper is READY (no firmware_restart injected).
// FAILS pre-fix (auto-placed position saved during the injection), PASSES after.
TEST_CASE_METHOD(XMLTestFixture,
                 "PanelWidgetManager does not persist layout while Klipper is not READY",
                 "[panel_widget][manager][regression]") {
    helix::init_widget_registrations();

    // Dependency-free component for the spy widgets, plus a trivial stand-in for
    // the injected firmware_restart widget so the build path stays quiet.
    lv_xml_register_component_from_data(
        "test_grid_spy_widget",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");
    lv_xml_register_component_from_data(
        "panel_widget_firmware_restart",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");

    // Route the two widgets we use through the dependency-free spy factory.
    auto override_factory = [](const char* id) -> WidgetFactory {
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        WidgetFactory original = def->factory;
        helix::register_widget_factory(id, [](const std::string&) -> std::unique_ptr<PanelWidget> {
            return std::make_unique<GridSpyWidget>();
        });
        return original;
    };
    WidgetFactory orig_clock = override_factory("clock");
    WidgetFactory orig_shutdown = override_factory("shutdown");

    // Connected but Klipper NOT ready -> populate_widgets injects firmware_restart.
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_connection_state"),
                       static_cast<int>(ConnectionState::CONNECTED));
    lv_subject_set_int(lv_xml_get_subject(nullptr, "klippy_state"),
                       static_cast<int>(KlippyState::SHUTDOWN));

    const std::string panel_id = "test_klippy_clobber";

    // Page 1 (a secondary page: no registry-default append) holds one positioned
    // widget (clock @0,0) and one auto-placed widget (shutdown @-1,-1). The
    // auto-placed widget is what makes the buggy write-back call save().
    auto* cfg = Config::get_instance();
    nlohmann::json widget_cfg = {{"main_page_index", 0},
                                 {"next_page_id", 2},
                                 {"pages",
                                  {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                                   {{"id", "spy"},
                                    {"widgets",
                                     {{{"id", "clock"},
                                       {"enabled", true},
                                       {"col", 0},
                                       {"row", 0},
                                       {"colspan", 1},
                                       {"rowspan", 1}},
                                      {{"id", "shutdown"},
                                       {"enabled", true},
                                       {"col", -1},
                                       {"row", -1},
                                       {"colspan", 1},
                                       {"rowspan", 1}}}}}}}};
    const std::string panel_path = cfg->df() + "panel_widgets/" + panel_id;
    cfg->set<nlohmann::json>(panel_path, widget_cfg);

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 300);
    process_lvgl(10);

    // Snapshot the persisted layout, populate while Klipper is not READY, snapshot
    // again. The not-READY populate must leave the on-disk layout untouched.
    nlohmann::json before = cfg->get<nlohmann::json>(panel_path, nlohmann::json());
    auto widgets = mgr.populate_widgets(panel_id, container, /*page_index=*/1);
    nlohmann::json after = cfg->get<nlohmann::json>(panel_path, nlohmann::json());

    INFO("before: " << before.dump());
    INFO("after:  " << after.dump());
    REQUIRE(after == before);

    helix::register_widget_factory("clock", orig_clock);
    helix::register_widget_factory("shutdown", orig_shutdown);
    mgr.clear_panel_config(panel_id);
}

// Transient in-memory lock-in: a not-READY populate must also not mutate the
// in-memory entry positions of auto-placed widgets. The gate observer rebuilds
// on each klippy_state transition WITHOUT reloading the cached config, so if a
// not-READY populate writes a transient slot into the in-memory entry, the
// subsequent READY populate sees an "explicit" position, never re-derives it,
// and (because nothing changed) never persists it — the widget is frozen at a
// slot computed while the temporary firmware_restart widget occupied the grid,
// and disk never records the real position.
//
// Invariant: after a not-READY populate followed by a READY populate on the
// SAME cached config (no reload — the live gate-observer rebuild path), the
// auto-placed widget is persisted at the SAME position a clean READY-only
// populate would produce. FAILS with the save-only guard (disk stays col=-1),
// PASSES once the whole write-back is skipped while not READY.
TEST_CASE_METHOD(XMLTestFixture,
                 "PanelWidgetManager re-derives auto-placement cleanly after a not-READY rebuild",
                 "[panel_widget][manager][regression]") {
    helix::init_widget_registrations();

    lv_xml_register_component_from_data(
        "test_grid_spy_widget",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");
    lv_xml_register_component_from_data(
        "panel_widget_firmware_restart",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");

    auto override_factory = [](const char* id) -> WidgetFactory {
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        WidgetFactory original = def->factory;
        helix::register_widget_factory(id, [](const std::string&) -> std::unique_ptr<PanelWidget> {
            return std::make_unique<GridSpyWidget>();
        });
        return original;
    };
    WidgetFactory orig_clock = override_factory("clock");
    WidgetFactory orig_shutdown = override_factory("shutdown");

    auto* cfg = Config::get_instance();
    auto& mgr = PanelWidgetManager::instance();
    auto* conn = lv_xml_get_subject(nullptr, "printer_connection_state");
    auto* klippy = lv_xml_get_subject(nullptr, "klippy_state");
    lv_subject_set_int(conn, static_cast<int>(ConnectionState::CONNECTED));

    auto make_cfg = []() {
        return nlohmann::json{{"main_page_index", 0},
                              {"next_page_id", 2},
                              {"pages",
                               {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                                {{"id", "spy"},
                                 {"widgets",
                                  {{{"id", "clock"},
                                    {"enabled", true},
                                    {"col", 0},
                                    {"row", 0},
                                    {"colspan", 1},
                                    {"rowspan", 1}},
                                   {{"id", "shutdown"},
                                    {"enabled", true},
                                    {"col", -1},
                                    {"row", -1},
                                    {"colspan", 1},
                                    {"rowspan", 1}}}}}}}};
    };

    // Returns the persisted {col,row} of `shutdown` after running the given
    // populate sequence on a freshly-seeded panel.
    auto persisted_shutdown = [&](const std::string& panel_id,
                                  const std::vector<KlippyState>& states) -> std::pair<int, int> {
        const std::string panel_path = cfg->df() + "panel_widgets/" + panel_id;
        cfg->set<nlohmann::json>(panel_path, make_cfg());
        mgr.get_widget_config(panel_id).mark_dirty();
        mgr.clear_panel_config(panel_id);

        lv_obj_t* container = lv_obj_create(test_screen());
        lv_obj_set_size(container, 400, 300);
        process_lvgl(10);

        // Run each populate on the SAME cached config (no mark_dirty between) —
        // this is the live gate-observer rebuild path.
        for (KlippyState s : states) {
            lv_subject_set_int(klippy, static_cast<int>(s));
            // Clear only the render caches (active_configs_/grid_descriptors_) so
            // populate rebuilds — but keep the loaded PanelWidgetConfig, exactly
            // like the gate-observer rebuild that does NOT reload from disk.
            mgr.clear_panel_config(panel_id);
            mgr.populate_widgets(panel_id, container, /*page_index=*/1);
        }

        nlohmann::json after = cfg->get<nlohmann::json>(panel_path, nlohmann::json());
        std::pair<int, int> pos{-1, -1};
        if (after.contains("pages") && after["pages"].size() > 1) {
            for (const auto& w : after["pages"][1]["widgets"]) {
                if (w.value("id", "") == "shutdown") {
                    pos = {w.value("col", -1), w.value("row", -1)};
                }
            }
        }
        return pos;
    };

    // Clean READY-only placement.
    auto clean = persisted_shutdown("test_ready_only", {KlippyState::READY});
    // not-READY rebuild, then READY rebuild on the same cached config.
    auto recovered =
        persisted_shutdown("test_transient_recover", {KlippyState::SHUTDOWN, KlippyState::READY});

    INFO("clean=(" << clean.first << "," << clean.second << ") recovered=(" << recovered.first
                   << "," << recovered.second << ")");
    REQUIRE(clean.first >= 0);
    REQUIRE(clean.second >= 0);
    REQUIRE(recovered == clean);

    helix::register_widget_factory("clock", orig_clock);
    helix::register_widget_factory("shutdown", orig_shutdown);
    mgr.clear_panel_config("test_ready_only");
    mgr.clear_panel_config("test_transient_recover");
}

// Companion to the not-READY test above: the not-READY guard must NOT suppress
// the legitimate persistence path. When Klipper IS READY (no firmware_restart
// injected), populate_widgets() must still write back and persist auto-placed
// positions so a newly added/auto-placed widget keeps its slot across reloads.
TEST_CASE_METHOD(XMLTestFixture,
                 "PanelWidgetManager persists auto-placed positions while Klipper is READY",
                 "[panel_widget][manager][regression]") {
    helix::init_widget_registrations();

    lv_xml_register_component_from_data(
        "test_grid_spy_widget",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");

    auto override_factory = [](const char* id) -> WidgetFactory {
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        WidgetFactory original = def->factory;
        helix::register_widget_factory(id, [](const std::string&) -> std::unique_ptr<PanelWidget> {
            return std::make_unique<GridSpyWidget>();
        });
        return original;
    };
    WidgetFactory orig_clock = override_factory("clock");
    WidgetFactory orig_shutdown = override_factory("shutdown");

    // Connected AND Klipper READY -> no firmware_restart injection.
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_connection_state"),
                       static_cast<int>(ConnectionState::CONNECTED));
    lv_subject_set_int(lv_xml_get_subject(nullptr, "klippy_state"),
                       static_cast<int>(KlippyState::READY));

    const std::string panel_id = "test_klippy_ready_persist";

    auto* cfg = Config::get_instance();
    nlohmann::json widget_cfg = {{"main_page_index", 0},
                                 {"next_page_id", 2},
                                 {"pages",
                                  {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                                   {{"id", "spy"},
                                    {"widgets",
                                     {{{"id", "clock"},
                                       {"enabled", true},
                                       {"col", 0},
                                       {"row", 0},
                                       {"colspan", 1},
                                       {"rowspan", 1}},
                                      {{"id", "shutdown"},
                                       {"enabled", true},
                                       {"col", -1},
                                       {"row", -1},
                                       {"colspan", 1},
                                       {"rowspan", 1}}}}}}}};
    const std::string panel_path = cfg->df() + "panel_widgets/" + panel_id;
    cfg->set<nlohmann::json>(panel_path, widget_cfg);

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 300);
    process_lvgl(10);

    auto widgets = mgr.populate_widgets(panel_id, container, /*page_index=*/1);

    // The READY populate auto-places `shutdown` and persists its position.
    nlohmann::json after = cfg->get<nlohmann::json>(panel_path, nlohmann::json());
    INFO("after: " << after.dump());
    REQUIRE(after.contains("pages"));
    const auto& spy_widgets = after["pages"][1]["widgets"];
    bool shutdown_persisted = false;
    for (const auto& w : spy_widgets) {
        if (w.value("id", "") == "shutdown") {
            shutdown_persisted = w.value("col", -1) >= 0 && w.value("row", -1) >= 0;
        }
    }
    REQUIRE(shutdown_persisted);

    helix::register_widget_factory("clock", orig_clock);
    helix::register_widget_factory("shutdown", orig_shutdown);
    mgr.clear_panel_config(panel_id);
}

// The set of visible widgets must not depend on klippy_state. Nothing observes
// that subject for panel rebuilds any more, so a widget that DID vary with it
// would change on screen only when some unrelated gate happened to fire.
TEST_CASE_METHOD(XMLTestFixture, "Visible widget ids do not depend on klippy_state",
                 "[panel_widget][manager]") {
    helix::init_widget_registrations();

    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_connection_state"),
                       static_cast<int>(ConnectionState::CONNECTED));

    const std::string panel_id = "test_klippy_independent_ids";
    auto& mgr = PanelWidgetManager::instance();

    lv_subject_set_int(lv_xml_get_subject(nullptr, "klippy_state"),
                       static_cast<int>(KlippyState::READY));
    const std::vector<std::string> when_ready = mgr.compute_visible_widget_ids(panel_id);

    lv_subject_set_int(lv_xml_get_subject(nullptr, "klippy_state"),
                       static_cast<int>(KlippyState::SHUTDOWN));
    const std::vector<std::string> when_shutdown = mgr.compute_visible_widget_ids(panel_id);

    // Non-empty, or this compares two empty vectors and proves nothing.
    REQUIRE_FALSE(when_ready.empty());
    CHECK(when_ready == when_shutdown);

    mgr.clear_panel_config(panel_id);
}
