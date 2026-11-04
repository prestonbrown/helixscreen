// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_home_printer_image_observer.cpp
 * @brief HomePanel's printer-image refresh is live in the order the app builds it.
 *
 * A session initializes its core subjects, then constructs the panels (HomePanel
 * observes PrinterImageManager's image_changed subject from its constructor), and only
 * then runs the session services. A printer switch deinits every subject and repeats
 * that order, so the subject the panel observes has to come up with the core subjects.
 *
 * Run with: ./build/bin/helix-tests "[home_panel][printer_image]"
 */

#include "ui_panel_home.h"

#include "../../lvgl_ui_test_fixture.h"
#include "../../test_helpers/home_panel_test_access.h"
#include "../../test_helpers/subject_initializer_test_access.h"
#include "core/update_queue.h"
#include "printer_image_manager.h"

#include <lvgl.h>
#include <memory>

#include "../../catch_amalgamated.hpp"

namespace {

class HomeImageObserverFixture : public LVGLUITestFixture {
  public:
    ~HomeImageObserverFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        panel_.reset();
        helix::ui::UpdateQueue::instance().drain();
        helix::PrinterImageManager::instance().init();
    }

    /// One session's build, from a torn-down image manager: core subjects, then the panel.
    void build_session() {
        panel_.reset();
        helix::PrinterImageManager::instance().deinit_subjects();
        SubjectInitializer si;
        SubjectInitializerTestAccess::init_core_subjects(si);
        panel_ = std::make_unique<HomePanel>(state(), nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Whether an image change reaches the panel's refresh.
    bool refreshes_on_image_change() {
        auto& cached = HomePanelTestAccess::last_printer_image_path(*panel_);
        cached = "stale";
        helix::PrinterImageManager::instance().notify_image_changed();
        helix::ui::UpdateQueue::instance().drain();
        return cached.empty();
    }

  private:
    std::unique_ptr<HomePanel> panel_;
};

} // namespace

TEST_CASE_METHOD(HomeImageObserverFixture,
                 "HomePanel built after the core subjects refreshes on an image change",
                 "[home_panel][printer_image]") {
    build_session();
    CHECK(refreshes_on_image_change());
}

TEST_CASE_METHOD(HomeImageObserverFixture,
                 "HomePanel rebuilt for the next printer refreshes on an image change",
                 "[home_panel][printer_image][printer_switch]") {
    build_session();
    REQUIRE(refreshes_on_image_change());
    build_session();
    CHECK(refreshes_on_image_change());
}
