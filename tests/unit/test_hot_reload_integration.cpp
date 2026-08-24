// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_hot_reload_integration.cpp
 * @brief Integration test: rebuild_active_views() actually swaps panel widget pointers
 *
 * Guards against regression of the "panels are hidden-not-destroyed" bug that made
 * XML hot-reload a silent no-op. If this test fails, somebody reverted
 * NavigationManager::rebuild_active_views or PanelBase::rebuild.
 *
 * The test:
 *   1. Registers a minimal in-memory XML stub component ("fake_rebuild_panel").
 *   2. Constructs a FakePanel that refers to that component via get_xml_component_name().
 *   3. Creates an initial widget via lv_xml_create, registers it with NavigationManager
 *      as PanelId::Home, and hooks up the FakePanel as that panel's lifecycle instance.
 *   4. Calls NavigationManager::rebuild_active_views().
 *   5. Drains the UpdateQueue so any deferred deletions/callbacks fire.
 *   6. Asserts that NavigationManager::get_panel_widget(Home) points at a *different*
 *      widget than it did before the rebuild — proving the rebuild actually tore down
 *      and re-created the tree rather than leaving the old pointer in place.
 *
 * We do not use LVGLUITestFixture because this test only exercises the swap logic.
 * MoonrakerTestFixture gives us LVGL + PrinterState + MoonrakerAPI with no singletons
 * to re-initialize per-test.
 */

#include "ui_nav_manager.h"
#include "ui_panel_base.h"
#include "ui_update_queue.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "test_fixtures.h"

#include "../catch_amalgamated.hpp"

namespace {

/// Minimal PanelBase subclass whose XML component is a bare-bones stub.
/// Does not touch printer state or API — just enough to exercise rebuild().
class FakePanel : public PanelBase {
  public:
    FakePanel(helix::PrinterState& state, MoonrakerAPI* api, const char* component_name)
        : PanelBase(state, api), component_name_(component_name) {
        subjects_initialized_ = true; // suppress the "setup before init_subjects" warning
    }

    void init_subjects() override {}

    const char* get_name() const override {
        return "FakePanel";
    }

    const char* get_xml_component_name() const override {
        return component_name_;
    }

    void on_activate() override {
        ++activate_count_;
    }

    void on_deactivate() override {
        ++deactivate_count_;
    }

    int activate_count_ = 0;
    int deactivate_count_ = 0;

  private:
    const char* component_name_;
};

/// Register a minimal XML stub once per test-run. lv_xml_register_component_from_data
/// is idempotent enough for our purposes — if it's already registered (from a prior
/// test run in the same process), re-registering returns OK.
void ensure_fake_component_registered(const char* name) {
    lv_xml_register_component_from_data(name,
                                        "<component>"
                                        "<view extends=\"lv_obj\" width=\"100\" height=\"100\"/>"
                                        "</component>");
}

} // anonymous namespace

TEST_CASE_METHOD(MoonrakerTestFixture, "rebuild_active_views swaps active panel widget pointer",
                 "[hot-reload][integration]") {
    // 1. Register stub component
    constexpr const char* COMPONENT = "fake_rebuild_panel";
    ensure_fake_component_registered(COMPONENT);

    // 2. Initialize NavigationManager subjects (required before set_panels/rebuild).
    //    init() is idempotent via its subjects_initialized_ guard.
    auto& nav = NavigationManager::instance();
    nav.init();

    // 3. Build the initial widget for PanelId::Home on the test screen.
    lv_obj_t* parent = test_screen();
    REQUIRE(parent != nullptr);

    lv_obj_t* initial_widget = static_cast<lv_obj_t*>(lv_xml_create(parent, COMPONENT, nullptr));
    REQUIRE(initial_widget != nullptr);

    // 4. Register widget array with NavigationManager. Only Home is populated —
    //    other slots are nullptr, which is fine for this test.
    lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
    panels[static_cast<int>(helix::PanelId::Home)] = initial_widget;
    nav.set_panels(panels);

    // 5. Construct FakePanel and register with NavigationManager.
    FakePanel fake_panel(state(), &api(), COMPONENT);
    fake_panel.setup(initial_widget, parent);
    nav.register_panel_instance(helix::PanelId::Home, &fake_panel);

    // 6. Verify initial state: Home is active, widget pointer matches what we set.
    REQUIRE(nav.get_active() == helix::PanelId::Home);
    lv_obj_t* before = nav.get_panel_widget(helix::PanelId::Home);
    REQUIRE(before == initial_widget);

    // 7. Call rebuild_active_views — this should tear down `initial_widget` and
    //    create a fresh one via lv_xml_create, updating NavigationManager's cache.
    nav.rebuild_active_views();

    // 8. Drain queued callbacks (safe_delete_deferred uses lv_async_call which is
    //    processed by lv_timer_handler). Process LVGL briefly to run deferred
    //    deletions; this doesn't affect the widget pointer swap, which happens
    //    synchronously inside rebuild_active_views.
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);

    // 9. The swapped pointer must differ from the original.
    lv_obj_t* after = nav.get_panel_widget(helix::PanelId::Home);
    REQUIRE(after != nullptr);
    REQUIRE(after != before);

    // 10. Bonus: confirm the PanelBase was tracked through the rebuild — its
    //     panel_ member should now point at the new widget, not the deleted one.
    REQUIRE(fake_panel.get_panel() == after);

    // 11. Verify lifecycle calls fired: on_deactivate() for old, on_activate() for new
    //     (since initial widget was visible, rebuild re-activates the new widget).
    REQUIRE(fake_panel.deactivate_count_ >= 1);
    REQUIRE(fake_panel.activate_count_ >= 1);

    // 12. Cleanup: unregister our FakePanel so it doesn't outlive the stack frame
    //     while NavigationManager still holds the pointer. The test fixture will
    //     tear down LVGL after we return, but clearing the slot is cheap insurance.
    nav.register_panel_instance(helix::PanelId::Home, nullptr);

    // Clear panel_widgets_ entry too — nav holds a dangling pointer after our
    // local `fake_panel` is destroyed and LVGL tears down.
    lv_obj_t* null_panels[UI_PANEL_COUNT] = {nullptr};
    nav.set_panels(null_panels);

    // Drain any deferred deletions queued by the rebuild teardown.
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);

    // Reset NavigationManager subjects so the singleton doesn't hold dangling
    // observers into the test's LVGL display after teardown. Without this,
    // global destructors end up calling lv_observer_remove on freed subjects
    // and segfault at process exit. Mirrors NavigationTestFixture's ~dtor.
    nav.deinit_subjects();
}
