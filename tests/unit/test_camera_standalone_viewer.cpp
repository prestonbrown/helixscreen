// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_camera_standalone_viewer.cpp
 * @brief The fullscreen camera viewer opened with no owning CameraWidget
 *
 * Its stream is a CameraView's: it starts through one, preempts other views of
 * the same camera, and closes through CameraView's teardown, so a wedged join
 * never runs on the main thread. With a CameraWidget attached it opens that
 * widget's fullscreen instead of a second connection.
 */

#include "lvgl.h"

#include "../catch_amalgamated.hpp"

#if HELIX_HAS_CAMERA

#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/camera_stream_test_access.h"
#include "../test_helpers/camera_view_test_access.h"
#include "../test_helpers/camera_widget_test_access.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "app_globals.h"
#include "camera_view.h"
#include "display_settings_manager.h"
#include "http_executor.h"
#include "printer_state.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace helix;

namespace {

constexpr const char* kStreamUrl = "http://127.0.0.1:9/stream";

/// A printer whose one webcam refuses connections, and a navigation stack
/// seeded the way the app seeds it, so the viewer pushes and closes as it
/// does in the app without a camera.
class StandaloneViewerFixture : public LVGLUITestFixture {
  public:
    StandaloneViewerFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);
        home_ = lv_obj_create(test_screen());
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_;
        NavigationManager::instance().set_panels(panels);
        get_printer_state().capabilities_state().set_webcam_available(true, kStreamUrl, "");
        drain();
    }
    ~StandaloneViewerFixture() override {
        if (CameraWidgetTestAccess::standalone_view()) {
            close_viewer();
        }
        NavigationManagerTestAccess::set_panel_stack(NavigationManager::instance(), {home_});
        get_printer_state().capabilities_state().set_webcam_available(false);
        drain();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }
    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    void close_viewer() {
        helix::nav::go_back();
        drain();
        process_lvgl(20);
        drain();
    }

  private:
    lv_obj_t* home_ = nullptr;
    bool animations_were_enabled_ = true;
};

bool wait_until_true(const std::atomic<bool>& flag, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return flag.load();
}

} // namespace

TEST_CASE_METHOD(StandaloneViewerFixture,
                 "Standalone camera viewer streams through a CameraView, one at a time",
                 "[camera][camera_view][standalone_viewer]") {
    REQUIRE(CameraWidgetTestAccess::standalone_view() == nullptr);
    REQUIRE(open_standalone_camera_fullscreen(test_screen()));
    drain();

    CameraView* view = CameraWidgetTestAccess::standalone_view();
    REQUIRE(view != nullptr);
    CHECK(view->has_stream());
    CHECK(view->feed().stream_url == kStreamUrl);
    CHECK(CameraWidgetTestAccess::standalone_overlay() != nullptr);

    CHECK_FALSE(open_standalone_camera_fullscreen(test_screen()));

    close_viewer();
    CHECK(CameraWidgetTestAccess::standalone_view() == nullptr);
}

TEST_CASE_METHOD(StandaloneViewerFixture,
                 "Standalone camera viewer close hands a wedged join off the main thread",
                 "[camera][camera_view][standalone_viewer]") {
    auto& pool = http::HttpExecutor::fast();
    const bool pool_was_running = pool.running();
    pool.start();

    REQUIRE(open_standalone_camera_fullscreen(test_screen()));
    drain();
    CameraView* view = CameraWidgetTestAccess::standalone_view();
    REQUIRE(view != nullptr);

    // Swap the refused stream for one whose join would block for seconds.
    view->stop({});
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto exited = std::make_shared<std::atomic<bool>>(false);
    auto wedged = std::make_unique<CameraStream>();
    CameraStreamTestAccess::set_join_timeout(*wedged, std::chrono::milliseconds(3000));
    CameraStreamTestAccess::spawn_wedged_worker(*wedged, release, exited);
    WebcamInfo feed;
    feed.stream_url = kStreamUrl;
    CameraViewTestAccess::adopt(*view, std::move(wedged), feed);
    REQUIRE(view->has_stream());

    const auto before = std::chrono::steady_clock::now();
    close_viewer();
    const auto took = std::chrono::steady_clock::now() - before;
    CHECK(CameraWidgetTestAccess::standalone_view() == nullptr);
    CHECK(took < std::chrono::milliseconds(1000));
    CHECK_FALSE(exited->load());

    release->store(true);
    REQUIRE(wait_until_true(*exited));
    for (int i = 0; i < 1000 && pool.inflight() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!pool_was_running) {
        pool.stop();
    }
    drain();
}

TEST_CASE_METHOD(StandaloneViewerFixture,
                 "Standalone camera viewer takes the camera from another view of it",
                 "[camera][camera_view][standalone_viewer]") {
    CameraView card;
    int preempted = 0;
    card.on_preempted([&]() {
        ++preempted;
        card.stop({});
    });
    REQUIRE(card.start("", nlohmann::json::object(), {}, nullptr, nullptr));

    REQUIRE(open_standalone_camera_fullscreen(test_screen()));
    drain();
    CHECK(preempted == 1);
    CHECK_FALSE(card.has_stream());
    CameraView* view = CameraWidgetTestAccess::standalone_view();
    REQUIRE(view != nullptr);
    CHECK(view->has_stream());

    close_viewer();
    // Closed, the camera is free for the other view's next activation.
    REQUIRE(card.start("", nlohmann::json::object(), {}, nullptr, nullptr));
    CHECK(preempted == 1);
    card.stop({});
    drain();
}

TEST_CASE_METHOD(StandaloneViewerFixture,
                 "Standalone camera viewer hands off to an attached widget's fullscreen",
                 "[camera][camera_view][standalone_viewer]") {
    lv_obj_t* tile = lv_obj_create(test_screen());
    {
        CameraWidget widget;
        widget.attach(tile, test_screen());

        REQUIRE(open_standalone_camera_fullscreen(test_screen()));
        drain();
        CHECK(CameraWidgetTestAccess::standalone_view() == nullptr);
        CHECK(CameraWidgetTestAccess::fullscreen_overlay(widget) != nullptr);
        CHECK(CameraWidgetTestAccess::has_stream(widget));

        widget.detach();
    }
    lv_obj_delete(tile);
    drain();
}

#endif // HELIX_HAS_CAMERA
