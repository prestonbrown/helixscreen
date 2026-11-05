// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_webcam_selection.cpp
 * @brief Which webcam a camera view shows: the auto-pick and the per-widget
 *        `source` override, as pure functions over the discovered list.
 *
 * No printer, no network, no display. The rule under test is the one the
 * camera widget, the fullscreen viewers and the config modal all share
 * (webcam_selection.h), so a wrong fallback here is a wrong fallback
 * everywhere.
 */

#include "webcam_selection.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::webcam;

namespace {

WebcamInfo mjpeg(const std::string& name, const std::string& path = "/webcam/") {
    WebcamInfo cam;
    cam.name = name;
    cam.service = "mjpegstreamer";
    cam.stream_url = path + "?action=stream";
    cam.snapshot_url = path + "?action=snapshot";
    return cam;
}

WebcamInfo webrtc(const std::string& name, const std::string& path = "/webrtc/") {
    WebcamInfo cam;
    cam.name = name;
    cam.service = "webrtc-go2rtc";
    cam.stream_url = path + "stream.html";
    cam.snapshot_url = path + "frame.jpeg";
    return cam;
}

} // namespace

TEST_CASE("webcam auto-pick prefers the first MJPEG entry", "[webcam][camera]") {
    std::vector<WebcamInfo> cams = {webrtc("Chamber"), mjpeg("Nozzle", "/nozzle/"),
                                    mjpeg("Bed", "/bed/")};

    auto pick = auto_pick(cams);
    REQUIRE(pick.has_value());
    CHECK(pick->name == "Nozzle");
    CHECK(pick->stream_url == "/nozzle/?action=stream");
    CHECK(auto_pick_index(cams) == 1);
}

TEST_CASE("webcam auto-pick falls back to snapshot polling for non-MJPEG services",
          "[webcam][camera]") {
    std::vector<WebcamInfo> cams = {webrtc("Chamber")};

    auto pick = auto_pick(cams);
    REQUIRE(pick.has_value());
    CHECK(pick->name == "Chamber");
    // The stream_url is an HTML page; polling the snapshot is all that works.
    CHECK(pick->stream_url.empty());
    CHECK(pick->snapshot_url == "/webrtc/frame.jpeg");
}

TEST_CASE("webcam auto-pick skips entries discovery ruled out", "[webcam][camera]") {
    SECTION("service down") {
        std::vector<WebcamInfo> cams = {mjpeg("Nozzle"), mjpeg("Bed", "/bed/")};
        cams[0].unavailable_reason = "service not running: crowsnest (failed/failed)";
        auto pick = auto_pick(cams);
        REQUIRE(pick.has_value());
        CHECK(pick->name == "Bed");
    }
    SECTION("disabled") {
        std::vector<WebcamInfo> cams = {mjpeg("Nozzle"), mjpeg("Bed", "/bed/")};
        cams[0].enabled = false;
        auto pick = auto_pick(cams);
        REQUIRE(pick.has_value());
        CHECK(pick->name == "Bed");
    }
    SECTION("snapshot_url is an HTML viewer page") {
        WebcamInfo iframe;
        iframe.name = "K2";
        iframe.service = "iframe";
        iframe.snapshot_url = "/snapshot.html";
        std::vector<WebcamInfo> cams = {iframe};
        CHECK_FALSE(auto_pick(cams).has_value());
        CHECK_FALSE(is_usable(iframe));
    }
    SECTION("nothing listed") {
        CHECK_FALSE(auto_pick({}).has_value());
    }
}

TEST_CASE("webcam auto-pick takes an unnamed local-probe entry", "[webcam][camera]") {
    // Discovery appends this when Moonraker lists nothing usable and a
    // loopback streamer answered: no name, no service, snapshot only.
    WebcamInfo local;
    local.snapshot_url = "http://127.0.0.1:8080/?action=snapshot";
    std::vector<WebcamInfo> cams = {webrtc("Dead")};
    cams[0].unavailable_reason = "unreachable at http://10.0.0.5/frame.jpeg";
    cams.push_back(local);

    auto pick = auto_pick(cams);
    REQUIRE(pick.has_value());
    CHECK(pick->name.empty());
    CHECK(pick->snapshot_url == local.snapshot_url);
}

TEST_CASE("webcam source picks the named camera over the auto-pick", "[webcam][camera]") {
    std::vector<WebcamInfo> cams = {mjpeg("Nozzle", "/nozzle/"), mjpeg("Bed", "/bed/")};
    cams[1].flip_horizontal = true;
    cams[1].target_fps = 5;

    auto pick = select_webcam(cams, "Bed");
    REQUIRE(pick.has_value());
    CHECK(pick->name == "Bed");
    CHECK(pick->stream_url == "/bed/?action=stream");
    // The chosen camera's own Moonraker transform and fps travel with it.
    CHECK(pick->flip_horizontal);
    CHECK(pick->target_fps == 5);
    CHECK(source_is_honored(cams, "Bed"));
}

TEST_CASE("webcam source with no preference is the auto-pick", "[webcam][camera]") {
    std::vector<WebcamInfo> cams = {mjpeg("Nozzle", "/nozzle/"), mjpeg("Bed", "/bed/")};
    auto pick = select_webcam(cams, "");
    REQUIRE(pick.has_value());
    CHECK(pick->name == "Nozzle");
    CHECK_FALSE(source_is_honored(cams, ""));
}

TEST_CASE("webcam source falls back to the auto-pick, never to no camera", "[webcam][camera]") {
    std::vector<WebcamInfo> cams = {mjpeg("Nozzle", "/nozzle/"), mjpeg("Bed", "/bed/")};

    SECTION("named camera is absent from the list") {
        auto pick = select_webcam(cams, "Chamber");
        REQUIRE(pick.has_value());
        CHECK(pick->name == "Nozzle");
        CHECK_FALSE(source_is_honored(cams, "Chamber"));
    }
    SECTION("named camera's service is down") {
        cams[1].unavailable_reason = "service not running: crowsnest (failed/failed)";
        auto pick = select_webcam(cams, "Bed");
        REQUIRE(pick.has_value());
        CHECK(pick->name == "Nozzle");
        CHECK_FALSE(source_is_honored(cams, "Bed"));
    }
    SECTION("named camera is disabled") {
        cams[1].enabled = false;
        auto pick = select_webcam(cams, "Bed");
        REQUIRE(pick.has_value());
        CHECK(pick->name == "Nozzle");
    }
    SECTION("named camera is down and it was also the auto-pick") {
        cams[0].unavailable_reason = "unreachable at http://10.0.0.5/?action=snapshot";
        auto pick = select_webcam(cams, "Nozzle");
        REQUIRE(pick.has_value());
        CHECK(pick->name == "Bed");
    }
    SECTION("nothing usable at all") {
        cams[0].unavailable_reason = "x";
        cams[1].unavailable_reason = "y";
        CHECK_FALSE(select_webcam(cams, "Bed").has_value());
    }
}

TEST_CASE("webcam source names a non-MJPEG camera: snapshot polling", "[webcam][camera]") {
    std::vector<WebcamInfo> cams = {mjpeg("Nozzle", "/nozzle/"), webrtc("Chamber")};
    auto pick = select_webcam(cams, "Chamber");
    REQUIRE(pick.has_value());
    CHECK(pick->name == "Chamber");
    CHECK(pick->stream_url.empty());
    CHECK(pick->snapshot_url == "/webrtc/frame.jpeg");
}

TEST_CASE("webcam entry parsing reads Moonraker's fields", "[webcam][camera]") {
    nlohmann::json entry = {{"name", "Nozzle"},
                            {"service", "ustreamer"},
                            {"stream_url", "/webcam/?action=stream"},
                            {"snapshot_url", "/webcam/?action=snapshot"},
                            {"uid", "abc"},
                            {"enabled", false},
                            {"flip_horizontal", true},
                            {"flip_vertical", true},
                            {"target_fps", 30}};
    WebcamInfo cam = parse_webcam_entry(entry);
    CHECK(cam.name == "Nozzle");
    CHECK(cam.service == "ustreamer");
    CHECK(cam.stream_url == "/webcam/?action=stream");
    CHECK(cam.snapshot_url == "/webcam/?action=snapshot");
    CHECK(cam.uid == "abc");
    CHECK_FALSE(cam.enabled);
    CHECK(cam.flip_horizontal);
    CHECK(cam.flip_vertical);
    CHECK(cam.target_fps == 30);
    CHECK(cam.unavailable_reason.empty());

    WebcamInfo bare = parse_webcam_entry(nlohmann::json::object());
    CHECK(bare.enabled);
    CHECK(bare.target_fps == 15);
    CHECK(is_mjpeg_service("mjpegstreamer-adaptive"));
    CHECK(is_mjpeg_service("ustreamer"));
    CHECK_FALSE(is_mjpeg_service("webrtc-camerastreamer"));
    CHECK_FALSE(is_mjpeg_service(""));
}
