// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "led/led_controller.h"
#include "led/led_device_page.h"
#include "led/led_devices.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <set>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::led;

namespace {

LedStripInfo strip(const std::string& id, LedBackendType b, bool color = true, bool white = true) {
    LedStripInfo s;
    s.id = id;
    s.name = id;
    s.backend = b;
    s.supports_color = color;
    s.supports_white = white;
    return s;
}

/// Records the object names of every printer.objects.query it is sent.
struct QueryRecordingClient : public MoonrakerClientMock {
    using MoonrakerClientMock::MoonrakerClientMock;
    using MoonrakerClientMock::send_jsonrpc;

    std::vector<std::set<std::string>> queries;

    helix::RequestId send_jsonrpc(const std::string& method, const json& params,
                                  helix::SharedCallback<void(const json&)> cb) override {
        if (method == "printer.objects.query" && params.contains("objects")) {
            std::set<std::string> names;
            for (auto it = params["objects"].begin(); it != params["objects"].end(); ++it) {
                names.insert(it.key());
            }
            queries.push_back(std::move(names));
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(cb));
    }
};

struct DeviceStateFixture : public LVGLTestFixture {
    QueryRecordingClient client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;

    DeviceStateFixture() {
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        auto& ctrl = LedController::instance();
        ctrl.deinit();
        ctrl.init(api.get(), &client);
        ctrl.native().add_strip(strip("neopixel a", LedBackendType::NATIVE));
        ctrl.native().add_strip(strip("neopixel b", LedBackendType::NATIVE));
        ctrl.native().add_strip(strip("led w", LedBackendType::NATIVE, false, false));
        ctrl.output_pin().add_pin(
            strip("output_pin enc", LedBackendType::OUTPUT_PIN, false, false));
        ctrl.wled().add_strip(strip("printer_led", LedBackendType::WLED, false, false));
        LedMacroInfo m;
        m.display_name = "Lamp";
        m.type = MacroLedType::ON_OFF;
        m.on_macro = "LIGHTS_ON";
        m.off_macro = "LIGHTS_OFF";
        ctrl.set_configured_macros({m});
        ctrl.rebuild_macro_backend();
    }
    ~DeviceStateFixture() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        LedController::instance().deinit();
    }
    int version() {
        return lv_subject_get_int(LedController::instance().get_led_state_version_subject());
    }
};

} // namespace

TEST_CASE_METHOD(DeviceStateFixture, "device_state: native strip reads from status",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    CHECK(ctrl.device_state("neopixel a").power == PowerState::Unknown);
    ctrl.update_from_status({{"neopixel a", {{"color_data", {{1.0, 0.0, 0.0, 0.0}}}}}});
    const auto s = ctrl.device_state("neopixel a");
    CHECK(s.power == PowerState::On);
    CHECK(s.brightness == 100);
    CHECK(s.rgb == 0xFF0000);
    CHECK(s.has_rgb);
    ctrl.update_from_status({{"neopixel a", {{"color_data", {{0.0, 0.0, 0.0, 0.0}}}}}});
    CHECK(ctrl.device_state("neopixel a").power == PowerState::Off);
}

TEST_CASE_METHOD(DeviceStateFixture, "device_state: a printer switch forgets the old state",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    ctrl.update_from_status({{"neopixel a", {{"color_data", {{1.0, 0.0, 0.0, 0.0}}}}}});
    ctrl.update_from_status({{"output_pin enc", {{"value", 0.3}}}});
    ctrl.deinit();
    ctrl.init(api.get(), &client);
    ctrl.native().add_strip(strip("neopixel a", LedBackendType::NATIVE));
    ctrl.output_pin().add_pin(strip("output_pin enc", LedBackendType::OUTPUT_PIN, false, false));
    CHECK(ctrl.device_state("neopixel a").power == PowerState::Unknown);
    CHECK(ctrl.device_state("output_pin enc").power == PowerState::Unknown);
}

TEST_CASE_METHOD(DeviceStateFixture, "device_state: a re-discovery keeps the known state",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    PrinterDiscovery discovery;
    discovery.parse_objects(nlohmann::json::array({"neopixel a", "output_pin case_lamp"}));
    ctrl.discover_from_hardware(discovery);
    ctrl.update_from_status({{"neopixel a", {{"color_data", {{1.0, 0.0, 0.0, 0.0}}}}}});
    ctrl.update_from_status({{"output_pin case_lamp", {{"value", 0.3}}}});
    ctrl.discover_from_hardware(discovery);
    CHECK(ctrl.device_state("neopixel a").power == PowerState::On);
    CHECK(ctrl.device_state("output_pin case_lamp").power == PowerState::On);
}

TEST_CASE_METHOD(DeviceStateFixture, "device_state: a white-only strip has no hue",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    ctrl.update_from_status({{"led w", {{"color_data", {{0.0, 0.0, 0.0, 0.5}}}}}});
    const auto s = ctrl.device_state("led w");
    CHECK(s.power == PowerState::On);
    CHECK(s.brightness == 50);
    CHECK_FALSE(s.has_rgb);
}

TEST_CASE_METHOD(DeviceStateFixture, "device_state: output pin, WLED, macro", "[led][state]") {
    auto& ctrl = LedController::instance();
    CHECK(ctrl.device_state("output_pin enc").power == PowerState::Unknown);
    ctrl.update_from_status({{"output_pin enc", {{"value", 0.3}}}});
    CHECK(ctrl.device_state("output_pin enc").power == PowerState::On);
    CHECK(ctrl.device_state("output_pin enc").brightness == 30);

    CHECK(ctrl.device_state("printer_led").power == PowerState::Unknown);
    ctrl.wled().update_strip_state("printer_led", WledStripState{true, 128, -1});
    CHECK(ctrl.device_state("printer_led").power == PowerState::On);
    CHECK(ctrl.device_state("printer_led").brightness == 50);

    CHECK(ctrl.device_state("macro:Lamp").power == PowerState::Unknown);
}

TEST_CASE_METHOD(DeviceStateFixture, "update_from_status bumps the version only for LED objects",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    const int before = version();
    ctrl.update_from_status({{"extruder", {{"temperature", 200.0}}}});
    CHECK(version() == before);
    ctrl.update_from_status({{"neopixel b", {{"color_data", {{0.0, 1.0, 0.0, 0.0}}}}}});
    CHECK(version() == before + 1);
}

TEST_CASE_METHOD(DeviceStateFixture, "set_power reaches only the ids it is given", "[led][state]") {
    auto& ctrl = LedController::instance();
    ctrl.set_power({"neopixel a"}, true);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(ctrl.native().has_strip_color("neopixel a"));
    CHECK_FALSE(ctrl.native().has_strip_color("neopixel b"));
}

TEST_CASE_METHOD(DeviceStateFixture, "set_power reads back exactly the Klipper devices it touched",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    client.queries.clear();
    ctrl.set_power({"neopixel a", "output_pin enc", "macro:Lamp", "printer_led"}, true);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(client.queries.size() == 1);
    CHECK(client.queries[0] == std::set<std::string>{"neopixel a", "output_pin enc"});
}

TEST_CASE_METHOD(DeviceStateFixture, "toggle_power turns off a strip that is on", "[led][state]") {
    auto& ctrl = LedController::instance();
    ctrl.update_from_status({{"neopixel a", {{"color_data", {{1.0, 1.0, 1.0, 0.0}}}}}});
    CHECK_FALSE(ctrl.toggle_power({"neopixel a"}));
}

TEST_CASE_METHOD(DeviceStateFixture, "toggle_power alternates an unreadable macro",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    CHECK(ctrl.toggle_power({"macro:Lamp"}));
    CHECK_FALSE(ctrl.toggle_power({"macro:Lamp"}));
}

TEST_CASE_METHOD(DeviceStateFixture, "all_devices appends PRESET macros after the switchable ones",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    LedMacroInfo p;
    p.display_name = "Party";
    p.type = MacroLedType::PRESET;
    p.presets = {"LED_PARTY"};
    auto macros = ctrl.configured_macros();
    macros.push_back(p);
    ctrl.set_configured_macros(macros);
    const auto devices = ctrl.all_devices();
    REQUIRE_FALSE(devices.empty());
    CHECK(devices.back().id == "macro:Party");
    const auto sw = ctrl.switchable_ids();
    CHECK(std::find(sw.begin(), sw.end(), "macro:Party") == sw.end());
}

TEST_CASE_METHOD(DeviceStateFixture, "device_state: a lit W channel counts toward the hue",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    // Warm white on RGBW: a small tint under a full W reads as a pale warm white,
    // not as the tint alone.
    ctrl.update_from_status({{"neopixel a", {{"color_data", {{0.35, 0.12, 0.0, 1.0}}}}}});
    CHECK(ctrl.device_state("neopixel a").rgb == output_rgb(0.35, 0.12, 0.0, 1.0));
    ctrl.update_from_status({{"neopixel a", {{"color_data", {{0.0, 0.0, 0.0, 0.4}}}}}});
    CHECK(ctrl.device_state("neopixel a").rgb == 0xFFFFFFu);
}

TEST_CASE_METHOD(DeviceStateFixture, "set_power on a WLED strip bumps the state version",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    int before = version();
    ctrl.set_power({"printer_led"}, true);
    CHECK(version() > before);
    CHECK(ctrl.device_state("printer_led").power == PowerState::On);

    before = version();
    ctrl.set_power({"printer_led"}, false);
    CHECK(version() > before);
    CHECK(ctrl.device_state("printer_led").power == PowerState::Off);
}
