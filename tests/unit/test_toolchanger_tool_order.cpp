// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_tool_order.cpp
 * @brief A tool's slot is its Klipper tool number, so the tool list follows the numbers.
 *
 * Everything downstream of discovery takes a tool's slot from its position in
 * tool_names(): the backend selects slot i by naming tool_names()[i], the switcher
 * labels it "T<i>", and ToolState pairs it with the i-th extruder. Ordered by
 * name, `[tool Left]` numbered 1 lands in slot 0, so tapping it mounts the tool
 * Klipper numbers 0.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "core/update_queue.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"

#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using helix::PrinterDiscovery;

namespace {

PrinterDiscovery changer_with(const std::vector<std::string>& tools) {
    json objects = json::array({"toolchanger", "extruder", "heater_bed", "toolhead"});
    for (const auto& t : tools) {
        objects.push_back("tool " + t);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

std::vector<std::string> names(const PrinterDiscovery& hw) {
    return hw.tool_names();
}

} // namespace

TEST_CASE("tools numbered past nine keep numeric order before any status arrives",
          "[toolchanger][tool_order]") {
    std::vector<std::string> tools;
    for (int i = 11; i >= 0; --i) {
        tools.push_back("T" + std::to_string(i));
    }
    const auto hw = changer_with(tools);

    REQUIRE(hw.tool_names().size() == 12);
    for (int i = 0; i < 12; ++i) {
        CHECK(hw.tool_names()[static_cast<size_t>(i)] == "T" + std::to_string(i));
    }
}

TEST_CASE("named tools follow Klipper's tool numbers, not their names",
          "[toolchanger][tool_order]") {
    auto hw = changer_with({"Left", "Right"});
    REQUIRE(names(hw) == std::vector<std::string>{"Left", "Right"});

    // klipper-toolchanger: tool_numbers ascending, tool_names in the same order.
    hw.order_tools_by_number(
        {{"tool_numbers", {0, 1}}, {"tool_names", {"tool Right", "tool Left"}}});

    CHECK(names(hw) == std::vector<std::string>{"Right", "Left"});
}

TEST_CASE("tools Klipper leaves unnumbered follow the numbered ones", "[toolchanger][tool_order]") {
    auto hw = changer_with({"A", "B", "C"});

    hw.order_tools_by_number({{"tool_numbers", {0, 1}}, {"tool_names", {"tool C", "tool A"}}});

    CHECK(names(hw) == std::vector<std::string>{"C", "A", "B"});
}

TEST_CASE("a toolchanger status that cannot order the tools leaves them as discovered",
          "[toolchanger][tool_order]") {
    const std::vector<std::string> discovered{"Left", "Right"};

    SECTION("no tool_names") {
        auto hw = changer_with(discovered);
        hw.order_tools_by_number({{"tool_numbers", {0, 1}}});
        CHECK(names(hw) == discovered);
    }
    SECTION("not an object") {
        auto hw = changer_with(discovered);
        hw.order_tools_by_number(json());
        CHECK(names(hw) == discovered);
    }
    SECTION("names no discovered tool, or are not strings") {
        auto hw = changer_with(discovered);
        hw.order_tools_by_number({{"tool_names", {"tool Middle", 7, nullptr}}});
        CHECK(names(hw) == discovered);
    }
    SECTION("a name listed twice still yields each tool once") {
        auto hw = changer_with(discovered);
        hw.order_tools_by_number({{"tool_names", {"tool Right", "tool Right", "tool Left"}}});
        CHECK(names(hw) == std::vector<std::string>{"Right", "Left"});
    }
}

namespace {

/// Runs the real discovery sequence over the mock transport, with the
/// subscription's initial toolchanger status replaced by @p tool_names.
class ReorderingClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;
    using MoonrakerClientMock::send_jsonrpc;

    helix::RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 helix::SharedCallback<void(const json&)> success_cb,
                 helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms,
                 bool silent,
                 std::optional<helix::rpc_error_policy::CallerIntent> intent) override {
        if (method == "printer.objects.subscribe") {
            subscribed = params.value("objects", json::object());
            if (tool_names) {
                auto names = *tool_names;
                success_cb = [names, inner = std::move(success_cb)](const json& response) {
                    json patched = response;
                    seen_toolchanger = patched["result"]["status"].contains("toolchanger");
                    patched["result"]["status"]["toolchanger"]["tool_names"] = names;
                    inner(patched);
                };
            } else {
                success_cb = [inner = std::move(success_cb)](const json& response) {
                    if (response.contains("result") && response["result"].contains("status")) {
                        reported_status = response["result"]["status"];
                    }
                    inner(response);
                };
            }
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, success_cb, error_cb, timeout_ms,
                                                 silent, intent);
    }

    bool discover_real() {
        bool done = false;
        MoonrakerClient::discover_printer([&done]() { done = true; }, [](const std::string&) {});
        helix::ui::UpdateQueue::instance().drain();
        return done;
    }

    std::optional<json> tool_names;
    json subscribed;
    static inline bool seen_toolchanger = false;
    static inline json reported_status;
};

} // namespace

TEST_CASE("discovery orders the tools by the numbers in Klipper's first status",
          "[toolchanger][tool_order]") {
    LVGLTestFixture fixture;
    helix::ScopedEnv ams("HELIX_MOCK_AMS", "toolchanger");

    ReorderingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.tool_names = json::array({"tool T3", "tool T2", "tool T1", "tool T0"});
    ReorderingClient::seen_toolchanger = false;
    REQUIRE(client.discover_real());

    REQUIRE(client.subscribed.contains("toolchanger"));
    CHECK(client.subscribed["toolchanger"] ==
          json::array({"status", "tool_number", "tool_numbers", "tool_names"}));
    REQUIRE(ReorderingClient::seen_toolchanger);
    CHECK(client.hardware().tool_names() == std::vector<std::string>{"T3", "T2", "T1", "T0"});
}

TEST_CASE("the mock toolchanger reports every tool in number order", "[toolchanger][tool_order]") {
    LVGLTestFixture fixture;
    helix::ScopedEnv ams("HELIX_MOCK_AMS", "toolchanger");

    ReorderingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    ReorderingClient::reported_status = json();
    REQUIRE(client.discover_real());

    const json& tc = ReorderingClient::reported_status["toolchanger"];
    CHECK(tc["tool_numbers"] == json::array({0, 1, 2, 3}));
    CHECK(tc["tool_names"] == json::array({"tool T0", "tool T1", "tool T2", "tool T3"}));
    CHECK(client.hardware().tool_names() == std::vector<std::string>{"T0", "T1", "T2", "T3"});
}
