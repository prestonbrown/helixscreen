// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_webcam_service_health.cpp
 * @brief Unit tests for helix::webcam service-state cross-checking.
 *
 * prestonbrown/helixscreen#1351: webcam availability was decided from
 * server.webcams.list alone. A crowsnest install in `service_state: failed`
 * still leaves its entry in the webcam database, and because a stock crowsnest
 * registers a RELATIVE snapshot_url ("/webcam/?action=snapshot") the discovery
 * reachability probe deliberately skips it — so the camera widget offered a
 * stream nothing could answer.
 *
 * The rule under test is deliberately asymmetric: it hides a camera ONLY on a
 * positive "this service is down" from systemd. Unknown service, absent
 * service_state, and every transient systemd state must read as available,
 * because a false negative (hiding a working camera) is worse than the false
 * positive being removed here.
 */

#include "../../include/webcam_service_health.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::webcam;
using nlohmann::json;

namespace {

// A stock crowsnest webcam entry: MJPEG service, RELATIVE urls — the exact
// shape the absolute-URL reachability probe skips.
json crowsnest_cam() {
    return json{{"name", "webcam"},
                {"service", "mjpegstreamer"},
                {"enabled", true},
                {"stream_url", "/webcam/?action=stream"},
                {"snapshot_url", "/webcam/?action=snapshot"}};
}

json svc(const std::string& active, const std::string& sub) {
    return json{{"active_state", active}, {"sub_state", sub}};
}

// The always-present entries Moonraker reports beside the camera service.
json base_services() {
    return json{{"klipper", svc("active", "running")}, {"moonraker", svc("active", "running")}};
}

} // namespace

// ============================================================================
// The reported bug: crowsnest failed + relative URL
// ============================================================================

TEST_CASE("failed camera service hides a webcam the probe cannot check", "[1351][webcam]") {
    json services = base_services();
    services["crowsnest"] = svc("failed", "failed");

    std::string reason;
    REQUIRE(owning_service_is_down(crowsnest_cam(), services, &reason));
    // The reason names the unit AND its systemd state, so a field log says why.
    REQUIRE(reason.find("crowsnest") != std::string::npos);
    REQUIRE(reason.find("failed") != std::string::npos);
    REQUIRE(owning_service_unit(crowsnest_cam(), services) == "crowsnest");
}

TEST_CASE("running camera service leaves a relative-URL webcam available", "[1351][webcam]") {
    json services = base_services();
    services["crowsnest"] = svc("active", "running");

    REQUIRE_FALSE(owning_service_is_down(crowsnest_cam(), services));
    REQUIRE(owning_service_unit(crowsnest_cam(), services) == "crowsnest");
}

TEST_CASE("a stopped (not crashed) camera service also counts as down", "[1351][webcam]") {
    json services = base_services();
    services["crowsnest"] = svc("inactive", "dead");

    REQUIRE(owning_service_is_down(crowsnest_cam(), services));
}

// ============================================================================
// Fail-open: every way of NOT knowing must leave the camera available
// ============================================================================

TEST_CASE("absent service_state leaves the webcam available", "[1351][webcam]") {
    REQUIRE_FALSE(owning_service_is_down(crowsnest_cam(), json::object()));
    REQUIRE(owning_service_unit(crowsnest_cam(), json::object()).empty());
}

TEST_CASE("service_state without the camera unit leaves the webcam available", "[1351][webcam]") {
    // Klipper and Moonraker are up; nothing reports on the streamer at all.
    REQUIRE_FALSE(owning_service_is_down(crowsnest_cam(), base_services()));
}

TEST_CASE("an unrecognized webcam service is never condemned", "[1351][webcam]") {
    json cam = crowsnest_cam();
    cam["service"] = "some-future-streamer";
    json services = base_services();
    services["crowsnest"] = svc("failed", "failed");

    // A failed crowsnest says nothing about a streamer we cannot map to it.
    REQUIRE_FALSE(owning_service_is_down(cam, services));
}

TEST_CASE("a transient systemd state is not down", "[1351][webcam]") {
    for (const auto& state : {svc("activating", "start"), svc("reloading", "running"),
                              svc("unknown", "unknown"), svc("active", "running")}) {
        json services = base_services();
        services["crowsnest"] = state;
        INFO("active_state=" << state.value("active_state", ""));
        REQUIRE_FALSE(owning_service_is_down(crowsnest_cam(), services));
    }
}

TEST_CASE("malformed service_state entries fail open", "[1351][webcam]") {
    json services = base_services();

    SECTION("null entry") {
        services["crowsnest"] = nullptr;
        REQUIRE_FALSE(owning_service_is_down(crowsnest_cam(), services));
    }
    SECTION("non-string states") {
        services["crowsnest"] = json{{"active_state", 3}, {"sub_state", nullptr}};
        REQUIRE_FALSE(owning_service_is_down(crowsnest_cam(), services));
    }
    SECTION("webcam entry with no service field") {
        services["crowsnest"] = svc("failed", "failed");
        json cam = crowsnest_cam();
        cam.erase("service");
        REQUIRE_FALSE(owning_service_is_down(cam, services));
    }
    SECTION("webcam entry is not an object") {
        services["crowsnest"] = svc("failed", "failed");
        REQUIRE_FALSE(owning_service_is_down(json("webcam"), services));
    }
}

// ============================================================================
// Resolving WHICH unit owns a webcam entry
// ============================================================================

TEST_CASE("a healthy streamer wins over a failed supervisor beside it", "[1351][webcam]") {
    // ustreamer running standalone with a broken crowsnest still installed:
    // "mjpegstreamer" maps to both, and the live one must decide.
    json services = base_services();
    services["ustreamer"] = svc("active", "running");
    services["crowsnest"] = svc("failed", "failed");

    REQUIRE_FALSE(owning_service_is_down(crowsnest_cam(), services));
    REQUIRE(owning_service_unit(crowsnest_cam(), services) == "ustreamer");
}

TEST_CASE("every candidate down means down", "[1351][webcam]") {
    json services = base_services();
    services["ustreamer"] = svc("inactive", "dead");
    services["crowsnest"] = svc("failed", "failed");

    REQUIRE(owning_service_is_down(crowsnest_cam(), services));
}

TEST_CASE("unit keys match across .service and separator spellings", "[1351][webcam]") {
    json cam = crowsnest_cam();
    cam["service"] = "webrtc-camerastreamer";
    json services = base_services();
    services["camera-streamer.service"] = svc("failed", "failed");

    REQUIRE(owning_service_is_down(cam, services));
    REQUIRE(owning_service_unit(cam, services) == "camera-streamer.service");
}

TEST_CASE("a service named exactly like its unit needs no table row", "[1351][webcam]") {
    json cam = crowsnest_cam();
    cam["service"] = "go2rtc";
    json services = base_services();
    services["go2rtc"] = svc("failed", "failed");

    REQUIRE(owning_service_is_down(cam, services));
}

// ============================================================================
// extract_service_state — accepts the shapes discovery actually holds
// ============================================================================

TEST_CASE("extract_service_state unwraps the JSON-RPC envelope", "[1351][webcam]") {
    json services = base_services();
    services["crowsnest"] = svc("failed", "failed");
    json response = json{{"result", {{"system_info", {{"service_state", services}}}}}};

    json out = extract_service_state(response);
    REQUIRE(out.contains("crowsnest"));
    REQUIRE(owning_service_is_down(crowsnest_cam(), out));
}

TEST_CASE("extract_service_state accepts a bare system_info object", "[1351][webcam]") {
    json services = base_services();
    json wrapped = json{{"system_info", {{"service_state", services}}}};
    REQUIRE(extract_service_state(wrapped).contains("klipper"));

    json bare = json{{"service_state", services}};
    REQUIRE(extract_service_state(bare).contains("klipper"));
}

TEST_CASE("extract_service_state yields an empty object when there is none", "[1351][webcam]") {
    // Older Moonraker / non-systemd host: distribution and cpu_info, no services.
    json response = json{{"result", {{"system_info", {{"distribution", {{"name", "Debian"}}}}}}}};
    REQUIRE(extract_service_state(response).is_object());
    REQUIRE(extract_service_state(response).empty());

    REQUIRE(extract_service_state(json::array()).empty());
    REQUIRE(extract_service_state(json("nope")).empty());
    // A non-object service_state must not be handed back as one.
    REQUIRE(extract_service_state(json{{"service_state", "failed"}}).empty());
}
