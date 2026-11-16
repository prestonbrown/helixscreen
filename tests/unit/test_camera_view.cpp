// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_camera_view.cpp
 * @brief CameraView's stream teardown and its one-client-per-camera rule
 */

#include "lvgl.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

#if HELIX_HAS_CAMERA
#include "../lvgl_test_fixture.h"
#include "../test_helpers/camera_stream_test_access.h"
#include "../test_helpers/camera_view_test_access.h"
#include "app_globals.h"
#include "camera_view.h"
#include "core/http_executor.h"
#include "core/update_queue.h"
#include "printer_state.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

using namespace helix;

namespace {

constexpr const char* kStreamUrl = "http://127.0.0.1:9/stream";

/// A printer whose one webcam refuses connections, so a stream starts and
/// stops without a camera.
class CameraViewFixture : public LVGLTestFixture {
  public:
    CameraViewFixture() {
        get_printer_state().capabilities_state().set_webcam_available(true, kStreamUrl, "");
        drain();
    }
    ~CameraViewFixture() override {
        get_printer_state().capabilities_state().set_webcam_available(false);
        drain();
    }
    static void drain() {
        helix::ui::UpdateQueue::instance().drain();
    }
    static WebcamInfo feed() {
        WebcamInfo f;
        f.stream_url = kStreamUrl;
        return f;
    }
};

bool wait_until_true(const std::atomic<bool>& flag, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return flag.load();
}

} // namespace

TEST_CASE_METHOD(CameraViewFixture,
                 "CameraView: a start after the worker gave up clears the image first",
                 "[camera][camera_view]") {
    CameraView view;
    auto dead = std::make_unique<CameraStream>();
    CameraStreamTestAccess::spawn_worker_to_exhaustion(*dead);
    for (int i = 0; i < 1000 && dead->is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE_FALSE(dead->is_running());
    CameraViewTestAccess::adopt(view, std::move(dead), feed());
    LifetimeToken old_token = view.stream_token();

    // The image still shows a frame of the stream that gave up.
    lv_draw_buf_t* frame = lv_draw_buf_create(4, 4, LV_COLOR_FORMAT_RGB888, 0);
    lv_obj_t* image = lv_image_create(lv_screen_active());
    lv_image_set_src(image, frame);
    REQUIRE(lv_image_get_src(image) == frame);

    REQUIRE(view.start("", nlohmann::json::object(), {image}, nullptr, nullptr));
    CHECK(lv_image_get_src(image) == nullptr);
    CHECK(old_token.expired());

    view.stop({image});
    lv_obj_delete(image);
    lv_draw_buf_destroy(frame);
    drain();
}

TEST_CASE_METHOD(CameraViewFixture, "CameraView: stop hands a wedged join off the main thread",
                 "[camera][camera_view]") {
    auto& pool = http::HttpExecutor::fast();
    const bool pool_was_running = pool.running();
    pool.start();

    auto release = std::make_shared<std::atomic<bool>>(false);
    auto exited = std::make_shared<std::atomic<bool>>(false);
    {
        CameraView view;
        auto wedged = std::make_unique<CameraStream>();
        CameraStreamTestAccess::set_join_timeout(*wedged, std::chrono::milliseconds(3000));
        CameraStreamTestAccess::spawn_wedged_worker(*wedged, release, exited);
        CameraViewTestAccess::adopt(view, std::move(wedged), feed());

        const auto before = std::chrono::steady_clock::now();
        view.stop({});
        const auto took = std::chrono::steady_clock::now() - before;
        CHECK(took < std::chrono::milliseconds(1000));
        CHECK_FALSE(view.has_stream());
    }

    release->store(true);
    REQUIRE(wait_until_true(*exited));
    // Let the pool finish reaping before anything else uses it.
    for (int i = 0; i < 1000 && pool.inflight() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!pool_was_running) {
        pool.stop();
    }
    drain();
}

TEST_CASE_METHOD(CameraViewFixture,
                 "CameraView: a reap job dropped unrun still leaks a detached stream",
                 "[camera][camera_view]") {
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto exited = std::make_shared<std::atomic<bool>>(false);
    auto wedged = std::make_unique<CameraStream>();
    CameraStreamTestAccess::set_join_timeout(*wedged, std::chrono::milliseconds(50));
    CameraStreamTestAccess::spawn_wedged_worker(*wedged, release, exited);
    const int leaked_before = CameraViewTestAccess::leaked_streams();

    {
        // What a stopping pool does with a queued job: destroys it unrun.
        auto owner = CameraViewTestAccess::reaping_owner(std::move(wedged));
        std::function<void()> job = [owner]() {};
        owner.reset();
    }

    // The join timed out and detached the worker, so the stream must be
    // leaked rather than freed under it.
    CHECK(CameraViewTestAccess::leaked_streams() == leaked_before + 1);

    release->store(true);
    REQUIRE(wait_until_true(*exited));
    drain();
}

TEST_CASE_METHOD(CameraViewFixture, "CameraView: starting a camera stops other views of it",
                 "[camera][camera_view]") {
    CameraView first;
    CameraView second;
    int first_preempted = 0;
    int second_preempted = 0;
    first.on_preempted([&]() {
        ++first_preempted;
        first.stop({});
    });
    second.on_preempted([&]() {
        ++second_preempted;
        second.stop({});
    });

    REQUIRE(first.start("", nlohmann::json::object(), {}, nullptr, nullptr));
    CHECK(first_preempted == 0);

    REQUIRE(second.start("", nlohmann::json::object(), {}, nullptr, nullptr));
    CHECK(first_preempted == 1);
    CHECK_FALSE(first.has_stream());
    CHECK(second.has_stream());
    CHECK(second_preempted == 0);

    second.stop({});
    drain();
}

#endif // HELIX_HAS_CAMERA
