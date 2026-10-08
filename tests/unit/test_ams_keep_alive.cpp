// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_keep_alive.cpp
 * @brief The AMS panel stays built between opens, and drops only what it must.
 *
 * Building the panel is most of an open's cost on slow hardware, so a close keeps
 * the widget tree and frees only the path canvas buffer. A theme change, a slot
 * count change and a printer switch each still reach the cached tree.
 */

#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_filament_path_canvas.h"
#include "ui_nav_manager.h"
#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_panel_test_access.h"
#include "../test_helpers/draw_buf_alloc_spy.h"
#include "../test_helpers/memory_monitor_test_access.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "display_settings_manager.h"
#include "memory_monitor.h"
#include "src/ui/ui_filament_path_internal.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <algorithm>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// NavigationManager seeded the way the app has it, animations off, an AFC mock
/// backend, and the AMS panel opened and closed the way a user does.
class AmsKeepAliveFixture : public LVGLUITestFixture {
  public:
    AmsKeepAliveFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);
        helix::ui::destroy_static_panels();

        lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = lv_obj_create(test_screen());
        panels[static_cast<int>(PanelId::Controls)] = lv_obj_create(test_screen());
        NavigationManager::instance().set_panels(panels);

        backend_ = use_backend(4);
    }

    ~AmsKeepAliveFixture() override {
        helix::ui::destroy_static_panels();
        drain();
        AmsState::instance().set_backend(nullptr);
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    AmsBackendMock* use_backend(int slots, bool multi_unit = false) {
        auto mock = std::make_unique<AmsBackendMock>(slots);
        mock->set_afc_mode(true);
        mock->set_operation_delay(0);
        if (multi_unit) {
            mock->set_multi_unit_mode(true);
        }
        REQUIRE(mock->start().success());
        auto* raw = mock.get();
        AmsState::instance().set_backend(std::move(mock));
        AmsState::instance().init_subjects(true);
        AmsState::instance().sync_from_backend();
        return raw;
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain();
    }

    void open() {
        navigate_to_ams_panel();
        drain();
        process_lvgl(50);
    }

    void close() {
        NavigationManager::instance().go_back();
        drain();
        process_lvgl(50);
    }

    lv_obj_t* panel_obj() const {
        AmsPanel* p = get_existing_ams_panel();
        return p ? p->get_panel() : nullptr;
    }

    lv_obj_t* path_canvas() const {
        lv_obj_t* root = panel_obj();
        return root ? lv_obj_find_by_name(root, "path_canvas") : nullptr;
    }

    lv_obj_t* first_slot() const {
        lv_obj_t* root = panel_obj();
        lv_obj_t* grid = root ? lv_obj_find_by_name(root, "slot_grid") : nullptr;
        return grid ? lv_obj_get_child(grid, 0) : nullptr;
    }

    AmsBackendMock* backend_ = nullptr;
    bool animations_were_enabled_ = true;
};

void flag_on_delete(lv_obj_t* obj, bool* flag) {
    lv_obj_add_event_cb(
        obj, [](lv_event_t* e) { *static_cast<bool*>(lv_event_get_user_data(e)) = true; },
        LV_EVENT_DELETE, flag);
}

} // namespace

TEST_CASE_METHOD(AmsKeepAliveFixture, "AmsPanel: a second open reuses the built panel",
                 "[ams][keep_alive]") {
    open();
    lv_obj_t* root = panel_obj();
    REQUIRE(root != nullptr);
    lv_obj_t* slot = first_slot();
    REQUIRE(slot != nullptr);
    // Static: the slot's delete hook can fire in the fixture teardown, after the test frame is
    // gone.
    static bool slot_deleted;
    slot_deleted = false;
    flag_on_delete(slot, &slot_deleted);

    close();
    REQUIRE(lv_obj_is_valid(root));

    open();
    CHECK(panel_obj() == root);
    CHECK(helix::nav::is_showing(root));
    CHECK_FALSE(slot_deleted);
}

TEST_CASE_METHOD(AmsKeepAliveFixture,
                 "AmsPanel: closing frees the path buffer and reopening allocates it once",
                 "[ams][keep_alive][canvas_buffer]") {
    BufAllocSpy spy;
    open();
    auto* data = ui::fpath::get_data(path_canvas());
    REQUIRE(data != nullptr);
    REQUIRE(data->layers.overlay_buf != nullptr);
    const size_t path_bytes = data->layers.overlay_buf->data_size;
    // The overlay push also snapshots the screen into a buffer of its own.
    auto path_allocations = [&] {
        return std::count(spy.sizes.begin(), spy.sizes.end(), path_bytes);
    };
    const auto before = path_allocations();

    close();
    CHECK(data->layers.overlay_buf == nullptr);

    open();
    REQUIRE(data->layers.overlay_buf != nullptr);
    CHECK(path_allocations() == before + 1);
}

TEST_CASE_METHOD(AmsKeepAliveFixture, "AmsPanel: a theme change while closed shows new colours",
                 "[ams][keep_alive]") {
    open();
    auto* stale = ui::fpath::get_data(path_canvas());
    REQUIRE(stale != nullptr);
    close();

    // Colours the canvas loaded under a theme that is about to be replaced.
    stale->theme.color_idle = lv_color_hex(0x123456);
    theme_manager_notify_change();

    open();
    auto* data = ui::fpath::get_data(path_canvas());
    REQUIRE(data != nullptr);
    const bool dark = theme_manager_is_dark_mode();
    CHECK(lv_color_to_u32(data->theme.color_idle) ==
          lv_color_to_u32(
              theme_manager_get_color(dark ? "filament_idle_dark" : "filament_idle_light")));
}

TEST_CASE_METHOD(AmsKeepAliveFixture,
                 "AmsPanel: a slot count change while closed rebuilds the slots once, on open",
                 "[ams][keep_alive]") {
    open();
    lv_obj_t* slot = first_slot();
    REQUIRE(slot != nullptr);
    // Static: the slot's delete hook can fire in the fixture teardown, after the test frame is
    // gone.
    static bool slot_deleted;
    slot_deleted = false;
    flag_on_delete(slot, &slot_deleted);
    close();

    use_backend(6);
    process_lvgl(100);
    CHECK_FALSE(slot_deleted);

    open();
    CHECK(slot_deleted);
    lv_obj_t* grid = lv_obj_find_by_name(panel_obj(), "slot_grid");
    REQUIRE(grid != nullptr);
    CHECK(lv_obj_get_child_count(grid) == 6);
}

TEST_CASE_METHOD(AmsKeepAliveFixture, "AmsPanel: a printer switch still drops the cached panel",
                 "[ams][keep_alive]") {
    open();
    close();
    lv_obj_t* root = panel_obj();
    REQUIRE(root != nullptr);

    // The registry teardown is what a printer switch runs. It hands the widget
    // tree to its caller to free; what matters here is that the panel lets go.
    helix::ui::destroy_static_panels();
    CHECK(get_existing_ams_panel() == nullptr);
    CHECK(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

    open();
    CHECK(panel_obj() != root);
}

TEST_CASE_METHOD(AmsKeepAliveFixture,
                 "AmsPanel: a hot reload drops the closed panel and leaves an open one",
                 "[ams][keep_alive]") {
    open();
    AmsPanel* panel = get_existing_ams_panel();
    REQUIRE(panel != nullptr);
    lv_obj_t* root = panel_obj();

    // On screen: the reload leaves it alone.
    CHECK_FALSE(panel->rebuild());
    CHECK(panel_obj() == root);

    // Closed: the cached tree goes, and the next open builds from the new XML.
    close();
    CHECK(panel->rebuild());
    CHECK(panel_obj() == nullptr);
    process_lvgl(50);

    open();
    CHECK(panel_obj() != nullptr);
    CHECK(helix::nav::is_showing(panel_obj()));
}

TEST_CASE_METHOD(AmsKeepAliveFixture, "AmsPanel: a navbar switch away deactivates and closes it",
                 "[ams][keep_alive]") {
    open();
    AmsPanel* panel = get_existing_ams_panel();
    REQUIRE(panel != nullptr);
    REQUIRE(panel->holds_poll_ref_);
    REQUIRE(AmsPanelTestAccess::is_open(*panel));
    lv_obj_t* canvas = path_canvas();
    ui_filament_path_canvas_set_error_segment(canvas, 3);
    REQUIRE(ui_filament_path_canvas_is_animating(canvas));

    NavigationManagerTestAccess::switch_to_panel(NavigationManager::instance(), PanelId::Controls);
    drain();
    process_lvgl(50);

    CHECK_FALSE(panel->holds_poll_ref_);
    CHECK_FALSE(AmsPanelTestAccess::is_open(*panel));
    CHECK_FALSE(ui_filament_path_canvas_is_animating(canvas));
}

TEST_CASE_METHOD(AmsKeepAliveFixture, "AmsPanel: a closed panel does not act on AMS state",
                 "[ams][keep_alive]") {
    open();
    AmsPanel* panel = get_existing_ams_panel();
    REQUIRE(panel != nullptr);
    auto* path = ui::fpath::get_data(path_canvas());
    REQUIRE(path != nullptr);
    close();

    // Its operation handlers and stall watchdog go with the sidebar.
    CHECK_FALSE(AmsPanelTestAccess::has_sidebar(*panel));

    SECTION("an AMS fault raises no AMS dialog over another screen") {
        backend_->simulate_error(AmsResult::FILAMENT_JAM);
        AmsState::instance().sync_from_backend();
        process_lvgl(100);
        CHECK_FALSE(panel->is_error_modal_visible());
    }

    SECTION("a tool change waits for the next open") {
        const int before = path->active_slot;
        const int target = before == 2 ? 1 : 2;
        REQUIRE(backend_->select_slot(target).success());
        AmsState::instance().sync_from_backend();
        process_lvgl(100);
        CHECK(path->active_slot == before);

        open();
        CHECK(path->active_slot == target);
        CHECK(AmsPanelTestAccess::has_sidebar(*panel));
    }
}

TEST_CASE_METHOD(AmsKeepAliveFixture,
                 "AmsOverviewPanel: a closed overview does not refresh its cards",
                 "[ams][keep_alive]") {
    backend_ = use_backend(4, true);
    open();
    auto* overview = helix::lazy_global_if_exists<AmsOverviewPanel>();
    REQUIRE(overview != nullptr);
    close();
    const int refreshes = AmsPanelTestAccess::units_refreshes(*overview);

    lv_subject_t* version = AmsState::instance().get_slots_version_subject();
    lv_subject_set_int(version, lv_subject_get_int(version) + 1);
    process_lvgl(100);
    CHECK(AmsPanelTestAccess::units_refreshes(*overview) == refreshes);

    open();
    CHECK(AmsPanelTestAccess::units_refreshes(*overview) > refreshes);
}

TEST_CASE_METHOD(AmsKeepAliveFixture,
                 "AMS: opening the other unit-count panel drops the hidden one",
                 "[ams][keep_alive]") {
    open();
    close();
    REQUIRE(panel_obj() != nullptr);

    backend_ = use_backend(4, true);
    open();
    CHECK(panel_obj() == nullptr);
    auto* overview = helix::lazy_global_if_exists<AmsOverviewPanel>();
    REQUIRE(overview != nullptr);
    CHECK(helix::nav::is_showing(overview->get_panel()));
}

TEST_CASE_METHOD(AmsKeepAliveFixture, "AMS: critical memory pressure drops the hidden panel",
                 "[ams][keep_alive]") {
    open();
    close();
    REQUIRE(panel_obj() != nullptr);

    auto& monitor = MemoryMonitor::instance();
    MemoryMonitorTestAccess::fire_warning(monitor, MemoryPressureLevel::warning, "test",
                                          MemoryStats{}, MemoryInfo{}, 0);
    drain();
    CHECK(panel_obj() != nullptr);

    MemoryMonitorTestAccess::fire_warning(monitor, MemoryPressureLevel::critical, "test",
                                          MemoryStats{}, MemoryInfo{}, 0);
    drain();
    CHECK(panel_obj() == nullptr);
}

TEST_CASE_METHOD(AmsKeepAliveFixture, "AMS: critical memory pressure leaves an open panel",
                 "[ams][keep_alive]") {
    open();
    MemoryMonitorTestAccess::fire_warning(MemoryMonitor::instance(), MemoryPressureLevel::critical,
                                          "test", MemoryStats{}, MemoryInfo{}, 0);
    drain();
    CHECK(panel_obj() != nullptr);
    CHECK(helix::nav::is_showing(panel_obj()));
}

TEST_CASE_METHOD(AmsKeepAliveFixture, "AmsPanel: reopening keeps one operation stepper",
                 "[ams][keep_alive]") {
    auto steppers = [&] {
        lv_obj_t* box = lv_obj_find_by_name(panel_obj(), "progress_stepper_container");
        REQUIRE(box != nullptr);
        return lv_obj_get_child_count(box);
    };
    auto start_load = [&] {
        auto* sidebar = AmsPanelTestAccess::sidebar(*get_existing_ams_panel());
        REQUIRE(sidebar != nullptr);
        sidebar->start_operation(StepOperationType::LOAD_FRESH, 0);
        process_lvgl(30);
    };
    open();
    start_load();
    REQUIRE(steppers() == 1);
    for (int i = 0; i < 2; ++i) {
        close();
        open();
        start_load();
        CHECK(steppers() == 1);
    }
}

TEST_CASE_METHOD(AmsKeepAliveFixture, "AmsPanel: a stale close callback re-arms the next close",
                 "[ams][keep_alive]") {
    open();
    AmsPanel* panel = get_existing_ams_panel();
    REQUIRE(panel != nullptr);

    // A close callback from a slide-out that a reopen overtook: it is consumed
    // when it runs, and runs while the panel is showing again.
    helix::nav::clear_on_close(panel_obj());
    AmsPanel::run_close();
    REQUIRE(AmsPanelTestAccess::is_open(*panel));

    close();
    CHECK_FALSE(AmsPanelTestAccess::is_open(*panel));
}
