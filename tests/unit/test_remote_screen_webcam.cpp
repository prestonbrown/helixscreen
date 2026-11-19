// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_remote_screen_webcam.cpp
 * @brief The webcam entry the screen server registers in Moonraker, and the round
 *        trip through the mock's registry.
 */

#include "../lvgl_test_fixture.h"
#include "host_identity.h"
#include "json_utils.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "remote_screen_webcam.h"
#include "webcam_selection.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

TEST_CASE("webcam item: view-only with route is mjpegstreamer on stream.mjpg, relative snapshot",
          "[remote_screen][webcam]") {
    auto item = remote_screen_webcam_item(kRemoteScreenWebcamName, true, false,
                                          "/helixscreen/screen/", "10.0.0.5", 7130, 800, 480);
    REQUIRE(item.has_value());
    CHECK((*item)["name"] == kRemoteScreenWebcamName);
    CHECK((*item)["service"] == "mjpegstreamer");
    CHECK((*item)["stream_url"] == "/helixscreen/screen/stream.mjpg");
    CHECK((*item)["snapshot_url"] == "/helixscreen/screen/snapshot.jpg");
    CHECK((*item)["target_fps"] == 2);
    CHECK((*item)["location"] == "printer");
    CHECK((*item)["enabled"] == true);
}

TEST_CASE("webcam item: control with route is iframe pointing at the route",
          "[remote_screen][webcam]") {
    auto item = remote_screen_webcam_item(kRemoteScreenWebcamName, true, true,
                                          "/helixscreen/screen/", "10.0.0.5", 7130, 800, 480);
    REQUIRE(item.has_value());
    CHECK((*item)["service"] == "iframe");
    CHECK((*item)["stream_url"] == "/helixscreen/screen/");
    CHECK((*item)["snapshot_url"] == "/helixscreen/screen/snapshot.jpg");
}

TEST_CASE("webcam item: LAN mode uses http://ip:port", "[remote_screen][webcam]") {
    auto view = remote_screen_webcam_item(kRemoteScreenWebcamName, true, false, "", "192.168.1.40",
                                          7130, 800, 480);
    REQUIRE(view.has_value());
    CHECK((*view)["snapshot_url"] == "http://192.168.1.40:7130/snapshot.jpg");
    auto control = remote_screen_webcam_item(kRemoteScreenWebcamName, true, true, "",
                                             "192.168.1.40", 7131, 800, 480);
    REQUIRE(control.has_value());
    CHECK((*control)["stream_url"] == "http://192.168.1.40:7131/");
}

TEST_CASE("webcam item: disabled is nullopt", "[remote_screen][webcam]") {
    CHECK_FALSE(remote_screen_webcam_item(kRemoteScreenWebcamName, false, false, "/r/", "10.0.0.5",
                                          7130, 800, 480)
                    .has_value());
    CHECK_FALSE(remote_screen_webcam_item(kRemoteScreenWebcamName, false, true, "", "10.0.0.5",
                                          7130, 800, 480)
                    .has_value());
    // LAN mode with no address a browser could reach: registering would only mislead.
    CHECK_FALSE(
        remote_screen_webcam_item(kRemoteScreenWebcamName, true, false, "", "", 7130, 800, 480)
            .has_value());
}

TEST_CASE("webcam aspect ratio: the frame size in lowest terms", "[remote_screen][webcam]") {
    CHECK(remote_screen_aspect_ratio(800, 480) == "5:3");
    CHECK(remote_screen_aspect_ratio(480, 800) == "3:5");
    CHECK(remote_screen_aspect_ratio(1024, 600) == "128:75");
    CHECK(remote_screen_aspect_ratio(1920, 1080) == "16:9");
    CHECK(remote_screen_aspect_ratio(480, 480) == "1:1");
    // Nothing known: no ratio, so Moonraker keeps its own default.
    CHECK(remote_screen_aspect_ratio(0, 480).empty());
    CHECK(remote_screen_aspect_ratio(800, 0).empty());
    CHECK(remote_screen_aspect_ratio(-800, 480).empty());
}

TEST_CASE("webcam item: aspect ratio follows the screen", "[remote_screen][webcam]") {
    for (const bool control : {false, true}) {
        auto nginx = remote_screen_webcam_item(kRemoteScreenWebcamName, true, control,
                                               "/helixscreen/screen/", "", 7130, 1024, 600);
        REQUIRE(nginx.has_value());
        CHECK((*nginx)["aspect_ratio"] == "128:75");
        auto lan = remote_screen_webcam_item(kRemoteScreenWebcamName, true, control, "",
                                             "192.168.1.40", 7130, 480, 800);
        REQUIRE(lan.has_value());
        CHECK((*lan)["aspect_ratio"] == "3:5");
    }
    auto unknown = remote_screen_webcam_item(kRemoteScreenWebcamName, true, true, "",
                                             "192.168.1.40", 7130, 0, 0);
    REQUIRE(unknown.has_value());
    CHECK_FALSE(unknown->contains("aspect_ratio"));
}

TEST_CASE("select_lan_ipv4 skips 127.x, 169.254.x and IPv6", "[remote_screen][webcam]") {
    CHECK(select_lan_ipv4({"127.0.0.1", "::1", "fe80::1", "169.254.3.4", "192.168.1.9",
                           "10.0.0.2"}) == "192.168.1.9");
    CHECK(select_lan_ipv4({"127.0.0.1", "fe80::1"}).empty());
    CHECK(select_lan_ipv4({}).empty());
}

TEST_CASE("is_own_screen_webcam matches the registered name only", "[remote_screen][webcam]") {
    WebcamInfo own;
    own.name = kRemoteScreenWebcamName;
    WebcamInfo other;
    other.name = "Nozzle";
    CHECK(webcam::is_own_screen_webcam(own));
    CHECK_FALSE(webcam::is_own_screen_webcam(other));
}

TEST_CASE_METHOD(LVGLTestFixture, "mock: post then list round-trips; delete removes it",
                 "[remote_screen][webcam]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPI api(client, state);

    auto names = [&] {
        std::vector<std::string> out;
        api.timelapse().get_webcam_list(
            [&](const std::vector<WebcamInfo>& cams) {
                for (const auto& c : cams)
                    out.push_back(c.name);
            },
            [](const MoonrakerError&) {});
        return out;
    };
    auto has = [](const std::vector<std::string>& v, const std::string& n) {
        return std::find(v.begin(), v.end(), n) != v.end();
    };

    auto item = remote_screen_webcam_item(kRemoteScreenWebcamName, true, false,
                                          "/helixscreen/screen/", "", 7130, 800, 480);
    REQUIRE(item.has_value());
    bool posted = false;
    api.timelapse().post_webcam(
        *item, [&](const WebcamInfo&) { posted = true; }, [](const MoonrakerError&) {});
    CHECK(posted);
    CHECK(has(names(), kRemoteScreenWebcamName));

    // A second post replaces rather than duplicates.
    api.timelapse().post_webcam(*item, [](const WebcamInfo&) {}, [](const MoonrakerError&) {});
    const auto listed = names();
    CHECK(std::count(listed.begin(), listed.end(), kRemoteScreenWebcamName) == 1);

    bool deleted = false;
    WebcamInfo screen;
    screen.name = kRemoteScreenWebcamName;
    api.timelapse().delete_webcam(screen, [&] { deleted = true; }, [](const MoonrakerError&) {});
    CHECK(deleted);
    CHECK_FALSE(has(names(), kRemoteScreenWebcamName));
}

TEST_CASE("webcam name: the plain name on this host, the host name added for another's Moonraker",
          "[remote_screen][webcam]") {
    // Two screens on one Moonraker must not overwrite each other's entry.
    CHECK(remote_screen_webcam_name(true, "helixpi") == kRemoteScreenWebcamName);
    CHECK(remote_screen_webcam_name(false, "helixpi") ==
          std::string(kRemoteScreenWebcamName) + " (helixpi)");
    CHECK(remote_screen_webcam_name(false, "") == kRemoteScreenWebcamName);
}

TEST_CASE("is_own_screen_webcam also matches another screen's entry", "[remote_screen][webcam]") {
    WebcamInfo other_screen;
    other_screen.name = std::string(kRemoteScreenWebcamName) + " (helixpi)";
    CHECK(webcam::is_own_screen_webcam(other_screen));
    WebcamInfo lookalike;
    lookalike.name = "HelixScreen Screenshots";
    CHECK_FALSE(webcam::is_own_screen_webcam(lookalike));
}

TEST_CASE("the nginx route only applies when Moonraker is on this host",
          "[remote_screen][webcam]") {
    // The route is this host's nginx guarding this host's Moonraker; a screen pointed at
    // another printer faces the LAN with pairing instead.
    CHECK(remote_screen_effective_route(false, "/helixscreen/screen/") == "/helixscreen/screen/");
    CHECK(remote_screen_effective_route(true, "/helixscreen/screen/").empty());
    CHECK(remote_screen_effective_route(false, "").empty());
}

TEST_CASE("webcam item carries the screen marker; parse and is_own_screen_webcam read it",
          "[remote_screen][webcam]") {
    auto item =
        remote_screen_webcam_item(kRemoteScreenWebcamName, true, false, "/r/", "", 7130, 800, 480);
    REQUIRE(item.has_value());
    json renamed = *item;
    renamed["name"] = "Printer screen";
    const WebcamInfo cam = webcam::parse_webcam_entry(renamed);
    CHECK(cam.screen_server);
    CHECK(webcam::is_own_screen_webcam(cam));
    // A feed the user added has no marker, whatever it is called.
    const WebcamInfo feed = webcam::parse_webcam_entry(json{{"name", "Printer screen"}});
    CHECK_FALSE(feed.screen_server);
    CHECK_FALSE(webcam::is_own_screen_webcam(feed));
}

namespace {

/// The screen's webcam entry tracked across reconciles, against the mock's registry.
struct WebcamReconcileHarness {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
    std::string uid;
    int stores = 0;

    WebcamReconcileHarness() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPI>(client, state);
    }

    std::optional<json> item(const std::string& lan_ip = "192.168.1.40") const {
        return remote_screen_webcam_item(kRemoteScreenWebcamName, true, false, "", lan_ip, 7130,
                                         800, 480);
    }

    void reconcile(const std::optional<json>& want) {
        reconcile_remote_screen_webcam(*api, want, kRemoteScreenWebcamName, uid,
                                       [this](const std::string& u) {
                                           uid = u;
                                           ++stores;
                                       });
    }

    std::vector<WebcamInfo> screens() {
        std::vector<WebcamInfo> out;
        api->timelapse().get_webcam_list(
            [&](const std::vector<WebcamInfo>& cams) {
                for (const auto& c : cams)
                    if (webcam::is_own_screen_webcam(c))
                        out.push_back(c);
            },
            [](const MoonrakerError&) {});
        return out;
    }

    /// What the user does in Fluidd's webcam settings: a new name and frame rate.
    void rename(const std::string& to) {
        api->timelapse().post_webcam(
            json{{"uid", uid}, {"name", to}, {"target_fps", 5}}, [](const WebcamInfo&) {},
            [](const MoonrakerError&) {});
    }
    /// What Moonraker stored for the tracked entry, aspect_ratio included.
    json stored() {
        auto& posted = client.posted_webcams();
        std::lock_guard<std::mutex> lock(posted.mutex);
        for (const auto& e : posted.items) {
            if (json_util::safe_string(e, "uid", "") == uid) {
                return e;
            }
        }
        return json();
    }
    /// What the user does when the screen tile looks wrong: pick another aspect ratio.
    void set_aspect(const std::string& ratio) {
        api->timelapse().post_webcam(
            json{{"uid", uid}, {"aspect_ratio", ratio}}, [](const WebcamInfo&) {},
            [](const MoonrakerError&) {});
    }
    void remove_in_moonraker() {
        WebcamInfo cam;
        cam.uid = uid;
        api->timelapse().delete_webcam(cam, [] {}, [](const MoonrakerError&) {});
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "screen webcam: a renamed entry is refreshed, never duplicated",
                 "[remote_screen][webcam]") {
    WebcamReconcileHarness h;
    h.reconcile(h.item());
    REQUIRE_FALSE(h.uid.empty());
    REQUIRE(h.screens().size() == 1);

    h.rename("Printer screen");
    // The next connect, on a new DHCP lease.
    h.reconcile(h.item("192.168.1.77"));
    h.reconcile(h.item("192.168.1.77"));

    const auto screens = h.screens();
    REQUIRE(screens.size() == 1);
    CHECK(screens[0].name == "Printer screen");
    CHECK(screens[0].target_fps == 5);
    CHECK(screens[0].snapshot_url == "http://192.168.1.77:7130/snapshot.jpg");
    CHECK(screens[0].uid == h.uid);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "screen webcam: deleted in Moonraker stays deleted until sharing is toggled",
                 "[remote_screen][webcam]") {
    WebcamReconcileHarness h;
    h.reconcile(h.item());
    const std::string first_uid = h.uid;
    h.remove_in_moonraker();

    h.reconcile(h.item());
    CHECK(h.screens().empty());
    CHECK(h.uid == first_uid);

    // Sharing off forgets the entry; on again adds a fresh one.
    h.reconcile(std::nullopt);
    CHECK(h.uid.empty());
    h.reconcile(h.item());
    REQUIRE(h.screens().size() == 1);
    CHECK_FALSE(h.uid.empty());
    CHECK(h.uid != first_uid);
}

TEST_CASE_METHOD(LVGLTestFixture, "screen webcam: sharing off removes a renamed entry by uid",
                 "[remote_screen][webcam]") {
    WebcamReconcileHarness h;
    h.reconcile(h.item());
    h.rename("Printer screen");
    h.reconcile(std::nullopt);
    CHECK(h.screens().empty());
    CHECK(h.uid.empty());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "screen webcam: with no uid remembered, an entry showing our snapshot is adopted",
                 "[remote_screen][webcam]") {
    WebcamReconcileHarness h;
    // An entry the user renamed, with no uid stored for it.
    json legacy = *h.item();
    legacy["name"] = "Printer screen";
    std::string legacy_uid;
    h.api->timelapse().post_webcam(
        legacy, [&](const WebcamInfo& cam) { legacy_uid = cam.uid; }, [](const MoonrakerError&) {});
    REQUIRE_FALSE(legacy_uid.empty());

    h.reconcile(h.item());
    const auto screens = h.screens();
    REQUIRE(screens.size() == 1);
    CHECK(screens[0].name == "Printer screen");
    CHECK(h.uid == legacy_uid);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "screen webcam: the aspect ratio is set on create and the user's choice is kept",
                 "[remote_screen][webcam]") {
    WebcamReconcileHarness h;
    h.reconcile(h.item());
    REQUIRE_FALSE(h.uid.empty());
    CHECK(json_util::safe_string(h.stored(), "aspect_ratio", "") == "5:3");

    h.set_aspect("16:9");
    // Later connects refresh the entry by uid, and a screen that reports another size
    // still leaves the user's ratio alone.
    h.reconcile(h.item("192.168.1.77"));
    h.reconcile(remote_screen_webcam_item(kRemoteScreenWebcamName, true, false, "", "192.168.1.77",
                                          7130, 1024, 600));
    CHECK(json_util::safe_string(h.stored(), "aspect_ratio", "") == "16:9");
    CHECK(json_util::safe_string(h.stored(), "snapshot_url", "") ==
          "http://192.168.1.77:7130/snapshot.jpg");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "screen webcam: adopting an existing entry keeps its aspect ratio",
                 "[remote_screen][webcam]") {
    WebcamReconcileHarness h;
    json legacy = *h.item();
    legacy["aspect_ratio"] = "4:3";
    h.api->timelapse().post_webcam(
        legacy, [&](const WebcamInfo& cam) { h.uid = cam.uid; }, [](const MoonrakerError&) {});
    REQUIRE_FALSE(h.uid.empty());
    const std::string legacy_uid = h.uid;
    h.uid.clear();

    h.reconcile(h.item());
    CHECK(h.uid == legacy_uid);
    CHECK(json_util::safe_string(h.stored(), "aspect_ratio", "") == "4:3");
}
