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

TEST_CASE("fake client answers a method from its respond_to table", "[fake_client][respond_to]") {
    FakeMoonrakerClient client;
    std::vector<nlohmann::json> replies;
    std::vector<std::string> errors;
    const auto send = [&](const std::string& method) {
        client.send_jsonrpc(
            method, nlohmann::json::object(),
            [&replies](const nlohmann::json& r) { replies.push_back(r); },
            [&errors](const MoonrakerError& e) { errors.push_back(e.message); }, 0, false,
            std::nullopt);
    };

    SECTION("a method with no entry is recorded and left unanswered") {
        send("a.method");
        CHECK(client.rpc_count("a.method") == 1);
        CHECK(replies.empty());
        CHECK(errors.empty());
        CHECK(client.reply_to_last("a.method", nlohmann::json{{"late", true}}));
        CHECK(replies == std::vector<nlohmann::json>{nlohmann::json{{"late", true}}});
        CHECK_FALSE(client.reply_to_last("never.sent", nlohmann::json::object()));
    }

    SECTION("a reply is handed over verbatim, and only for its own method") {
        client.respond_to("a.method", nlohmann::json{{"a", 1}});
        send("other.method");
        CHECK(replies.empty());
        send("a.method");
        CHECK(replies == std::vector<nlohmann::json>{nlohmann::json{{"a", 1}}});
    }

    SECTION("fail_on replaces a reply, and a held send fails on demand") {
        client.respond_to("a.method", nlohmann::json{{"a", 1}});
        MoonrakerError err;
        err.message = "refused";
        client.fail_on("a.method", err);
        send("a.method");
        CHECK(replies.empty());
        CHECK(errors == std::vector<std::string>{"refused"});

        send("held.method");
        CHECK(client.fail_last("held.method", err));
        CHECK(errors.size() == 2);
    }

    SECTION("every send form reaches the table") {
        int answered = 0;
        client.respond_to("a.method", FakeMoonrakerClient::Responder(
                                          [&answered](const nlohmann::json&, const auto&,
                                                      const auto&) { ++answered; }));
        client.send_jsonrpc("a.method");
        client.send_jsonrpc("a.method", nlohmann::json::object());
        client.send_jsonrpc("a.method", nlohmann::json::object(),
                            helix::SharedCallback<void(const nlohmann::json&)>{});
        send("a.method");
        CHECK(answered == 4);
    }
}
