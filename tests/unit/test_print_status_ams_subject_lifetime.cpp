// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_ams_subject_lifetime.cpp
 * @brief PrintStatusPanel's AMS-subject observers must carry AmsState's
 *        SubjectLifetime, or their guards dereference observer nodes that
 *        AmsState::deinit_subjects() freed.
 *
 * AmsState::deinit_subjects() runs lv_subject_deinit() on every subject it
 * owns, which frees each observer node. It does not bump the
 * ObserverGuard invalidation epoch (only StaticSubjectRegistry::deinit_all()
 * is paired with that), so the epoch defence never trips for it. The only
 * protection for an outside observer is the death token handed out by
 * AmsState::get_subjects_lifetime(). An observer created without it — the
 * guards fetch their subject by XML name, so there is no token'd accessor to
 * route through — calls lv_observer_remove() on a freed node when the panel
 * is destroyed, a double free of that node.
 */

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
#include "ams_state.h"
#include "printer_state.h"

#include <lvgl.h>
#include <memory>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

class PrintStatusAmsLifetimeFixture : public LVGLUITestFixture {
  public:
    PrintStatusAmsLifetimeFixture() {
        // Publish the AMS subject names first: the panel resolves
        // "ams_slot_count" by XML name, and a name registered in a scope that
        // has since been torn down resolves to nothing. init_subjects() is
        // idempotent on re-entry.
        AmsState::instance().init_subjects();

        // Process-lifetime panels must be initialized before the measurement
        // baseline is taken, or their observers would land inside the delta
        // this test asserts on.
        auto& global = get_global_print_status_panel();
        if (!global.are_subjects_initialized()) {
            global.init_subjects();
        }

        subject_ = AmsState::instance().get_slot_count_subject();
        REQUIRE(subject_ != nullptr);
        baseline_observers_ = lv_ll_get_len(&subject_->subs_ll);

        panel_ = std::make_unique<PrintStatusPanel>(state(), nullptr);
        panel_->init_subjects();
        root_ = panel_->create(test_screen());
        REQUIRE(root_ != nullptr);
    }

    ~PrintStatusAmsLifetimeFixture() override {
        if (root_ && helix::test::widget_in_tree(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
        panel_.reset();
        UpdateQueue::instance().drain();
        // deinit_subjects() in a test body withdraws the XML names and frees
        // the observers of every process-lifetime panel; re-initialize so the
        // next test sees a live AmsState.
        AmsState::instance().init_subjects();
    }

    /// Destroy the panel the way StaticPanelRegistry::destroy_all() does.
    void panel_release() {
        UpdateQueue::instance().drain();
        panel_.reset();
    }

  protected:
    lv_obj_t* root_ = nullptr;
    lv_subject_t* subject_ = nullptr;
    size_t baseline_observers_ = 0;

  private:
    std::unique_ptr<PrintStatusPanel> panel_;
};

} // namespace

TEST_CASE_METHOD(
    PrintStatusAmsLifetimeFixture,
    "PrintStatusPanel AMS observers survive AmsState deinit without touching freed nodes",
    "[observer][raii][crash_hardening][print_status]") {
    // The panel must have actually attached an observer to the subject this
    // test is about to deinit, or the rest passes vacuously.
    REQUIRE(lv_ll_get_len(&subject_->subs_ll) > baseline_observers_);

    // The death signal observers rely on: flipped by deinit_subjects() while
    // every shared_ptr copy is still held.
    auto lifetime = AmsState::instance().get_subjects_lifetime();
    REQUIRE(lifetime != nullptr);
    REQUIRE(*lifetime);

    // The teardown path under test: frees every observer node on AmsState's
    // subjects WITHOUT bumping the ObserverGuard invalidation epoch.
    AmsState::instance().deinit_subjects();
    CHECK_FALSE(*lifetime);
    CHECK(lv_ll_get_len(&subject_->subs_ll) == 0);

    // Destroying the panel resets its guards. With the death token the reset
    // skips lv_observer_remove(); without it this is a double free of a node
    // lv_subject_deinit() already freed.
    panel_release();

    // The subject storage lives in the AmsState singleton, so the pointer is
    // still valid here: re-init and notify through it. No observer may be
    // left attached from the destroyed panel.
    AmsState::instance().init_subjects();
    lv_subject_set_int(subject_, 4);
    UpdateQueue::instance().drain();
    CHECK(lv_ll_get_len(&subject_->subs_ll) == 0);
}
