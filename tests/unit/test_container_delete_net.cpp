// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_container_delete_net.cpp
 * @brief ContainerDeleteNet must disarm itself when its owner dies
 *
 * The net is a base class four list views inherit (PrintSelectListView,
 * PrintSelectCardView, SpoolmanListView, HistoryListView). All four happen to
 * call cleanup() — and therefore detach_container_net() — from their own
 * destructors, so nothing is broken today. That makes "the net is off before
 * the owner's storage goes" a per-derived convention rather than a guarantee:
 * a view added later that forgets leaves the container holding a callback into
 * a half-destroyed object, and net_on_container_delete() then dispatches
 * on_netted_container_destroyed() through a vtable whose override is already
 * gone — a pure-virtual call, or a plain use-after-free.
 *
 * The sibling abstraction gets this right: ~PanelWidget() calls
 * uninstall_delete_hook() itself and says so in a comment. These tests pin the
 * same contract on the base, with a deliberately forgetful derived view so the
 * only thing that can disarm the net is the base destructor.
 */

#include "ui_container_delete_net.h"

#include "../lvgl_test_fixture.h"

#include <memory>

#include "../catch_amalgamated.hpp"

namespace {

/// A view that uses the net exactly as the four real ones do, except that its
/// destructor does NOT detach — the fifth list view someone adds later.
class ForgetfulNetView : public ContainerDeleteNet {
  public:
    using ContainerDeleteNet::netted_container;
    using ContainerDeleteNet::retarget_container_net;

    int destroyed_calls = 0;

  protected:
    void on_netted_container_destroyed() override {
        destroyed_calls++;
    }
};

/// Is the net still armed on @p obj for @p view?
///
/// Reaches into LVGL's event list rather than inferring from behaviour: the
/// point is that the callback must be gone *before* anything can fire it, and a
/// behavioural probe would have to trigger the very use-after-free under test.
/// Same probe as test_print_status_teardown_uaf.cpp.
bool net_installed(lv_obj_t* obj, const void* view) {
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
        if (dsc != nullptr && lv_event_dsc_get_user_data(dsc) == view) {
            return true;
        }
    }
    return false;
}

} // namespace

// The panel-outlives-tree order: StaticPanelRegistry::destroy_all() runs before
// lv_deinit(), so a view member can be destroyed while the container it netted
// is still alive and only deleted afterwards.
TEST_CASE_METHOD(LVGLTestFixture, "ContainerDeleteNet detaches in its own destructor",
                 "[container_delete_net][teardown][uaf]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    REQUIRE(container != nullptr);

    auto view = std::make_unique<ForgetfulNetView>();
    view->retarget_container_net(container);
    // Guards against passing for the wrong reason: if retarget never armed the
    // net, its absence after destruction would prove nothing.
    REQUIRE(net_installed(container, view.get()));

    const void* dead = view.get();
    view.reset();

    CHECK_FALSE(net_installed(container, dead));

    // The container is torn down after its owner, exactly as it is under
    // lv_deinit(). With the net still armed this dispatches a pure virtual on a
    // destroyed object.
    lv_obj_delete(container);
    SUCCEED("container torn down after the view without firing into freed memory");
}

// The destructor detach must not cost a live owner its notification — the whole
// reason the net exists (#1396).
TEST_CASE_METHOD(LVGLTestFixture, "ContainerDeleteNet still fires for a live owner",
                 "[container_delete_net][teardown]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    ForgetfulNetView view;
    view.retarget_container_net(container);
    REQUIRE(view.netted_container() == container);

    lv_obj_delete(container);

    CHECK(view.destroyed_calls == 1);
    CHECK(view.netted_container() == nullptr);
}
