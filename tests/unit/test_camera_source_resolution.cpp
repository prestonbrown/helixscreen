// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_camera_source_resolution.cpp
 * @brief A camera view's `source` preference against PrinterState's webcam
 *        list: CameraStream::resolve_from_printer() hands back the named
 *        camera when it is usable and the auto-pick otherwise, and the widget
 *        reads its preference from the per-widget config.
 */

#include "lvgl.h"

#include "hv/json.hpp"

#if HELIX_HAS_CAMERA

#include "../lvgl_test_fixture.h"
#include "../test_helpers/camera_widget_test_access.h"
#include "app_globals.h"
#include "camera_stream.h"
#include "core/update_queue.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/camera_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

WebcamInfo mjpeg(const std::string& name, const std::string& path) {
    WebcamInfo cam;
    cam.name = name;
    cam.service = "mjpegstreamer";
    cam.stream_url = path + "?action=stream";
    cam.snapshot_url = path + "?action=snapshot";
    return cam;
}

/// The global PrinterState is process-wide: publish a list, and put it back
/// to "no webcam" on the way out so a later test starts from what it expects.
struct WebcamListScope {
    explicit WebcamListScope(std::vector<WebcamInfo> cams) {
        get_printer_state().capabilities_state().set_webcams(std::move(cams));
        helix::ui::UpdateQueue::instance().drain();
    }
    ~WebcamListScope() {
        get_printer_state().capabilities_state().set_webcam_available(false);
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "CameraStream::resolve_from_printer honors a usable source",
                 "[camera][webcam]") {
    WebcamListScope scope({mjpeg("Nozzle", "/nozzle/"), mjpeg("Bed", "/bed/")});

    auto feed = CameraStream::resolve_from_printer("Bed");
    REQUIRE(feed.has_value());
    CHECK(feed->name == "Bed");
    CHECK(feed->stream_url == "/bed/?action=stream");

    auto none = CameraStream::resolve_from_printer("");
    REQUIRE(none.has_value());
    CHECK(none->name == "Nozzle");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "CameraStream::resolve_from_printer falls back to the auto-pick, not to nothing",
                 "[camera][webcam]") {
    SECTION("named camera absent") {
        WebcamListScope scope({mjpeg("Nozzle", "/nozzle/")});
        auto feed = CameraStream::resolve_from_printer("Bed");
        REQUIRE(feed.has_value());
        CHECK(feed->name == "Nozzle");
    }
    SECTION("named camera's service down") {
        std::vector<WebcamInfo> cams = {mjpeg("Nozzle", "/nozzle/"), mjpeg("Bed", "/bed/")};
        cams[1].unavailable_reason = "service not running: crowsnest (failed/failed)";
        WebcamListScope scope(cams);
        auto feed = CameraStream::resolve_from_printer("Bed");
        REQUIRE(feed.has_value());
        CHECK(feed->name == "Nozzle");
    }
    SECTION("no cameras at all") {
        WebcamListScope scope({});
        CHECK_FALSE(CameraStream::resolve_from_printer("Bed").has_value());
        CHECK_FALSE(CameraStream::resolve_from_printer("").has_value());
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "CameraWidget reads its source from the per-widget config",
                 "[camera][webcam]") {
    CameraWidget w;
    CHECK(CameraWidgetTestAccess::configured_source(w).empty());

    w.set_config({{"rotation", 90}, {"source", "Bed"}});
    CHECK(CameraWidgetTestAccess::configured_source(w) == "Bed");
    // Nothing is streaming, so a config change asks nothing of the stream.
    CHECK_FALSE(CameraWidgetTestAccess::has_stream(w));
    CHECK(CameraWidgetTestAccess::stop_stream_calls(w) == 0);

    w.set_config({{"source", 42}}); // wrong type reads as no preference
    CHECK(CameraWidgetTestAccess::configured_source(w).empty());
}

#endif // HELIX_HAS_CAMERA
