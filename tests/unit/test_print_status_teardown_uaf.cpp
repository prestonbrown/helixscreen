// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_teardown_uaf.cpp
 * @brief PrintStatusPanel must not touch its widget tree after that tree is deleted
 *
 * PrintStatusPanel caches raw pointers into a widget tree whose lifetime it does
 * not own, and every PrinterState observer it registers lands as a queued
 * observe<int> lambda. The TSan nightly (08-30 and 08-31) died in exactly
 * that gap: a queued print-start-progress apply drained from a fixture
 * destructor AFTER lv_obj_delete(root) had freed the tree, so
 * on_print_start_progress_changed() ran lv_bar_set_value() on the freed
 * preparing bar.
 *
 * The pointers were only being dropped on the explicit destroy_overlay_ui()
 * path (on_ui_destroyed()). A raw lv_obj_delete gives the panel no call — the
 * same hole PowerPanel had (#776 family), closed there with an LV_EVENT_DELETE
 * hook on the panel root. These tests pin the same contract here: populate,
 * delete the tree, then drain.
 */

#include "ui_panel_print_status.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_status_panel_fixture.h"
#include "ams_state.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "filament_sensor_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "test_helpers/print_status_panel_test_access.h"

#include <lvgl.h>
#include <memory>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;
using print_status_panel_test::PrintStatusPanelFixture;

namespace {

/// Is the panel's delete hook still installed on `obj` for this panel instance?
///
/// Reaches into LVGL's event list rather than inferring from behaviour: the
/// point is that the hook must be gone *before* anything can fire it, and a
/// behavioural probe would have to trigger the very use-after-free under test.
/// Same probe as test_power_panel_teardown_uaf.cpp.
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

TEST_CASE_METHOD(PrintStatusPanelFixture,
                 "PrintStatusPanel drops cached widget pointers when its tree is deleted",
                 "[print_status][teardown][uaf]") {
    // The tree must have actually populated the pointers under test, or the
    // null checks below would pass for the wrong reason.
    lv_obj_t* bar = PrintStatusPanelTestAccess::preparing_progress_widget(panel());
    REQUIRE(bar != nullptr);
    REQUIRE(panel().get_panel() != nullptr);

    delete_widget_tree();

    CHECK(PrintStatusPanelTestAccess::preparing_progress_widget(panel()) == nullptr);
    CHECK(panel().get_panel() == nullptr);
}

TEST_CASE_METHOD(
    PrintStatusPanelFixture,
    "PrintStatusPanel queued print-start progress survives a drain after the tree dies",
    "[print_status][teardown][uaf]") {
    REQUIRE(PrintStatusPanelTestAccess::preparing_progress_widget(panel()) != nullptr);

    // A print-start-progress change fires the panel's observe<int> observer
    // synchronously; the handler itself is queued. This is the pending lambda
    // the nightly drained after the tree was already gone.
    lv_subject_set_int(state().print_state().get_print_start_progress_subject(), 42);

    // Tree dies while the handler is still sitting in the queue.
    delete_widget_tree();

    // Pre-fix this ran on_print_start_progress_changed() against the freed
    // preparing bar: lv_bar_set_value() on freed memory. SIGSEGV.
    UpdateQueue::instance().drain();

    // The handler must have RUN, not been skipped: it still publishes its own
    // subject, which has no widget subscribers left to notify.
    CHECK(PrintStatusPanelTestAccess::preparing_progress_subject_value(panel()) == 42);
    CHECK(PrintStatusPanelTestAccess::preparing_progress_widget(panel()) == nullptr);
}

// StaticPanelRegistry::destroy_all() runs before lv_deinit(), so in production
// the panel can be destroyed while its widget tree is still alive — the tree is
// then deleted with the panel already freed. If ~PrintStatusPanel() leaves its
// LV_EVENT_DELETE hook installed, that teardown calls on_root_deleted() on
// freed memory.
TEST_CASE_METHOD(PrintStatusPanelFixture,
                 "PrintStatusPanel uninstalls its delete hook when destroyed before its tree",
                 "[print_status][teardown][uaf]") {
    lv_obj_t* tree = panel().get_panel();
    REQUIRE(tree != nullptr);
    // Guards against the test passing for the wrong reason: if create() never
    // installed the hook, its absence after destruction would prove nothing.
    REQUIRE(delete_hook_installed(tree, &panel()));

    const void* dead = &panel();
    UpdateQueue::instance().drain();
    panel_release();

    CHECK_FALSE(delete_hook_installed(tree, dead));

    // The tree outlives the panel exactly as it does under lv_deinit(). With
    // the hook still installed this dereferences freed memory.
    lv_obj_delete(tree);
    UpdateQueue::instance().drain();
    SUCCEED("widget tree torn down after the panel without touching freed memory");
}

// The production memory-reclaim path: destroy_overlay_ui() detaches the tree,
// nulls the panel's pointers, and queues the actual deletion — and the panel can
// be destroyed before that deferred delete executes (StaticPanelRegistry order).
// The delete hook must come off THERE too, or the deferred delete reaches a
// freed panel, and the pointers must drop on this path exactly as they do on a
// raw delete.
TEST_CASE_METHOD(PrintStatusPanelFixture,
                 "PrintStatusPanel explicit teardown drops pointers and disarms the hook",
                 "[print_status][teardown][uaf]") {
    lv_obj_t* tree = panel().get_panel();
    REQUIRE(tree != nullptr);
    REQUIRE(delete_hook_installed(tree, &panel()));

    lv_obj_t* cached = tree;
    panel().destroy_overlay_ui(cached);

    // Deletion is deferred: the tree is detached but still allocated, so these
    // probe it directly rather than inferring from behaviour.
    CHECK(cached == nullptr);
    CHECK_FALSE(delete_hook_installed(tree, &panel()));
    CHECK(panel().get_panel() == nullptr);
    CHECK(PrintStatusPanelTestAccess::preparing_progress_widget(panel()) == nullptr);

    // Panel dies before the deferred delete runs; the async pass then tears the
    // tree down with the hook already gone.
    panel_release();
    process_lvgl(50);
    SUCCEED("explicit teardown left nothing armed against the panel");
}

// OverlayBase::rebuild() calls create() again while the replaced root is still
// alive, and only then hands it to safe_delete_subtree() — detached
// synchronously, deleted async. If create() installs the hook on the successor
// without taking it off the predecessor, no uninstall path can ever reach the
// old one again: the destructor and on_ui_destroyed() both work from
// delete_hook_root_, which by then names the successor. A hot-reload rebuild
// followed by shutdown before the async delete tick then fires on_root_deleted()
// through a freed `this`.
TEST_CASE_METHOD(PrintStatusPanelFixture,
                 "PrintStatusPanel moves its delete hook off the root it replaces",
                 "[print_status][teardown][uaf]") {
    lv_obj_t* old_root = panel().get_panel();
    REQUIRE(old_root != nullptr);
    // Guards against the test passing for the wrong reason: if create() never
    // installed the hook, its absence after the rebuild would prove nothing.
    REQUIRE(delete_hook_installed(old_root, &panel()));

    lv_obj_t* new_root = panel().create(test_screen());
    REQUIRE(new_root != nullptr);
    REQUIRE(new_root != old_root);

    CHECK_FALSE(delete_hook_installed(old_root, &panel()));
    CHECK(delete_hook_installed(new_root, &panel()));

    // The failure the stale hook produces: the panel dies with the replaced
    // tree still allocated, and nothing took the hook off it.
    const void* dead = &panel();
    panel_release();
    CHECK_FALSE(delete_hook_installed(old_root, dead));

    lv_obj_delete(old_root);
    lv_obj_delete(new_root);
    root_ = nullptr;
    UpdateQueue::instance().drain();
    SUCCEED("replaced root torn down after the panel without touching freed memory");
}

// OverlayBase::rebuild() deletes the replaced root after overlay_root_ already
// points at the successor. That late delete event must not blank the successor's
// cached pointers.
TEST_CASE_METHOD(PrintStatusPanelFixture,
                 "PrintStatusPanel ignores a replaced root's late delete event",
                 "[print_status][teardown][uaf]") {
    lv_obj_t* old_root = panel().get_panel();
    REQUIRE(old_root != nullptr);

    lv_obj_t* new_root = panel().create(test_screen());
    REQUIRE(new_root != nullptr);
    REQUIRE(new_root != old_root);
    REQUIRE(panel().get_panel() == new_root);

    lv_obj_delete(old_root);
    UpdateQueue::instance().drain();

    CHECK(panel().get_panel() == new_root);
    CHECK(PrintStatusPanelTestAccess::preparing_progress_widget(panel()) != nullptr);

    lv_obj_delete(new_root);
    UpdateQueue::instance().drain();
    root_ = nullptr;
}

// PrintStatusPanel observes subjects owned by two other singletons. Each of
// them frees every observer node it holds in its own deinit_subjects(), so the
// panel's guards have to carry that owner's SubjectLifetime: without it a guard
// stays truthy with a dangling lv_observer_t* and its reset() removes an
// observer that no longer exists. The panel is a process-global, so any test
// that tears one of those singletons down leaves the trap armed for whoever
// destroys the panel next.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrintStatusPanel guards survive an owner singleton's subject teardown",
                 "[print_status][teardown][uaf][observer]") {
    // Both singletons are process-globals whose subjects every later test in
    // this binary resolves by XML name, and deinit_subjects() withdraws those
    // names. Restoring from a destructor, not from the end of the test body,
    // means a failed CHECK or a throw between here and there cannot leave the
    // rest of the shard with a nameless AmsState. AmsState::deinit_subjects()
    // also runs clear_backends(); zero registered backends is the state
    // RegisteredBackend leaves behind, so that side of it needs no restoring.
    struct SubjectOwnerRestore {
        ~SubjectOwnerRestore() {
            helix::AmsState::instance().init_subjects(true);
            helix::FilamentSensorManager::instance().init_subjects();
        }
    } restore_owner_subjects;

    helix::AmsState::instance().init_subjects(true);
    helix::FilamentSensorManager::instance().init_subjects();

    // The lookups the panel performs must resolve, or it attaches nothing and
    // the teardown below would be trivially safe.
    REQUIRE(lv_xml_get_subject(nullptr, "ams_slot_count") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "toolchange_visible") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "filament_sensor_count") != nullptr);

    auto panel = std::make_unique<PrintStatusPanel>(state(), nullptr);
    panel->init_subjects();

    // Frees the observer nodes the panel just attached to both singletons.
    helix::AmsState::instance().deinit_subjects();
    helix::FilamentSensorManager::instance().deinit_subjects();

    // Every guard reset() lands here. Each one that never learned its subject
    // died calls lv_observer_remove() on a freed node.
    panel->deinit_subjects();
    panel.reset();
    UpdateQueue::instance().drain();

    SUCCEED("panel teardown completed without walking freed observer nodes");
}

// Any PrintStatusPanel registers its subjects' teardown with the registry, and that
// teardown names the process-global panel. Run after the global is gone, or when only a
// standalone panel ever existed, it must not construct one: a panel built inside
// destroy_all() outlives the teardown, keeps live observers, and opens the tune overlay
// on the next z-offset change.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrintStatusPanel subject teardown never builds the global panel",
                 "[print_status][teardown][lifecycle]") {
    StaticPanelRegistry::instance().destroy_all();
    REQUIRE(helix::lazy_global_if_exists<PrintStatusPanel>() == nullptr);
    {
        PrintStatusPanel panel(state(), nullptr);
        panel.init_subjects();
        panel.deinit_subjects();
    }
    REQUIRE(StaticPanelRegistry::instance().count() > 0);

    StaticPanelRegistry::instance().destroy_all();
    UpdateQueue::instance().drain();
    CHECK(helix::lazy_global_if_exists<PrintStatusPanel>() == nullptr);
    CHECK(StaticPanelRegistry::instance().count() == 0);
}
