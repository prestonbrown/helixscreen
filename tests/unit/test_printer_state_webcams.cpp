// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_state_webcams.cpp
 * @brief PrinterCapabilitiesState keeps the whole webcam list and derives the
 *        auto-pick into the single-feed getters and `printer_has_webcam`, with
 *        `webcam_count` counting the entries a picker can offer.
 */

#include "../lvgl_test_fixture.h"
#include "core/update_queue.h"
#include "printer_capabilities_state.h"

#include "../catch_amalgamated.hpp"

using helix::Capability;
using helix::PrinterCapabilitiesState;
using helix::ui::UpdateQueue;

namespace {
WebcamInfo mjpeg(const std::string& name, const std::string& path) {
    WebcamInfo cam;
    cam.name = name;
    cam.service = "mjpegstreamer";
    cam.stream_url = path + "?action=stream";
    cam.snapshot_url = path + "?action=snapshot";
    return cam;
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrinterCapabilitiesState::set_webcams keeps the list and "
                 "publishes the auto-pick",
                 "[capabilities][webcam][camera]") {
    UpdateQueue::instance().drain();
    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    std::vector<WebcamInfo> cams = {mjpeg("Nozzle", "/nozzle/"), mjpeg("Bed", "/bed/")};
    cams[0].unavailable_reason = "service not running: crowsnest (failed/failed)";
    cams[1].flip_vertical = true;
    cams[1].target_fps = 10;
    WebcamInfo local; // unnamed loopback probe result
    local.snapshot_url = "http://127.0.0.1:8080/?action=snapshot";
    cams.push_back(local);

    caps.set_webcams(cams);
    UpdateQueue::instance().drain();

    REQUIRE(caps.get_webcams().size() == 3);
    CHECK(caps.get_webcams()[0].name == "Nozzle");
    CHECK(caps.get_webcams()[0].unavailable_reason ==
          "service not running: crowsnest (failed/failed)");
    // The auto-pick skips the down camera and lands on Bed.
    CHECK(lv_subject_get_int(caps.subject(Capability::HasWebcam)) == 1);
    CHECK(caps.get_webcam_stream_url() == "/bed/?action=stream");
    CHECK(caps.get_webcam_snapshot_url() == "/bed/?action=snapshot");
    CHECK(caps.get_webcam_flip_vertical());
    CHECK_FALSE(caps.get_webcam_flip_horizontal());
    CHECK(caps.get_webcam_target_fps() == 10);
    // Two named entries; the loopback one has no name to pick.
    CHECK(lv_subject_get_int(caps.subject(Capability::WebcamCount)) == 2);

    // Nothing usable: the list is still there for a picker, the feed is gone.
    cams[1].unavailable_reason = "unreachable at http://10.0.0.5/?action=snapshot";
    cams.pop_back();
    caps.set_webcams(cams);
    UpdateQueue::instance().drain();
    CHECK(caps.get_webcams().size() == 2);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasWebcam)) == 0);
    CHECK(caps.get_webcam_stream_url().empty());
    CHECK(lv_subject_get_int(caps.subject(Capability::WebcamCount)) == 2);

    caps.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrinterCapabilitiesState::set_webcam_available is the one-entry list",
                 "[capabilities][webcam][camera]") {
    UpdateQueue::instance().drain();
    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    caps.set_webcam_available(true, "/webcam/?action=stream", "/webcam/?action=snapshot", true,
                              false, 30);
    UpdateQueue::instance().drain();
    REQUIRE(caps.get_webcams().size() == 1);
    // A stream URL handed in directly is streamed, not demoted to snapshot polling.
    CHECK(caps.get_webcam_stream_url() == "/webcam/?action=stream");
    CHECK(caps.get_webcam_snapshot_url() == "/webcam/?action=snapshot");
    CHECK(caps.get_webcam_flip_horizontal());
    CHECK(caps.get_webcam_target_fps() == 30);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasWebcam)) == 1);
    CHECK(lv_subject_get_int(caps.subject(Capability::WebcamCount)) == 0);

    caps.set_webcam_available(false);
    UpdateQueue::instance().drain();
    CHECK(caps.get_webcams().empty());
    CHECK(lv_subject_get_int(caps.subject(Capability::HasWebcam)) == 0);

    caps.deinit_subjects();
}
