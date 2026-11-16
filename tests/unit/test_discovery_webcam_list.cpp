// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_discovery_webcam_list.cpp
 * @brief Discovery publishes the WHOLE enabled webcam list, not just its pick.
 *
 * A camera widget can be configured to show one camera by name, so every
 * enabled entry Moonraker lists has to reach PrinterState with the health
 * verdicts discovery attaches (service down, absolute URL unreachable). The
 * auto-pick is derived from that list, so the two can never disagree.
 *
 * Drives the REAL discovery sequence against a mock transport that scripts
 * server.webcams.list, the same technique as test_discovery_machine_name.cpp.
 */

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "webcam_selection.h"

#include <functional>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

namespace {

class WebcamListClient : public MoonrakerClientMock {
  public:
    explicit WebcamListClient(json webcams) : webcams_(std::move(webcams)) {}

    void discover_printer_real() {
        MoonrakerClient::discover_printer([]() {}, [](const std::string&) {});
    }

    helix::RequestId send_jsonrpc(const std::string& method, const json& params,
                                  helix::SharedCallback<void(const json&)> cb) override {
        if (auto scripted = scripted_reply(method)) {
            if (cb) {
                cb(*scripted);
            }
            return 0;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(cb));
    }

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params,
        helix::SharedCallback<void(const json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        if (auto scripted = scripted_reply(method)) {
            if (success_cb) {
                success_cb(*scripted);
            }
            return 0;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

  private:
    std::optional<json> scripted_reply(const std::string& method) const {
        if (method == "server.webcams.list") {
            return json{{"jsonrpc", "2.0"}, {"result", {{"webcams", webcams_}}}};
        }
        return std::nullopt;
    }

    json webcams_;
};

json cam(const std::string& name, const std::string& service, const std::string& stream,
         const std::string& snapshot, bool enabled = true) {
    return json{{"name", name},         {"service", service},
                {"stream_url", stream}, {"snapshot_url", snapshot},
                {"enabled", enabled},   {"target_fps", 15}};
}

struct DiscoveryRun {
    explicit DiscoveryRun(json webcams) : client(std::move(webcams)) {
        client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
        client.discover_printer_real();
        helix::ui::UpdateQueue::instance().drain();
    }
    ~DiscoveryRun() {
        // The global PrinterState outlives this test; leave it as "no webcam".
        get_printer_state().capabilities_state().set_webcam_available(false);
        helix::ui::UpdateQueue::instance().drain();
    }
    WebcamListClient client;
};

} // namespace

TEST_CASE("Discovery keeps every enabled webcam and derives the auto-pick from the list",
          "[discovery][webcam][camera]") {
    LVGLTestFixture fixture;
    DiscoveryRun run(json::array({
        cam("Chamber", "webrtc-go2rtc", "/webrtc/stream.html", "/webrtc/frame.jpeg"),
        cam("Nozzle", "mjpegstreamer", "/webcam/?action=stream", "/webcam/?action=snapshot"),
        cam("Old", "mjpegstreamer", "/old/?action=stream", "/old/?action=snapshot",
            /*enabled=*/false),
        cam("Bed", "ustreamer", "/bed/?action=stream", "/bed/?action=snapshot"),
    }));

    auto& ps = get_printer_state();
    const auto& cams = ps.capabilities_state().get_webcams();
    REQUIRE(cams.size() == 3); // the disabled one is not offered at all
    CHECK(cams[0].name == "Chamber");
    CHECK(cams[1].name == "Nozzle");
    CHECK(cams[2].name == "Bed");
    CHECK(cams[2].service == "ustreamer");
    CHECK(cams[0].unavailable_reason.empty());

    // Auto-pick: the first MJPEG-family entry, not the first entry. (The
    // subjects behind has_webcam()/webcam_count are pinned on a standalone
    // PrinterCapabilitiesState in test_printer_state_webcams.cpp; the global
    // PrinterState's subjects are not initialized under this fixture.)
    CHECK(ps.capabilities_state().get_webcam_stream_url() == "/webcam/?action=stream");
    CHECK(ps.capabilities_state().get_webcam_snapshot_url() == "/webcam/?action=snapshot");
}

TEST_CASE("webcam discovery skips HelixScreen's own screen entry",
          "[discovery][webcam][remote_screen]") {
    LVGLTestFixture fixture;
    SECTION("with a real camera beside it, the real camera is picked") {
        DiscoveryRun run(json::array({
            cam(helix::kRemoteScreenWebcamName, "mjpegstreamer-adaptive",
                "/helixscreen/screen/snapshot.jpg", "/helixscreen/screen/snapshot.jpg"),
            cam("Nozzle", "mjpegstreamer", "/webcam/?action=stream", "/webcam/?action=snapshot"),
        }));
        auto& caps = get_printer_state().capabilities_state();
        REQUIRE(caps.get_webcams().size() == 1);
        CHECK(caps.get_webcams()[0].name == "Nozzle");
        CHECK(caps.get_webcam_snapshot_url() == "/webcam/?action=snapshot");
    }
    SECTION("alone, no camera is offered") {
        DiscoveryRun run(json::array({
            cam(helix::kRemoteScreenWebcamName, "mjpegstreamer-adaptive",
                "/helixscreen/screen/snapshot.jpg", "/helixscreen/screen/snapshot.jpg"),
        }));
        auto& caps = get_printer_state().capabilities_state();
        CHECK(caps.get_webcams().empty());
        CHECK(caps.get_webcam_snapshot_url() != "/helixscreen/screen/snapshot.jpg");
    }
}

TEST_CASE("Mock webcam spec parses into a Moonraker-shaped list", "[mock][webcam][camera]") {
    auto cams = MoonrakerClientMock::parse_mock_webcams("Nozzle,Bed,Chamber:webrtc-go2rtc");
    REQUIRE(cams.size() == 3);
    CHECK(cams[0].name == "Nozzle");
    CHECK(cams[0].service == "mjpegstreamer");
    CHECK(cams[0].stream_url == "/webcam/?action=stream");
    CHECK(cams[1].stream_url == "/webcam2/?action=stream");
    CHECK(cams[2].name == "Chamber");
    CHECK(cams[2].service == "webrtc-go2rtc");
    CHECK(cams[2].snapshot_url == "/webcam3/?action=snapshot");
    CHECK(MoonrakerClientMock::parse_mock_webcams(nullptr).empty());
    CHECK(MoonrakerClientMock::parse_mock_webcams("").empty());
}

TEST_CASE("Discovery marks an unreachable absolute snapshot URL and moves on",
          "[discovery][webcam][camera]") {
    LVGLTestFixture fixture;
    // Port 9 answers nothing on any host: the probe is refused immediately.
    DiscoveryRun run(json::array({
        cam("Stale", "mjpegstreamer", "http://127.0.0.1:9/?action=stream",
            "http://127.0.0.1:9/?action=snapshot"),
        cam("Nozzle", "mjpegstreamer", "/webcam/?action=stream", "/webcam/?action=snapshot"),
    }));

    auto& ps = get_printer_state();
    const auto& cams = ps.capabilities_state().get_webcams();
    REQUIRE(cams.size() == 2);
    // Still listed (a picker can show it as unavailable), but ruled out.
    CHECK(cams[0].name == "Stale");
    CHECK(cams[0].unavailable_reason.rfind("unreachable", 0) == 0);
    CHECK(cams[1].unavailable_reason.empty());
    CHECK(ps.capabilities_state().get_webcam_stream_url() == "/webcam/?action=stream");
}
