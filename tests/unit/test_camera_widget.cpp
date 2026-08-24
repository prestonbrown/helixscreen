// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "lvgl.h"
#include "panel_widget.h"
#include "panel_widget_registry.h"

#include "../catch_amalgamated.hpp"

#if HELIX_HAS_CAMERA
#include "ui_update_queue.h"

#include "../test_helpers/camera_stream_test_access.h"
#include "camera_stream.h"

#include <atomic>
#include <chrono>
#include <thread>
#endif

using namespace helix;

// ============================================================================
// Registry: camera widget definition
// ============================================================================

#if HELIX_HAS_CAMERA

TEST_CASE("CameraWidget: registered in widget registry", "[camera][panel_widget]") {
    const auto* def = find_widget_def("camera");
    REQUIRE(def != nullptr);
    REQUIRE(std::string(def->display_name) == "Camera");
    REQUIRE(std::string(def->icon) == "video");
    REQUIRE(def->hardware_gate_subject == nullptr);
    REQUIRE(def->default_enabled == false); // opt-in widget
    REQUIRE(def->colspan == 2);
    REQUIRE(def->rowspan == 2);
    REQUIRE(def->min_colspan == 1);
    REQUIRE(def->min_rowspan == 1);
    REQUIRE(def->max_colspan == 4);
    REQUIRE(def->max_rowspan == 3);
}

// ============================================================================
// MJPEG boundary parser
// ============================================================================

TEST_CASE("CameraStream: construct and destroy without crash", "[camera]") {
    CameraStream stream;
    REQUIRE_FALSE(stream.is_running());
}

TEST_CASE("CameraStream: start with empty URLs does nothing", "[camera]") {
    CameraStream stream;
    bool callback_called = false;
    stream.start("", "", [&](lv_draw_buf_t*) { callback_called = true; });
    REQUIRE_FALSE(stream.is_running());
    REQUIRE_FALSE(callback_called);
}

TEST_CASE("CameraStream: compute_scaled_size picks smallest sufficient scale", "[camera]") {
    CameraStream stream;

    SECTION("no target set returns source dimensions") {
        auto s = stream.compute_scaled_size(1920, 1080);
        CHECK(s.w == 1920);
        CHECK(s.h == 1080);
    }

    SECTION("source already at or below target returns source") {
        stream.set_target_size(1920, 1080);
        auto s = stream.compute_scaled_size(800, 480);
        CHECK(s.w == 800);
        CHECK(s.h == 480);
    }
}

// Decode-time downscaling is gated on tjGetScalingFactors, so a platform with
// no libturbojpeg decodes every frame at full camera resolution and then pays
// for an LVGL software rescale. That was Android until the APK started shipping
// its own libturbojpeg (prestonbrown/helixscreen#1245). The gate is what these
// tests pin down; the table is injected so the result does not depend on
// whether the host running the suite happens to have libturbojpeg installed.
TEST_CASE("CameraStream: decode-time downscale is gated on the turbojpeg scaling table",
          "[camera][1245]") {
    CameraStream stream;

    SECTION("no scaling-factor symbol decodes at full resolution") {
        CameraStreamTestAccess::clear_scaling_factors(stream);
        stream.set_target_size(800, 480);
        auto s = stream.compute_scaled_size(1920, 1080);
        CHECK(s.w == 1920);
        CHECK(s.h == 1080);
    }

    SECTION("smallest factor still covering the target is chosen") {
        CameraStreamTestAccess::install_turbojpeg_scaling_factors(stream);
        stream.set_target_size(800, 480);
        auto s = stream.compute_scaled_size(1920, 1080);
        CHECK(s.w == 960); // 1/2 — 3/8 would be 720x405, under the 800px target
        CHECK(s.h == 540);
    }

    SECTION("a larger target stops at a larger factor") {
        CameraStreamTestAccess::install_turbojpeg_scaling_factors(stream);
        stream.set_target_size(1280, 720);
        auto s = stream.compute_scaled_size(1920, 1080);
        CHECK(s.w == 1440); // 3/4 — 5/8 would be 1200x675, under 1280x720
        CHECK(s.h == 810);
    }

    SECTION("90 degree rotation compares against transposed targets") {
        CameraStreamTestAccess::install_turbojpeg_scaling_factors(stream);
        stream.set_rotation(CameraRotation::Rotate90);
        // Portrait widget: the decoder still sees a landscape frame, so the
        // target must be swapped back into camera space before comparing.
        stream.set_target_size(480, 800);
        auto s = stream.compute_scaled_size(1920, 1080);
        CHECK(s.w == 960);
        CHECK(s.h == 540);
    }

    SECTION("source already below target is never upscaled") {
        CameraStreamTestAccess::install_turbojpeg_scaling_factors(stream);
        stream.set_target_size(1920, 1080);
        auto s = stream.compute_scaled_size(640, 480);
        CHECK(s.w == 640);
        CHECK(s.h == 480);
    }
}

// ============================================================================
// Pixel copy: RGB → LVGL BGR swap
// ============================================================================

TEST_CASE("CameraStream: copy_pixels_rgb_to_lvgl swaps R and B channels", "[camera]") {
    // 2x2 image: known RGB values
    // Pixel (0,0) = R=0xFF G=0x00 B=0x00 (pure red)
    // Pixel (1,0) = R=0x00 G=0xFF B=0x00 (pure green)
    // Pixel (0,1) = R=0x00 G=0x00 B=0xFF (pure blue)
    // Pixel (1,1) = R=0x12 G=0x34 B=0x56
    const int W = 2, H = 2;
    const int stride = W * 3;
    // clang-format off
    uint8_t src[12] = {
        0xFF, 0x00, 0x00,   0x00, 0xFF, 0x00,  // row 0: red, green
        0x00, 0x00, 0xFF,   0x12, 0x34, 0x56,  // row 1: blue, mixed
    };
    // clang-format on
    uint8_t dst[12] = {};

    CameraStream::copy_pixels_rgb_to_lvgl(src, dst, W, H, stride, stride, false, false);

    // After swap: LVGL stores B,G,R
    // Pixel (0,0): was R=FF,G=00,B=00 → stored B=00,G=00,R=FF
    CHECK(dst[0] == 0x00); // B
    CHECK(dst[1] == 0x00); // G
    CHECK(dst[2] == 0xFF); // R

    // Pixel (1,0): was R=00,G=FF,B=00 → stored B=00,G=FF,R=00
    CHECK(dst[3] == 0x00);
    CHECK(dst[4] == 0xFF);
    CHECK(dst[5] == 0x00);

    // Pixel (0,1): was R=00,G=00,B=FF → stored B=FF,G=00,R=00
    CHECK(dst[6] == 0xFF);
    CHECK(dst[7] == 0x00);
    CHECK(dst[8] == 0x00);

    // Pixel (1,1): was R=12,G=34,B=56 → stored B=56,G=34,R=12
    CHECK(dst[9] == 0x56);
    CHECK(dst[10] == 0x34);
    CHECK(dst[11] == 0x12);
}

TEST_CASE("CameraStream: copy_pixels_rgb_to_lvgl with horizontal flip", "[camera]") {
    const int W = 2, H = 1;
    const int stride = W * 3;
    // Row: [R1,G1,B1] [R2,G2,B2] = [0xAA,0xBB,0xCC] [0x11,0x22,0x33]
    uint8_t src[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    uint8_t dst[6] = {};

    CameraStream::copy_pixels_rgb_to_lvgl(src, dst, W, H, stride, stride, true, false);

    // Flip H: pixel order reversed. Plus RGB→BGR swap.
    // dst[0..2] = src pixel 1 swapped: R=0x11,G=0x22,B=0x33 → B=0x33,G=0x22,R=0x11
    CHECK(dst[0] == 0x33);
    CHECK(dst[1] == 0x22);
    CHECK(dst[2] == 0x11);
    // dst[3..5] = src pixel 0 swapped: R=0xAA,G=0xBB,B=0xCC → B=0xCC,G=0xBB,R=0xAA
    CHECK(dst[3] == 0xCC);
    CHECK(dst[4] == 0xBB);
    CHECK(dst[5] == 0xAA);
}

TEST_CASE("CameraStream: copy_pixels_rgb_to_lvgl with vertical flip", "[camera]") {
    const int W = 1, H = 2;
    const int stride = W * 3;
    // Row 0: [0xAA,0xBB,0xCC], Row 1: [0x11,0x22,0x33]
    uint8_t src[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    uint8_t dst[6] = {};

    CameraStream::copy_pixels_rgb_to_lvgl(src, dst, W, H, stride, stride, false, true);

    // Flip V: row order reversed. Plus RGB→BGR swap.
    // dst row 0 = src row 1 swapped: R=0x11,G=0x22,B=0x33 → B=0x33,G=0x22,R=0x11
    CHECK(dst[0] == 0x33);
    CHECK(dst[1] == 0x22);
    CHECK(dst[2] == 0x11);
    // dst row 1 = src row 0 swapped: R=0xAA,G=0xBB,B=0xCC → B=0xCC,G=0xBB,R=0xAA
    CHECK(dst[3] == 0xCC);
    CHECK(dst[4] == 0xBB);
    CHECK(dst[5] == 0xAA);
}

TEST_CASE("CameraStream: copy_pixels_rgb_to_lvgl with both flips", "[camera]") {
    const int W = 2, H = 2;
    const int stride = W * 3;
    // Row 0: [0xAA,0xBB,0xCC] [0x11,0x22,0x33]
    // Row 1: [0x44,0x55,0x66] [0x77,0x88,0x99]
    uint8_t src[12] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    uint8_t dst[12] = {};

    CameraStream::copy_pixels_rgb_to_lvgl(src, dst, W, H, stride, stride, true, true);

    // Flip H+V = 180° rotation. Source pixel layout:
    //   (0,0)=AA,BB,CC  (1,0)=11,22,33
    //   (0,1)=44,55,66  (1,1)=77,88,99
    // After 180° rotation + RGB→BGR swap:
    //   dst(0,0) = src(1,1) swapped = B=99,G=88,R=77
    //   dst(1,0) = src(0,1) swapped = B=66,G=55,R=44
    //   dst(0,1) = src(1,0) swapped = B=33,G=22,R=11
    //   dst(1,1) = src(0,0) swapped = B=CC,G=BB,R=AA
    CHECK(dst[0] == 0x99);
    CHECK(dst[1] == 0x88);
    CHECK(dst[2] == 0x77);
    CHECK(dst[3] == 0x66);
    CHECK(dst[4] == 0x55);
    CHECK(dst[5] == 0x44);
    CHECK(dst[6] == 0x33);
    CHECK(dst[7] == 0x22);
    CHECK(dst[8] == 0x11);
    CHECK(dst[9] == 0xCC);
    CHECK(dst[10] == 0xBB);
    CHECK(dst[11] == 0xAA);
}

// ============================================================================
// Pixel copy: BGR no-swap paths (turbojpeg output)
// ============================================================================

TEST_CASE("CameraStream: copy_pixels_to_lvgl BGR no-swap fast path", "[camera]") {
    const int W = 2, H = 1;
    const int stride = W * 3;
    uint8_t src[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    uint8_t dst[6] = {};

    CameraStream::copy_pixels_to_lvgl(src, dst, W, H, stride, stride, false, false, false);

    // No swap, no flip: straight memcpy
    CHECK(dst[0] == 0xAA);
    CHECK(dst[1] == 0xBB);
    CHECK(dst[2] == 0xCC);
    CHECK(dst[3] == 0x11);
    CHECK(dst[4] == 0x22);
    CHECK(dst[5] == 0x33);
}

TEST_CASE("CameraStream: copy_pixels_to_lvgl BGR with horizontal flip", "[camera]") {
    const int W = 2, H = 1;
    const int stride = W * 3;
    uint8_t src[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    uint8_t dst[6] = {};

    CameraStream::copy_pixels_to_lvgl(src, dst, W, H, stride, stride, true, false, false);

    // Flip H only, no swap: pixel order reversed, channels preserved
    CHECK(dst[0] == 0x11);
    CHECK(dst[1] == 0x22);
    CHECK(dst[2] == 0x33);
    CHECK(dst[3] == 0xAA);
    CHECK(dst[4] == 0xBB);
    CHECK(dst[5] == 0xCC);
}

TEST_CASE("CameraStream: copy_pixels_to_lvgl BGR with vertical flip", "[camera]") {
    const int W = 1, H = 2;
    const int stride = W * 3;
    uint8_t src[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    uint8_t dst[6] = {};

    CameraStream::copy_pixels_to_lvgl(src, dst, W, H, stride, stride, false, true, false);

    // Flip V only, no swap: row order reversed, channels preserved
    CHECK(dst[0] == 0x11);
    CHECK(dst[1] == 0x22);
    CHECK(dst[2] == 0x33);
    CHECK(dst[3] == 0xAA);
    CHECK(dst[4] == 0xBB);
    CHECK(dst[5] == 0xCC);
}

// ============================================================================
// Boundary parsing from Content-Type
// ============================================================================

TEST_CASE("CameraStream: parse_boundary extracts boundary from Content-Type", "[camera]") {
    SECTION("standard boundary") {
        auto b = CameraStream::parse_boundary("multipart/x-mixed-replace;boundary=myboundary");
        CHECK(b == "--myboundary");
    }
    SECTION("boundary with space after semicolon") {
        auto b = CameraStream::parse_boundary("multipart/x-mixed-replace; boundary=frame");
        CHECK(b == "--frame");
    }
    SECTION("quoted boundary") {
        auto b =
            CameraStream::parse_boundary("multipart/x-mixed-replace; boundary=\"someboundary\"");
        CHECK(b == "--someboundary");
    }
    SECTION("boundary already has dashes") {
        auto b = CameraStream::parse_boundary("multipart/x-mixed-replace;boundary=--existing");
        CHECK(b == "--existing");
    }
    SECTION("no boundary parameter") {
        auto b = CameraStream::parse_boundary("image/jpeg");
        CHECK(b.empty());
    }
    SECTION("empty string") {
        auto b = CameraStream::parse_boundary("");
        CHECK(b.empty());
    }
}

// ============================================================================
// Worker-thread liveness — running_ must mean "the worker is alive" (#1245)
// ============================================================================

/// Bounded, real-clock wait for the worker to drop its liveness flag. Bounded
/// so a regression fails the assertion instead of hanging the suite; the
/// worker does no I/O on this path and normally clears it within microseconds.
static bool wait_for_worker_exit(const CameraStream& s, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!s.is_running()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return !s.is_running();
}

TEST_CASE_METHOD(HelixTestFixture, "CameraStream: worker clears is_running() when it gives up",
                 "[camera][1245]") {
    CameraStream stream;
    REQUIRE_FALSE(stream.is_running());

    // Failure budget exhausted, no snapshot URL: the worker reports the error
    // and the thread returns. Nothing else will ever run on this object again.
    CameraStreamTestAccess::spawn_worker_to_exhaustion(stream);

    // is_running() is exactly what CameraWidget::start_stream() consults before
    // deciding a restart would be redundant. A worker that has exited but still
    // reports true makes the camera unrevivable — the feed stays dead until the
    // user switches tabs, which is what issue #1245 item 4 describes.
    CHECK(wait_for_worker_exit(stream));
    CHECK_FALSE(stream.is_running());

    stream.stop(); // reap the thread; also the "worker already gone" stop() path
    CHECK_FALSE(stream.is_running());

    helix::ui::UpdateQueue::instance().drain(); // report_error()'s deferred callback
}

TEST_CASE_METHOD(HelixTestFixture, "CameraStream: restart after the worker gave up spawns again",
                 "[camera][1245]") {
    CameraStream stream;
    CameraStreamTestAccess::spawn_worker_to_exhaustion(stream);
    REQUIRE(wait_for_worker_exit(stream));
    REQUIRE_FALSE(stream.is_running());
    // The dead worker is still attached to the std::thread object — nothing
    // has joined it, exactly as in production.
    REQUIRE(CameraStreamTestAccess::worker_joinable(stream));

    // start() must reap it before assigning a new one — assigning over a
    // joinable std::thread is std::terminate, and with is_running() now false
    // the old "if (running_) stop();" guard would sail straight past it.
    std::atomic<int> frames{0};
    stream.start("http://127.0.0.1:9/stream", "", [&](lv_draw_buf_t*) { frames.fetch_add(1); });
    CHECK(stream.is_running());

    stream.stop();
    CHECK_FALSE(stream.is_running());
    CHECK(frames.load() == 0); // nothing is listening on port 9

    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(HelixTestFixture, "CameraStream: snapshot fallback keeps is_running() true",
                 "[camera][1245]") {
    CameraStream stream;
    // Paused: fetch_snapshot() returns before issuing any HTTP request, so the
    // poll loop spins on its sleep alone and the test touches no network.
    stream.set_max_fps(0);

    std::atomic<bool> saw_running{false};
    std::atomic<bool> released{false};
    std::thread watcher([&] {
        for (int i = 0; i < 200 && !saw_running.load(); ++i) {
            if (stream.is_running()) {
                saw_running.store(true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // Let the poll loop actually run for a beat before releasing it.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CameraStreamTestAccess::release_worker(stream);
        released.store(true);
    });

    // Blocks in snapshot_poll_loop() until the watcher releases it. Clearing
    // running_ where the reconnect loop gives up (rather than where the worker
    // body actually returns) would skip the fallback entirely and this would
    // return with saw_running still false.
    CameraStreamTestAccess::run_worker_inline_with_snapshot(stream,
                                                            "http://127.0.0.1:9/snapshot.jpg");
    watcher.join();

    CHECK(saw_running.load());
    CHECK(released.load());
    CHECK_FALSE(stream.is_running());

    helix::ui::UpdateQueue::instance().drain();
}

#else // !HELIX_HAS_CAMERA

TEST_CASE("CameraWidget: not registered on embedded platforms", "[camera][panel_widget]") {
    const auto* def = find_widget_def("camera");
    REQUIRE(def == nullptr);
}

#endif // HELIX_HAS_CAMERA
