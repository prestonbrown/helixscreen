// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_fake_moonraker_client.cpp
 * @brief The recording fake's discovery-callback capture and subscription book.
 *
 * Discovery-reaction tests drive production code through the callbacks it installs
 * on the client, so the fake has to hand those callbacks back and report which
 * subscriptions a feature left behind.
 */

// TEST_MIRROR_OK: the fake itself is the unit under test; it is test infrastructure and ships
// nowhere.
#include "../fake_moonraker_client.h"

#include "../catch_amalgamated.hpp"

using helix::test::FakeMoonrakerClient;

TEST_CASE("fake client keeps the discovery callbacks it was given", "[fake_client][discovery]") {
    FakeMoonrakerClient client;
    REQUIRE_FALSE(client.on_hardware_discovered);
    REQUIRE_FALSE(client.on_discovery_complete);

    int hw_calls = 0;
    int complete_calls = 0;
    client.set_on_hardware_discovered([&](const helix::PrinterDiscovery&) { ++hw_calls; });
    client.set_on_discovery_complete(
        [&](const helix::PrinterDiscovery&, const nlohmann::json& status) {
            complete_calls += static_cast<int>(status.size());
        });

    helix::PrinterDiscovery hw;
    client.on_hardware_discovered(hw);
    client.on_discovery_complete(hw, nlohmann::json{{"a", 1}, {"b", 2}});
    CHECK(hw_calls == 1);
    CHECK(complete_calls == 2);
}

TEST_CASE("fake client live_handlers reports what detach left behind", "[fake_client][discovery]") {
    FakeMoonrakerClient client;
    client.register_method_callback("notify_a", "keep", [](const nlohmann::json&) {});
    const auto before = client.live_handlers();

    client.register_method_callback("notify_b", "feature", [](const nlohmann::json&) {});
    client.register_method_callback("notify_b", "feature", [](const nlohmann::json&) {});
    CHECK(client.live_handlers() != before);
    CHECK(client.handler_count("notify_b", "feature") == 2);

    CHECK(client.unregister_method_callback("notify_b", "feature"));
    CHECK(client.live_handlers() == before);
    CHECK(client.handler_count("notify_b", "feature") == 0);
}

TEST_CASE("fake client delivers status notifications to notify subscribers until unsubscribed",
          "[fake_client]") {
    FakeMoonrakerClient client;
    const nlohmann::json frame = {{"method", "notify_status_update"}};
    CHECK_FALSE(client.fire_notification("notify_status_update", frame));

    int calls = 0;
    const auto id = client.register_notify_update([&](const nlohmann::json&) { ++calls; });
    REQUIRE(id != helix::kInvalidSubscriptionId);

    CHECK(client.fire_notification("notify_status_update", frame));
    CHECK_FALSE(client.fire_notification("notify_gcode_response", frame));
    CHECK(calls == 1);

    CHECK(client.unsubscribe_notify_update(id));
    CHECK_FALSE(client.fire_notification("notify_status_update", frame));
    CHECK(calls == 1);
}
