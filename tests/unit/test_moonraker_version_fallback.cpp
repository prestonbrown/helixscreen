// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_moonraker_version_fallback.cpp
 * @brief Moonraker's version when server.info reports a placeholder
 *
 * A Moonraker checkout without release tags (the k2-improvements install on a
 * K2 Plus) answers server.info with moonraker_version "?", while
 * machine.update.status still names the checkout. The responses below keep
 * that install's shape with everything but the version fields dropped.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "printer_versions_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

json tagless_moonraker_update_status() {
    return json::parse(R"({
        "busy": false,
        "version_info": {
            "moonraker": {
                "branch": "master", "channel": "stable", "configured_type": "git_repo",
                "current_hash": "9e676eba6b02661a4dfa3ec6e7ac3f3504498e6d",
                "full_version_string": "v0.0.0-1-g9e676eba-inferred",
                "is_valid": true, "name": "moonraker", "owner": "arksine",
                "remote_version": "v0.0.0-1", "rollback_version": "v0.0.0-1",
                "version": "v0.0.0-1"
            },
            "klipper": {
                "current_hash": "25d8714b9271bfaf84e403971038c30c758029ed",
                "version": "v0.0.0-2"
            }
        }
    })");
}

} // namespace

TEST_CASE("Moonraker version from update_manager", "[versions][moonraker_version]") {
    SECTION("a tagless checkout reads as its short commit hash") {
        CHECK(moonraker_version_from_update_status(tagless_moonraker_update_status()) == "9e676eb");
    }

    SECTION("a tagged checkout reads as its version") {
        json status = tagless_moonraker_update_status();
        status["version_info"]["moonraker"]["version"] = "v0.9.3-12";
        CHECK(moonraker_version_from_update_status(status) == "v0.9.3-12");
    }

    SECTION("a placeholder version with no hash reads as nothing") {
        json status = tagless_moonraker_update_status();
        status["version_info"]["moonraker"]["version"] = "?";
        status["version_info"]["moonraker"].erase("current_hash");
        CHECK(moonraker_version_from_update_status(status).empty());
    }

    SECTION("no moonraker entry reads as nothing, not another component's version") {
        json status = tagless_moonraker_update_status();
        status["version_info"].erase("moonraker");
        CHECK(moonraker_version_from_update_status(status).empty());
        CHECK(moonraker_version_from_update_status(json::object()).empty());
    }
}

TEST_CASE("Placeholder versions", "[versions][moonraker_version]") {
    CHECK(is_placeholder_version(""));
    CHECK(is_placeholder_version("  "));
    CHECK(is_placeholder_version("?"));
    CHECK(is_placeholder_version(" Unknown "));
    CHECK_FALSE(is_placeholder_version("v0.9.3"));
    CHECK_FALSE(is_placeholder_version("9e676eb"));
}

namespace {

/// Runs the real discovery sequence; server.info reports "?" and
/// machine.update.status answers with the tagless checkout.
class TaglessMoonrakerClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    void discover_printer_real(helix::SharedCallback<void()> on_complete,
                               helix::SharedCallback<void(const std::string&)> on_error) {
        MoonrakerClient::discover_printer(on_complete, on_error);
    }

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params,
        helix::SharedCallback<void(const json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        if (method == "machine.update.status" && success_cb) {
            success_cb(json{{"result", tagless_moonraker_update_status()}});
            return 0;
        }
        if (method == "server.info" && success_cb) {
            return MoonrakerClientMock::send_jsonrpc(
                method, params,
                [success_cb](const json& response) {
                    json patched = response;
                    patched["result"]["moonraker_version"] = "?";
                    success_cb(patched);
                },
                error_cb, timeout_ms, silent, intent);
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, success_cb, error_cb, timeout_ms,
                                                 silent, intent);
    }
};

} // namespace

TEST_CASE("Discovery reads Moonraker's version from update_manager when server.info has none",
          "[versions][moonraker_version][discovery]") {
    LVGLTestFixture fixture;
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    TaglessMoonrakerClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);

    bool completed = false;
    client.discover_printer_real([&completed]() { completed = true; }, [](const std::string&) {});
    REQUIRE(completed);
    CHECK(client.hardware().moonraker_version() == "9e676eb");

    UpdateQueue::instance().drain();
    CHECK(std::string(lv_subject_get_string(
              state.versions_state().get_moonraker_version_subject())) == "9e676eb");
}
