// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_home_image_changed_subject_lifetime.cpp
 * @brief HomePanel's image_changed observer must carry
 *        PrinterImageManager's SubjectLifetime, or its guard dereferences an
 *        observer node that deinit_subjects() freed.
 *
 * PrinterImageManager::deinit_subjects() runs lv_subject_deinit() on
 * image_changed_subject_, which frees every observer node on it. It does not
 * bump the ObserverGuard invalidation epoch (only
 * StaticSubjectRegistry::deinit_all() is paired with that), so the epoch
 * defence never trips for it. The only protection for an outside observer is
 * the death token handed out by PrinterImageManager::get_subjects_lifetime().
 * HomePanel attaches its observer in the constructor and resets the guard only
 * at member destruction — after deinit_subjects() has already run — so without
 * the token that reset calls lv_observer_remove() on a freed node.
 */

#include "ui_panel_home.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "printer_image_manager.h"

#include <lvgl.h>
#include <memory>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

class HomeImageChangedLifetimeFixture : public LVGLUITestFixture {
  public:
    HomeImageChangedLifetimeFixture() {
        // init() is idempotent and only initializes the subject once; it must
        // have run before the baseline is taken.
        helix::PrinterImageManager::instance().init();

        subject_ = helix::PrinterImageManager::instance().get_image_changed_subject();
        REQUIRE(subject_ != nullptr);
        baseline_observers_ = lv_ll_get_len(&subject_->subs_ll);

        // The observer attaches in HomePanel's constructor; create() is not
        // needed to exercise the teardown path under test.
        panel_ = std::make_unique<HomePanel>(state(), nullptr);
    }

    ~HomeImageChangedLifetimeFixture() override {
        UpdateQueue::instance().drain();
        panel_.reset();
        UpdateQueue::instance().drain();
        // deinit_subjects() in a test body frees the observers of every
        // process-lifetime panel; re-initialize so the next test sees a live
        // manager.
        helix::PrinterImageManager::instance().init();
    }

    /// Destroy the panel the way StaticPanelRegistry::destroy_all() does.
    void panel_release() {
        UpdateQueue::instance().drain();
        panel_.reset();
    }

  protected:
    lv_subject_t* subject_ = nullptr;
    size_t baseline_observers_ = 0;

  private:
    std::unique_ptr<HomePanel> panel_;
};

} // namespace

TEST_CASE_METHOD(
    HomeImageChangedLifetimeFixture,
    "HomePanel image_changed observer survives manager deinit without touching freed nodes",
    "[observer][raii][crash_hardening][home_panel]") {
    // The panel must have actually attached an observer to the subject this
    // test is about to deinit, or the rest passes vacuously.
    REQUIRE(lv_ll_get_len(&subject_->subs_ll) > baseline_observers_);

    // The death signal observers rely on: flipped by deinit_subjects() while
    // every shared_ptr copy is still held.
    auto lifetime = helix::PrinterImageManager::instance().get_subjects_lifetime();
    REQUIRE(lifetime != nullptr);
    REQUIRE(*lifetime);

    // The teardown path under test: frees every observer node on the subject
    // WITHOUT bumping the ObserverGuard invalidation epoch.
    helix::PrinterImageManager::instance().deinit_subjects();
    CHECK_FALSE(*lifetime);
    CHECK(lv_ll_get_len(&subject_->subs_ll) == 0);

    // Destroying the panel resets its guard. With the death token the reset
    // skips lv_observer_remove(); without it this is a double free of a node
    // lv_subject_deinit() already freed.
    panel_release();

    // The subject storage lives in the manager singleton, so the pointer is
    // still valid here: re-init and notify through it. No observer may be
    // left attached from the destroyed panel.
    helix::PrinterImageManager::instance().init();
    lv_subject_set_int(subject_, lv_subject_get_int(subject_) + 1);
    UpdateQueue::instance().drain();
    CHECK(lv_ll_get_len(&subject_->subs_ll) == 0);
}

TEST_CASE_METHOD(HomeImageChangedLifetimeFixture,
                 "HomePanel image_changed observer is removed when the panel dies after an "
                 "invalidation epoch bump",
                 "[observer][raii][crash_hardening][home_panel]") {
    REQUIRE(lv_ll_get_len(&subject_->subs_ll) > baseline_observers_);

    // A teardown elsewhere bumps the process-global epoch; this subject is not freed by it.
    ObserverGuard::invalidate_all();
    panel_release();

    // Skipped removal would leave the dead panel's observer on the live subject.
    CHECK(lv_ll_get_len(&subject_->subs_ll) == baseline_observers_);
    lv_subject_set_int(subject_, lv_subject_get_int(subject_) + 1);
    UpdateQueue::instance().drain();
}
