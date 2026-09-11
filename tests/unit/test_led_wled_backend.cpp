// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../src/led/led_wled_json.h"
#include "../helix_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "led/led_controller.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::led;

TEST_CASE("WledBackend: set_on with null API calls error callback", "[led][wled]") {
    WledBackend backend;

    bool error_called = false;
    backend.set_on("test_strip", nullptr, [&](const std::string& err) {
        error_called = true;
        REQUIRE(err.find("no API") != std::string::npos);
    });
    REQUIRE(error_called);
}

TEST_CASE("WledBackend: set_off with null API calls error callback", "[led][wled]") {
    WledBackend backend;

    bool error_called = false;
    backend.set_off("test_strip", nullptr, [&](const std::string& err) {
        error_called = true;
        REQUIRE(err.find("no API") != std::string::npos);
    });
    REQUIRE(error_called);
}

TEST_CASE("WledBackend: set_brightness with null API calls error callback", "[led][wled]") {
    WledBackend backend;

    bool error_called = false;
    backend.set_brightness("test_strip", 50, nullptr, [&](const std::string& err) {
        error_called = true;
        REQUIRE(err.find("no API") != std::string::npos);
    });
    REQUIRE(error_called);
}

TEST_CASE("WledBackend: set_preset with null API calls error callback", "[led][wled]") {
    WledBackend backend;

    bool error_called = false;
    backend.set_preset("test_strip", 1, nullptr, [&](const std::string& err) {
        error_called = true;
        REQUIRE(err.find("no API") != std::string::npos);
    });
    REQUIRE(error_called);
}

TEST_CASE("WledBackend: toggle with null API calls error callback", "[led][wled]") {
    WledBackend backend;

    bool error_called = false;
    backend.toggle("test_strip", nullptr, [&](const std::string& err) {
        error_called = true;
        REQUIRE(err.find("no API") != std::string::npos);
    });
    REQUIRE(error_called);
}

TEST_CASE("WledBackend: null callbacks don't crash with null API", "[led][wled]") {
    WledBackend backend;
    // All callbacks null, api_ null -- should not crash
    backend.set_on("test", nullptr, nullptr);
    backend.set_off("test", nullptr, nullptr);
    backend.set_brightness("test", 50, nullptr, nullptr);
    backend.set_preset("test", 1, nullptr, nullptr);
    backend.toggle("test", nullptr, nullptr);
}

TEST_CASE("WledBackend: type is WLED", "[led][wled]") {
    WledBackend backend;
    REQUIRE(backend.type() == LedBackendType::WLED);
}

TEST_CASE("WledBackend: strips are discoverable with correct backend type", "[led][wled]") {
    WledBackend backend;

    LedStripInfo strip;
    strip.name = "Printer LED";
    strip.id = "printer_led";
    strip.backend = LedBackendType::WLED;
    strip.supports_color = true;
    strip.supports_white = true;
    backend.add_strip(strip);

    REQUIRE(backend.strips()[0].backend == LedBackendType::WLED);
    REQUIRE(backend.strips()[0].id == "printer_led");
}

TEST_CASE("WledBackend: multiple strip discovery", "[led][wled]") {
    WledBackend backend;

    LedStripInfo s1;
    s1.name = "Printer";
    s1.id = "printer_led";
    s1.backend = LedBackendType::WLED;
    s1.supports_color = true;
    s1.supports_white = true;
    backend.add_strip(s1);

    LedStripInfo s2;
    s2.name = "Enclosure";
    s2.id = "enclosure_led";
    s2.backend = LedBackendType::WLED;
    s2.supports_color = true;
    s2.supports_white = false;
    backend.add_strip(s2);

    REQUIRE(backend.strips().size() == 2);
    REQUIRE(backend.strips()[0].id == "printer_led");
    REQUIRE(backend.strips()[1].id == "enclosure_led");
}

TEST_CASE("WledBackend: strip management", "[led][wled]") {
    WledBackend backend;

    REQUIRE(!backend.is_available());
    REQUIRE(backend.strips().empty());

    LedStripInfo strip;
    strip.name = "WLED Strip";
    strip.id = "wled_living_room";
    strip.backend = LedBackendType::WLED;
    strip.supports_color = true;
    strip.supports_white = false;

    backend.add_strip(strip);
    REQUIRE(backend.is_available());
    REQUIRE(backend.strips().size() == 1);
    REQUIRE(backend.strips()[0].name == "WLED Strip");
    REQUIRE(backend.strips()[0].id == "wled_living_room");

    // Add a second strip
    LedStripInfo strip2;
    strip2.name = "Bedroom LEDs";
    strip2.id = "wled_bedroom";
    strip2.backend = LedBackendType::WLED;
    strip2.supports_color = true;
    strip2.supports_white = true;

    backend.add_strip(strip2);
    REQUIRE(backend.strips().size() == 2);

    backend.clear();
    REQUIRE(!backend.is_available());
    REQUIRE(backend.strips().empty());
}

// ============================================================================
// Strip State Management
// ============================================================================

TEST_CASE("WledBackend: default strip state is off with full brightness", "[led][wled]") {
    WledBackend backend;
    auto state = backend.get_strip_state("unknown_strip");
    REQUIRE(!state.is_on);
    REQUIRE(state.brightness == 255);
    REQUIRE(state.active_preset == -1);
}

TEST_CASE("WledBackend: update and get strip state", "[led][wled]") {
    WledBackend backend;

    WledStripState new_state;
    new_state.is_on = true;
    new_state.brightness = 128;
    new_state.active_preset = 3;
    backend.update_strip_state("test_led", new_state);

    auto state = backend.get_strip_state("test_led");
    REQUIRE(state.is_on);
    REQUIRE(state.brightness == 128);
    REQUIRE(state.active_preset == 3);
}

TEST_CASE("WledBackend: clear resets strip states", "[led][wled]") {
    WledBackend backend;

    WledStripState new_state{true, 100, 2};
    backend.update_strip_state("test_led", new_state);
    backend.clear();

    auto state = backend.get_strip_state("test_led");
    REQUIRE(!state.is_on);
    REQUIRE(state.active_preset == -1);
}

TEST_CASE("WledBackend: multiple strip states are independent", "[led][wled]") {
    WledBackend backend;

    backend.update_strip_state("strip_a", {true, 200, 1});
    backend.update_strip_state("strip_b", {false, 50, 5});

    auto a = backend.get_strip_state("strip_a");
    auto b = backend.get_strip_state("strip_b");

    REQUIRE(a.is_on);
    REQUIRE(a.brightness == 200);
    REQUIRE(a.active_preset == 1);

    REQUIRE(!b.is_on);
    REQUIRE(b.brightness == 50);
    REQUIRE(b.active_preset == 5);
}

// ============================================================================
// Strip Address Management
// ============================================================================

TEST_CASE("WledBackend: set and get strip address", "[led][wled]") {
    WledBackend backend;

    backend.set_strip_address("printer_led", "192.168.1.50");
    REQUIRE(backend.get_strip_address("printer_led") == "192.168.1.50");
}

TEST_CASE("WledBackend: unknown strip returns empty address", "[led][wled]") {
    WledBackend backend;
    REQUIRE(backend.get_strip_address("nonexistent").empty());
}

TEST_CASE("WledBackend: clear removes addresses", "[led][wled]") {
    WledBackend backend;
    backend.set_strip_address("printer_led", "192.168.1.50");
    backend.clear();
    REQUIRE(backend.get_strip_address("printer_led").empty());
}

TEST_CASE("WledBackend: overwrite strip address", "[led][wled]") {
    WledBackend backend;
    backend.set_strip_address("printer_led", "192.168.1.50");
    backend.set_strip_address("printer_led", "10.0.0.100");
    REQUIRE(backend.get_strip_address("printer_led") == "10.0.0.100");
}

// ============================================================================
// Preset Management
// ============================================================================

TEST_CASE("WledBackend: set and get presets", "[led][wled]") {
    WledBackend backend;

    std::vector<WledPresetInfo> presets = {{1, "Warm White"}, {2, "Rainbow"}, {3, "Fire"}};
    backend.set_strip_presets("printer_led", presets);

    const auto& result = backend.get_strip_presets("printer_led");
    REQUIRE(result.size() == 3);
    REQUIRE(result[0].id == 1);
    REQUIRE(result[0].name == "Warm White");
    REQUIRE(result[1].id == 2);
    REQUIRE(result[1].name == "Rainbow");
    REQUIRE(result[2].id == 3);
    REQUIRE(result[2].name == "Fire");
}

TEST_CASE("WledBackend: unknown strip returns empty presets", "[led][wled]") {
    WledBackend backend;
    const auto& result = backend.get_strip_presets("unknown");
    REQUIRE(result.empty());
}

TEST_CASE("WledBackend: clear removes presets", "[led][wled]") {
    WledBackend backend;
    backend.set_strip_presets("test", {{1, "Test"}});
    backend.clear();
    REQUIRE(backend.get_strip_presets("test").empty());
}

TEST_CASE("WledBackend: overwrite presets", "[led][wled]") {
    WledBackend backend;
    backend.set_strip_presets("test", {{1, "First"}, {2, "Second"}});
    backend.set_strip_presets("test", {{10, "New Preset"}});

    const auto& result = backend.get_strip_presets("test");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].id == 10);
    REQUIRE(result[0].name == "New Preset");
}

TEST_CASE("WledBackend: per-strip presets are independent", "[led][wled]") {
    WledBackend backend;
    backend.set_strip_presets("strip_a", {{1, "Warm White"}, {2, "Rainbow"}});
    backend.set_strip_presets("strip_b", {{1, "Bright White"}});

    REQUIRE(backend.get_strip_presets("strip_a").size() == 2);
    REQUIRE(backend.get_strip_presets("strip_b").size() == 1);
    REQUIRE(backend.get_strip_presets("strip_b")[0].name == "Bright White");
}

// ============================================================================
// fetch_presets_from_device (without HTTP, just behavior check)
// ============================================================================

TEST_CASE("WledBackend: fetch_presets_from_device with no address calls on_complete",
          "[led][wled]") {
    WledBackend backend;
    bool completed = false;
    backend.fetch_presets_from_device("test_strip", [&]() { completed = true; });
    REQUIRE(completed);
}

TEST_CASE("WledBackend: fetch_presets_from_device with address calls on_complete", "[led][wled]") {
    WledBackend backend;
    backend.set_strip_address("test_strip", "192.168.1.50");
    bool completed = false;
    backend.fetch_presets_from_device("test_strip", [&]() { completed = true; });
    REQUIRE(completed);
}

// ============================================================================
// poll_status (without API, behavior check)
// ============================================================================

TEST_CASE("WledBackend: poll_status with no API calls on_complete", "[led][wled]") {
    WledBackend backend;
    bool completed = false;
    backend.poll_status([&]() { completed = true; });
    REQUIRE(completed);
}

// ============================================================================
// wled_strip_map() — /machine/wled/strips envelope unwrapping (#1241)
// ============================================================================

TEST_CASE("wled_strip_map: unwraps the real Moonraker result.strips envelope",
          "[led][wled][1241]") {
    const nlohmann::json payload = {
        {"result",
         {{"strips",
           {{"TopLight",
             {{"strip", "TopLight"}, {"status", "off"}, {"brightness", 255}, {"preset", -1}}}}}}}};

    const auto& map = helix::led::detail::wled_strip_map(payload);
    REQUIRE(map.is_object());
    REQUIRE(map.size() == 1);
    REQUIRE(map.contains("TopLight"));
    REQUIRE(map.begin().key() == "TopLight");
    // The bug: iterating result directly yielded the wrapper key.
    REQUIRE(!map.contains("strips"));
}

TEST_CASE("wled_strip_map: handles a flat result map with no strips wrapper", "[led][wled][1241]") {
    const nlohmann::json payload = {
        {"result", {{"printer_led", {{"strip", "printer_led"}, {"status", "on"}}}}}};

    const auto& map = helix::led::detail::wled_strip_map(payload);
    REQUIRE(map.size() == 1);
    REQUIRE(map.contains("printer_led"));
}

TEST_CASE("wled_strip_map: handles a payload with no result wrapper", "[led][wled][1241]") {
    const nlohmann::json payload = {
        {"strips", {{"TopLight", {{"strip", "TopLight"}, {"status", "on"}}}}}};

    const auto& map = helix::led::detail::wled_strip_map(payload);
    REQUIRE(map.size() == 1);
    REQUIRE(map.contains("TopLight"));
}

TEST_CASE("wled_strip_map: a strip legitimately named 'strips' resolves once",
          "[led][wled][1241]") {
    SECTION("inside the real envelope") {
        const nlohmann::json payload = {
            {"result",
             {{"strips",
               {{"strips", {{"strip", "strips"}, {"status", "on"}, {"brightness", 128}}}}}}}};

        const auto& map = helix::led::detail::wled_strip_map(payload);
        REQUIRE(map.size() == 1);
        REQUIRE(map.contains("strips"));
        // Must be the strip DETAIL under that key, not the wrapper again.
        REQUIRE(map["strips"].is_object());
        REQUIRE(map["strips"].value("status", "") == "on");
    }

    SECTION("alongside another strip") {
        const nlohmann::json payload = {
            {"result",
             {{"strips",
               {{"strips", {{"strip", "strips"}, {"status", "on"}}},
                {"TopLight", {{"strip", "TopLight"}, {"status", "off"}}}}}}}};

        const auto& map = helix::led::detail::wled_strip_map(payload);
        REQUIRE(map.size() == 2);
        REQUIRE(map.contains("strips"));
        REQUIRE(map.contains("TopLight"));
    }

    SECTION("flat shape where result.strips IS the strip detail") {
        // No wrapper level at all: the "strip" member marks it as a detail object.
        const nlohmann::json payload = {
            {"result", {{"strips", {{"strip", "strips"}, {"status", "off"}}}}}};

        const auto& map = helix::led::detail::wled_strip_map(payload);
        REQUIRE(map.size() == 1);
        REQUIRE(map.contains("strips"));
        REQUIRE(map["strips"].value("strip", "") == "strips");
    }
}

TEST_CASE("wled_strip_map: unusable input yields an empty object, not a crash",
          "[led][wled][1241]") {
    SECTION("null") {
        REQUIRE(helix::led::detail::wled_strip_map(nlohmann::json()).empty());
    }

    SECTION("array") {
        REQUIRE(helix::led::detail::wled_strip_map(nlohmann::json::array({1, 2})).empty());
    }

    SECTION("string") {
        REQUIRE(helix::led::detail::wled_strip_map(nlohmann::json("nope")).empty());
    }

    SECTION("result is not an object") {
        const nlohmann::json payload = {{"result", nullptr}};
        // Falls back to the outer object, which has no usable strip entries.
        const auto& map = helix::led::detail::wled_strip_map(payload);
        REQUIRE(map.is_object());
        REQUIRE(!map.contains("strips"));
    }

    SECTION("empty object") {
        REQUIRE(helix::led::detail::wled_strip_map(nlohmann::json::object()).empty());
    }

    SECTION("returned reference outlives a temporary sub-object") {
        const nlohmann::json empty_in = nlohmann::json::array();
        const auto& map = helix::led::detail::wled_strip_map(empty_in);
        REQUIRE(map.is_object()); // static empty, still readable
        REQUIRE(map.empty());
    }
}

// ============================================================================
// poll_status / discovery against the mock's real-shaped envelope (#1241)
// ============================================================================

/// LED tests that drive the controller leak a deferred connection-state
/// notification into the UpdateQueue; drain it here rather than in whichever
/// test runs next (same pattern as LedDiscoveryFixture).
struct WledMockFixture : public HelixTestFixture {
    MoonrakerClientMock mock_client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> mock_api;

    WledMockFixture() {
        state.init_subjects(false);
        state.set_klippy_state_sync(helix::KlippyState::READY);
        mock_api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
    }

    ~WledMockFixture() override {
        helix::led::LedController::instance().deinit();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

TEST_CASE_METHOD(WledMockFixture, "WledBackend: poll_status keys state by strip name, not wrapper",
                 "[led][wled][1241]") {
    WledBackend backend;
    backend.set_api(mock_api.get());

    // Mock defaults: printer_led on / brightness 200 / preset 2,
    //                enclosure_led off / brightness 128 / preset -1.
    bool completed = false;
    backend.poll_status([&]() { completed = true; });
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(completed);

    auto printer = backend.get_strip_state("printer_led");
    REQUIRE(printer.is_on);
    REQUIRE(printer.brightness == 200);
    REQUIRE(printer.active_preset == 2);

    auto enclosure = backend.get_strip_state("enclosure_led");
    REQUIRE(!enclosure.is_on);
    REQUIRE(enclosure.brightness == 128);
    REQUIRE(enclosure.active_preset == -1);

    // The regression: everything used to land under the literal key "strips",
    // which then defaulted to off/255/-1 for every real strip id.
    auto bogus = backend.get_strip_state("strips");
    REQUIRE(!bogus.is_on);
    REQUIRE(bogus.brightness == 255);
    REQUIRE(bogus.active_preset == -1);
}

TEST_CASE_METHOD(WledMockFixture, "LedController: WLED discovery uses real strip ids",
                 "[led][wled][discovery][1241]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(mock_api.get(), &mock_client);

    ctrl.discover_wled_strips();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    const auto& strips = ctrl.wled().strips();
    REQUIRE(strips.size() == 2);

    std::vector<std::string> ids;
    for (const auto& s : strips) {
        ids.push_back(s.id);
        REQUIRE(s.id != "strips"); // the wrapper key must never become a strip id
    }
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids[0] == "enclosure_led");
    REQUIRE(ids[1] == "printer_led");
}
