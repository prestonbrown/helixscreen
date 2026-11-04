// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plr_offer_subject_lifetime.cpp
 * @brief PlrOfferController's PrinterState-subject observers must carry
 *        PrinterState's SubjectLifetime, or their guards dereference observer
 *        nodes that PrinterState::deinit_subjects() freed.
 *
 * PrinterState::deinit_subjects() runs lv_subject_deinit() on every subject it
 * reaches (its own and every per-domain component's, print_domain_ included),
 * which frees each observer node. It does not bump the ObserverGuard
 * invalidation epoch (only StaticSubjectRegistry::deinit_all() is paired with
 * that), so the epoch defence never trips for it. The only protection for an
 * outside observer is the death token handed out by
 * PrinterState::get_subjects_lifetime(). An observer created without it calls
 * lv_observer_remove() on a freed node when the controller is destroyed, a
 * double free of that node.
 */

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "plr_offer_controller.h"
#include "printer_state.h"

#include <lvgl.h>
#include <memory>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

class PlrOfferLifetimeFixture : public LVGLUITestFixture {
  public:
    PlrOfferLifetimeFixture() {
        // Publish the print-domain subject names first: the controller reads
        // pl_env_valid and printer_connection_state through PrinterState's
        // accessors, which need initialized subjects. init_subjects() is
        // idempotent on re-entry.
        auto& ps = get_printer_state();
        ps.init_subjects();

        subject_ = ps.print_state().get_plr_snapshot_valid_subject();
        REQUIRE(subject_ != nullptr);
        baseline_observers_ = lv_ll_get_len(&subject_->subs_ll);

        controller_ = std::make_unique<helix::ui::PlrOfferController>();
        // observe<int> defers its registration fire through UpdateQueue;
        // drain so the queued handler runs inside the test, not after it.
        UpdateQueue::instance().drain();
    }

    ~PlrOfferLifetimeFixture() override {
        UpdateQueue::instance().drain();
        controller_.reset();
        UpdateQueue::instance().drain();
        // deinit_subjects() in a test body withdraws the XML names and frees
        // the observers of everything watching PrinterState's subjects;
        // re-initialize so the next test sees a live PrinterState.
        get_printer_state().init_subjects();
    }

    /// Destroy the controller the way SubjectInitializer teardown does.
    void controller_release() {
        UpdateQueue::instance().drain();
        controller_.reset();
    }

  protected:
    lv_subject_t* subject_ = nullptr;
    size_t baseline_observers_ = 0;

  private:
    std::unique_ptr<helix::ui::PlrOfferController> controller_;
};

} // namespace

TEST_CASE_METHOD(
    PlrOfferLifetimeFixture,
    "PlrOfferController observers survive PrinterState deinit without touching freed nodes",
    "[observer][raii][crash_hardening][plr]") {
    // The controller must have actually attached an observer to the subject
    // this test is about to deinit, or the rest passes vacuously.
    REQUIRE(lv_ll_get_len(&subject_->subs_ll) > baseline_observers_);

    // The death signal observers rely on: flipped by deinit_subjects() while
    // every shared_ptr copy is still held.
    auto lifetime = get_printer_state().get_subjects_lifetime();
    REQUIRE(lifetime != nullptr);
    REQUIRE(*lifetime);

    // The teardown path under test: frees every observer node on
    // PrinterState's subjects WITHOUT bumping the ObserverGuard epoch.
    get_printer_state().deinit_subjects();
    CHECK_FALSE(*lifetime);
    CHECK(lv_ll_get_len(&subject_->subs_ll) == 0);

    // Destroying the controller resets its guards. With the death token the
    // reset skips lv_observer_remove(); without it this is a double free of a
    // node lv_subject_deinit() already freed.
    controller_release();

    // The subject storage lives in the PrinterState singleton, so the pointer
    // is still valid here: re-init and notify through it. No observer may be
    // left attached from the destroyed controller.
    get_printer_state().init_subjects();
    lv_subject_set_int(subject_, 1);
    UpdateQueue::instance().drain();
    CHECK(lv_ll_get_len(&subject_->subs_ll) == 0);
}
