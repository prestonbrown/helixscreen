// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_discovery_machine_name.cpp
 * @brief The vendor model string in machine.system_info must reach detection
 *
 * `printer.info.hostname` is whatever the rootfs was imaged with, and several
 * vendors ship a stock distro default there — a QIDI Q2 reports `linaro-alip`,
 * which names neither the vendor nor the model. The same printer's
 * `machine.system_info` reply carries `system_info.machine_name` = `QIDI@Q2`,
 * the strongest identification signal the machine offers.
 *
 * PrinterDetector::auto_detect() reads exactly one identity string out of
 * PrinterDiscovery — `hostname()` (printer_detector.cpp, auto_detect) — so the
 * model string has to arrive there or it cannot influence detection at all.
 *
 * These drive the REAL discovery sequence against a mock transport that answers
 * printer.info and machine.system_info the way a stock Q2 does.
 */

#include "../lvgl_test_fixture.h"
#include "moonraker_client_mock.h"
#include "printer_detector.h"
#include "printer_discovery.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

namespace {

/**
 * @brief Mock transport that runs the REAL discovery sequence with scripted
 *        printer.info and machine.system_info replies
 *
 * MoonrakerClientMock overrides discover_printer() with its own shortcut, so
 * the base implementation is what exercises MoonrakerDiscoverySequence. Every
 * method other than the two being scripted falls through to the mock's own
 * handler registry, the same technique as TestDiscoveryClient in
 * test_discovery_klippy_gate.cpp.
 */
class IdentityDiscoveryClient : public MoonrakerClientMock {
  public:
    IdentityDiscoveryClient(std::string hostname, std::string machine_name)
        : hostname_(std::move(hostname)), machine_name_(std::move(machine_name)) {}

    void discover_printer_real() {
        MoonrakerClient::discover_printer([]() {}, [](const std::string&) {});
    }

    helix::RequestId send_jsonrpc(const std::string& method, const json& params,
                                  std::function<void(const json&)> cb) override {
        if (auto scripted = scripted_reply(method)) {
            if (cb) {
                cb(*scripted);
            }
            return 0;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(cb));
    }

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params, std::function<void(const json&)> success_cb,
        std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
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
        if (method == "printer.info") {
            return json{{"jsonrpc", "2.0"},
                        {"result",
                         {{"state", "ready"},
                          {"state_message", "Printer is ready"},
                          {"hostname", hostname_},
                          {"app", "Klipper"},
                          {"software_version", "v0.12.0-qidi"}}}};
        }
        if (method == "machine.system_info") {
            json system_info = {
                {"cpu_info", {{"cpu_count", 4}, {"processor", "aarch64"}}},
                {"distribution", {{"name", "Ubuntu 20.04.6 LTS"}, {"id", "ubuntu"}}}};
            if (!machine_name_.empty()) {
                system_info["machine_name"] = machine_name_;
            }
            return json{{"jsonrpc", "2.0"}, {"result", {{"system_info", system_info}}}};
        }
        return std::nullopt;
    }

    std::string hostname_;
    std::string machine_name_;
};

/// The identity string detection is scored against, lowercased for comparison.
std::string discovered_identity(IdentityDiscoveryClient& client) {
    std::string identity = client.hardware().hostname();
    std::transform(identity.begin(), identity.end(), identity.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return identity;
}

} // namespace

TEST_CASE("Discovery carries the machine.system_info model string into identification",
          "[discovery][detector][qidi][q2]") {
    LVGLTestFixture fixture;

    IdentityDiscoveryClient client("linaro-alip", "QIDI@Q2");
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.discover_printer_real();

    const std::string identity = discovered_identity(client);
    INFO("identity string detection runs on: '" << identity << "'");

    // The vendor and the model are separate signals in the database - "qidi"
    // narrows to the range, "q2" picks the machine - so both have to survive.
    CHECK(identity.find("qidi") != std::string::npos);
    CHECK(identity.find("q2") != std::string::npos);
}

TEST_CASE("Discovery keeps the printer.info hostname when no model string is reported",
          "[discovery][detector]") {
    // Most printers report no machine_name at all. Nothing may be invented for
    // them: the hostname stays exactly what printer.info said.
    LVGLTestFixture fixture;

    IdentityDiscoveryClient client("voron-trident-300", /*machine_name=*/"");
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.discover_printer_real();

    CHECK(client.hardware().hostname() == "voron-trident-300");
}
