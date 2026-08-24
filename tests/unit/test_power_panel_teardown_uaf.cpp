// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_power_panel_teardown_uaf.cpp
 * @brief PowerPanel must not touch its widget tree after that tree is deleted
 *
 * PowerPanel caches raw pointers into a widget tree whose lifetime it does not
 * own, and both of its rebuild producers (populate_device_list() from the
 * get_power_devices reply, handle_chip_clicked() from a chip tap) hand the
 * rebuild to the UpdateQueue. AsyncLifetimeGuard guards `this`; nothing guarded
 * the tree. Deleting the tree between "queued" and "drained" left every cached
 * pointer dangling and the null checks satisfied, so the deferred rebuild ran
 * safe_clean_children() and create_led_chip() on freed memory (#776 family).
 *
 * These tests reproduce that ordering directly: populate, delete the tree, then
 * drain.
 */

#include "ui_panel_power.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/power_panel_test_access.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "moonraker_types.h"
#include "printer_state.h"

#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

std::vector<PowerDevice> sample_devices() {
    return {
        PowerDevice{"printer", "gpio", "on", true},
        PowerDevice{"led_strip", "gpio", "off", false},
        PowerDevice{"enclosure_fan", "gpio", "on", false},
    };
}

/// Builds a real PowerPanel over the real power_panel XML tree.
///
/// api is null on purpose: setup() calls fetch_devices(), and a null API makes
/// that a logged no-op so the test drives populate_device_list() itself with a
/// deterministic device list instead of racing a network reply.
class PowerPanelTeardownFixture : public LVGLUITestFixture {
  public:
    PowerPanelTeardownFixture() {
        panel_ = std::make_unique<PowerPanel>(state(), nullptr);
        panel_->init_subjects();

        root_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "power_panel", nullptr));
        REQUIRE(root_ != nullptr);

        panel_->setup(root_, test_screen());
        // get_or_create_overlay() would have cached the root; seed it directly so
        // the test does not also depend on NavigationManager registration.
        PowerPanelTestAccess::set_cached_overlay(*panel_, root_);

        // setup() must have found every widget this panel caches, or the test
        // would pass for the wrong reason (null pointers are already safe).
        REQUIRE(PowerPanelTestAccess::chip_container(*panel_) != nullptr);
        REQUIRE(PowerPanelTestAccess::device_list_container(*panel_) != nullptr);
        REQUIRE(PowerPanelTestAccess::empty_state_container(*panel_) != nullptr);
    }

    ~PowerPanelTeardownFixture() override {
        // Drain before the fixture tears LVGL down so no queued lambda outlives
        // the panel. Tests that leave the tree standing destroy the panel first,
        // mirroring destroy_all()-before-lv_deinit(); ~PowerPanel() uninstalls its
        // delete hook so the base fixture's screen teardown cannot reach it.
        // reset() on an already-destroyed panel is a no-op.
        UpdateQueue::instance().drain();
        panel_.reset();
    }

    PowerPanel& panel() {
        return *panel_;
    }
    lv_obj_t* root() {
        return root_;
    }

    /// Destroy the panel while its widget tree is still alive, the way
    /// StaticPanelRegistry::destroy_all() does before lv_deinit().
    void destroy_panel() {
        UpdateQueue::instance().drain();
        panel_.reset();
    }

    /// Delete the panel's widget tree the way a screen teardown would: the
    /// panel gets no call, only LVGL's own delete event.
    void delete_widget_tree() {
        lv_obj_delete(root_);
        root_ = nullptr;
    }

  private:
    std::unique_ptr<PowerPanel> panel_;
    lv_obj_t* root_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(PowerPanelTeardownFixture,
                 "PowerPanel drops cached widget pointers when its tree is deleted",
                 "[power][teardown][uaf]") {
    PowerPanelTestAccess::populate_device_list(panel(), sample_devices());
    REQUIRE(PowerPanelTestAccess::device_row_count(panel()) == 3);

    delete_widget_tree();

    CHECK(PowerPanelTestAccess::chip_container(panel()) == nullptr);
    CHECK(PowerPanelTestAccess::device_list_container(panel()) == nullptr);
    CHECK(PowerPanelTestAccess::empty_state_container(panel()) == nullptr);
    CHECK(PowerPanelTestAccess::cached_overlay(panel()) == nullptr);
    CHECK(PowerPanelTestAccess::device_row_count(panel()) == 0);
}

TEST_CASE_METHOD(PowerPanelTeardownFixture,
                 "PowerPanel chip rebuild queued before teardown survives the drain",
                 "[power][teardown][uaf]") {
    // populate_device_list() queues the chip rebuild via lifetime_.defer().
    PowerPanelTestAccess::populate_device_list(panel(), sample_devices());

    // Tree dies while the rebuild is still sitting in the queue.
    delete_widget_tree();

    // Pre-fix this drain ran populate_device_chips_impl() on the freed
    // container: safe_clean_children() -> lv_obj_update_layout(), then
    // create_led_chip() -> lv_button_create() -> lv_obj_get_screen(). SIGSEGV.
    UpdateQueue::instance().drain();

    CHECK(PowerPanelTestAccess::chip_container(panel()) == nullptr);
}

TEST_CASE_METHOD(PowerPanelTeardownFixture,
                 "PowerPanel chip tap queued before teardown survives the drain",
                 "[power][teardown][uaf]") {
    PowerPanelTestAccess::populate_device_list(panel(), sample_devices());
    UpdateQueue::instance().drain(); // settle the initial build

    // The second producer: a chip tap re-queues the rebuild.
    PowerPanelTestAccess::handle_chip_clicked(panel(), "led_strip");

    delete_widget_tree();
    UpdateQueue::instance().drain();

    CHECK(PowerPanelTestAccess::chip_container(panel()) == nullptr);
}

TEST_CASE_METHOD(PowerPanelTeardownFixture,
                 "PowerPanel device-list reply after teardown is a no-op",
                 "[power][teardown][uaf]") {
    delete_widget_tree();

    // populate_device_list() is what the get_power_devices reply defers to. It
    // dereferences device_list_container_/empty_state_container_ and runs
    // clear_device_list() over device_rows_ — all stale after the delete.
    PowerPanelTestAccess::populate_device_list(panel(), sample_devices());
    UpdateQueue::instance().drain();

    // No rows can be built without a container to build them into.
    CHECK(PowerPanelTestAccess::device_row_count(panel()) == 0);
}

// ============================================================================
// Row teardown must not delete synchronously inside an UpdateQueue batch
// ============================================================================
//
// clear_device_list() runs from populate_device_list(), whose only production
// caller is the deferred get_power_devices reply. Deleting N rows synchronously
// there puts N sync deletions in one process_pending() batch, which corrupts
// LVGL's global event linked list (#776/#190/#80) — THREADING.md invariant 3
// bans exactly this. The deletions have to route through LVGL's own async list.

namespace {

int g_row_deletes = 0;

void count_row_delete(lv_event_t* /*e*/) {
    ++g_row_deletes;
}

/// Tag every current child of a container so we can tell when it really dies.
void watch_deletes(lv_obj_t* container) {
    g_row_deletes = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
        lv_obj_add_event_cb(lv_obj_get_child(container, i), count_row_delete, LV_EVENT_DELETE,
                            nullptr);
    }
}

} // namespace

TEST_CASE_METHOD(PowerPanelTeardownFixture,
                 "PowerPanel row teardown defers deletion out of the queue batch",
                 "[power][teardown][uaf]") {
    PowerPanelTestAccess::populate_device_list(panel(), sample_devices());
    UpdateQueue::instance().drain();
    process_lvgl(50); // settle the chip rebuild the first populate queued

    lv_obj_t* list = PowerPanelTestAccess::device_list_container(panel());
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 3);
    watch_deletes(list);

    // Re-populate from inside a batch, exactly as the get_power_devices reply
    // does. clear_device_list() tears down the three old rows in that batch.
    queue_update("test::power_repopulate", [this]() {
        PowerPanelTestAccess::populate_device_list(panel(),
                                                   {PowerDevice{"printer", "gpio", "on", false}});
    });
    UpdateQueue::instance().drain();

    // The rows must be detached and handed to LVGL's async list, not freed
    // inside the batch we just drained.
    CHECK(g_row_deletes == 0);
    CHECK(lv_obj_get_child_count(list) == 1);

    // LVGL's own async pass is where they actually die.
    process_lvgl(50);
    CHECK(g_row_deletes == 3);
}

namespace {

/// Is on_panel_deleted still installed on `obj` for this panel instance?
///
/// Reaches into LVGL's event list rather than inferring from behaviour: the
/// whole point is that the hook must be gone *before* anything fires it, and a
/// behavioural probe would have to trigger the very use-after-free under test.
bool delete_hook_installed(lv_obj_t* obj, const void* panel) {
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
        if (dsc != nullptr && lv_event_dsc_get_user_data(dsc) == panel) {
            return true;
        }
    }
    return false;
}

} // namespace

// StaticPanelRegistry::destroy_all() runs before lv_deinit(), so in production
// the panel is destroyed while its widget tree is still alive. If ~PowerPanel()
// leaves its LV_EVENT_DELETE hook installed, lv_deinit()'s later teardown of
// that tree calls on_panel_deleted() on freed memory.
TEST_CASE_METHOD(PowerPanelTeardownFixture,
                 "PowerPanel uninstalls its delete hook when destroyed before its tree",
                 "[power][teardown][uaf]") {
    PowerPanelTestAccess::populate_device_list(panel(), sample_devices());
    UpdateQueue::instance().drain();
    process_lvgl(50);

    lv_obj_t* tree = root();
    REQUIRE(tree != nullptr);
    // Guards against the test passing for the wrong reason: if setup() never
    // installed the hook, its absence after destruction would prove nothing.
    REQUIRE(delete_hook_installed(tree, &panel()));

    const void* dead = &panel();
    destroy_panel();

    CHECK_FALSE(delete_hook_installed(tree, dead));

    // The tree outlives the panel exactly as it does under lv_deinit(). With the
    // hook still installed this dereferences freed memory — a crash in a plain
    // build, a heap-use-after-free report under ASAN/TSAN.
    delete_widget_tree();
    process_lvgl(50);
    SUCCEED("widget tree torn down after the panel without touching freed memory");
}
