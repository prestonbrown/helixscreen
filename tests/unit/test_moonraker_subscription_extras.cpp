// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Plugin objects ride the app's union printer.objects.subscribe. These cases run
// the REAL discovery sequence over the mock transport (MoonrakerClientMock's own
// discover_printer is a shortcut that never subscribes).

#include "../lvgl_test_fixture.h"
#include "core/update_queue.h"
#include "moonraker_client_mock.h"
#include "moonraker_discovery_sequence.h"
#include "moonraker_subscription_merge.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

namespace {

/// Records every subscribe and fails or holds one the way the request tracker
/// delivers it: a refusal, a timeout and a lost connection all arrive through the
/// error callback, never the success callback.
class RecordingClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;
    using MoonrakerClientMock::send_jsonrpc;

    helix::RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 helix::SharedCallback<void(const json&)> success_cb,
                 helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms,
                 bool silent,
                 std::optional<helix::rpc_error_policy::CallerIntent> intent) override {
        if (method == "printer.objects.list" && objects_list_error) {
            if (success_cb)
                success_cb(json{{"error", {{"message", *objects_list_error}}}});
            return 0;
        }
        if (method == "printer.objects.subscribe") {
            json objects = params.contains("objects") ? params["objects"] : json::object();
            subscribes.push_back(objects);
            if (!fail_object.empty() && objects.contains(fail_object)) {
                if (error_cb)
                    error_cb(fail_error);
                return 0;
            }
            if (hold_next_subscribe) {
                hold_next_subscribe = false;
                held = [=]() {
                    MoonrakerClientMock::send_jsonrpc(method, params, success_cb, error_cb,
                                                      timeout_ms, silent, intent);
                };
                return 0;
            }
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, success_cb, error_cb, timeout_ms,
                                                 silent, intent);
    }

    /// Runs the real discovery sequence; true once it reported completion.
    bool discover_real() {
        done = false;
        MoonrakerClient::discover_printer([this]() { done = true; }, [](const std::string&) {});
        helix::ui::UpdateQueue::instance().drain();
        return done;
    }

    void release_held() {
        auto fn = std::move(held);
        held = nullptr;
        REQUIRE(fn);
        fn();
        helix::ui::UpdateQueue::instance().drain();
    }

    std::vector<json> subscribes;
    /// When set, printer.objects.list answers with this error message.
    std::optional<std::string> objects_list_error;
    std::string fail_object;
    MoonrakerError fail_error = MoonrakerError::from_json_rpc(
        json{{"code", 400}, {"message", "bad object"}}, "printer.objects.subscribe");
    bool hold_next_subscribe = false;
    helix::SharedCallback<void()> held;
    bool done = false;
};

struct ProviderValue {
    std::mutex mutex;
    json value = json::object();
    void set(json v) {
        std::lock_guard<std::mutex> lock(mutex);
        value = std::move(v);
    }
    json get() {
        std::lock_guard<std::mutex> lock(mutex);
        return value;
    }
};

/// The objects map the app subscribes with no provider at all.
json app_only_subscription() {
    RecordingClient plain(MoonrakerClientMock::PrinterType::VORON_24);
    plain.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    REQUIRE(plain.discover_real());
    REQUIRE(plain.subscribes.size() == 1);
    return plain.subscribes.front();
}

RecordingClient& with_provider(RecordingClient& client, ProviderValue& pv) {
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.set_subscription_extras_provider([&pv]() { return pv.get(); });
    return client;
}

} // namespace

TEST_CASE("extras ride the first subscription", "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    const json app = app_only_subscription();

    ProviderValue pv;
    pv.set({{"temperature_sensor spark", nullptr}});
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(with_provider(client, pv).discover_real());

    REQUIRE(client.subscribes.size() == 1);
    const json& sent = client.subscribes.front();
    CHECK(sent.contains("temperature_sensor spark"));
    CHECK(sent == helix::merge_subscription_objects(app, pv.get()));
    for (auto it = app.begin(); it != app.end(); ++it) {
        INFO(it.key());
        REQUIRE(sent.contains(it.key()));
        CHECK(sent[it.key()] == it.value());
    }
}

TEST_CASE("refresh adds and removes plugin objects without dropping app objects",
          "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    const json app = app_only_subscription();

    ProviderValue pv;
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(with_provider(client, pv).discover_real());
    REQUIRE(client.subscribes.size() == 1);
    CHECK(client.subscribes[0] == app); // an empty provider sends exactly the app objects

    const json plugin = {{"temperature_sensor plugin_x", nullptr},
                         {"extruder", json::array({"pressure_advance"})}};
    pv.set(plugin);
    client.refresh_subscription();
    REQUIRE(client.subscribes.size() == 2);
    CHECK(client.subscribes[1] == helix::merge_subscription_objects(app, plugin));

    client.refresh_subscription(); // nothing changed: nothing sent
    CHECK(client.subscribes.size() == 2);

    pv.set(json::object());
    client.refresh_subscription();
    REQUIRE(client.subscribes.size() == 3);
    CHECK(client.subscribes[2] == app);
}

TEST_CASE("refresh before the first subscription completes sends nothing",
          "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    ProviderValue pv;
    pv.set({{"temperature_sensor spark", nullptr}});
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    with_provider(client, pv);
    client.refresh_subscription();
    CHECK(client.subscribes.empty());
}

TEST_CASE("reconnect includes the extras", "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    ProviderValue pv;
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(with_provider(client, pv).discover_real());

    pv.set({{"temperature_sensor spark", nullptr}});
    REQUIRE(client.discover_real()); // a reconnect re-runs discovery on the same client
    REQUIRE(client.subscribes.size() == 2);
    CHECK(client.subscribes.back().contains("temperature_sensor spark"));
}

TEST_CASE("a subscribe error with extras falls back to the app objects",
          "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    const json app = app_only_subscription();

    std::vector<MoonrakerEventType> events;
    ProviderValue pv;
    pv.set({{"bogus_object", nullptr}});
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.fail_object = "bogus_object";
    client.register_event_handler([&events](const MoonrakerEvent& e) { events.push_back(e.type); });
    REQUIRE(with_provider(client, pv).discover_real());

    REQUIRE(client.subscribes.size() == 2);
    CHECK(client.subscribes[0].contains("bogus_object"));
    CHECK(client.subscribes[1] == app);
    for (auto type : events)
        CHECK(type != MoonrakerEventType::DISCOVERY_FAILED);

    client.refresh_subscription(); // the same refused extras are not sent again
    CHECK(client.subscribes.size() == 2);

    pv.set({{"temperature_sensor spark", nullptr}}); // a new result is tried
    client.refresh_subscription();
    REQUIRE(client.subscribes.size() == 3);
    CHECK(client.subscribes[2].contains("temperature_sensor spark"));
}

TEST_CASE("a refresh error with extras re-sends the app objects", "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    const json app = app_only_subscription();

    ProviderValue pv;
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.fail_object = "bogus_object";
    REQUIRE(with_provider(client, pv).discover_real());
    REQUIRE(client.subscribes.size() == 1);

    pv.set({{"bogus_object", nullptr}});
    client.refresh_subscription();
    REQUIRE(client.subscribes.size() == 3);
    CHECK(client.subscribes[1].contains("bogus_object"));
    CHECK(client.subscribes[2] == app);
}

TEST_CASE("a refused app subscription reports DISCOVERY_FAILED and completes discovery",
          "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    std::vector<MoonrakerEventType> events;
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.fail_object = "webhooks";
    client.register_event_handler([&events](const MoonrakerEvent& e) { events.push_back(e.type); });

    CHECK(client.discover_real());
    CHECK(client.subscribes.size() == 1);
    CHECK(std::count(events.begin(), events.end(), MoonrakerEventType::DISCOVERY_FAILED) == 1);
}

TEST_CASE("a failed objects list reports the firmware envelope's message",
          "[moonraker][subscription][firmware_error_envelope]") {
    LVGLTestFixture fixture;
    std::vector<std::string> failures;
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.objects_list_error =
        R"({"coded": "0003-0522-0002-0008", "oneshot": 0, "msg":"Lost communication with MCU 'e0'"})";
    client.register_event_handler([&failures](const MoonrakerEvent& e) {
        if (e.type == MoonrakerEventType::DISCOVERY_FAILED)
            failures.push_back(e.message);
    });
    std::string reported;
    client.MoonrakerClient::discover_printer(
        []() {}, [&reported](const std::string& why) { reported = why; });
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(failures.size() == 1);
    CHECK(failures[0] == "Lost communication with MCU 'e0'");
    CHECK(reported == "Lost communication with MCU 'e0'");
}

TEST_CASE("a timed-out subscribe with extras falls back without refusing them",
          "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    const json app = app_only_subscription();

    ProviderValue pv;
    pv.set({{"temperature_sensor spark", nullptr}});
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.fail_object = "temperature_sensor spark";
    client.fail_error = MoonrakerError::timeout("printer.objects.subscribe", 1000);
    REQUIRE(with_provider(client, pv).discover_real());
    REQUIRE(client.subscribes.size() == 2);
    CHECK(client.subscribes[1] == app);

    client.fail_object.clear();
    client.refresh_subscription(); // not refused, so the same extras are tried again
    REQUIRE(client.subscribes.size() == 3);
    CHECK(client.subscribes[2].contains("temperature_sensor spark"));
}

TEST_CASE("a refresh lost with the connection does not stop later refreshes",
          "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    ProviderValue pv;
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(with_provider(client, pv).discover_real());

    pv.set({{"temperature_sensor spark", nullptr}});
    client.fail_object = "temperature_sensor spark";
    client.fail_error = MoonrakerError::connection_lost("printer.objects.subscribe");
    client.refresh_subscription();
    REQUIRE(client.subscribes.size() == 2); // nothing re-sent on a lost connection

    client.fail_object.clear();
    client.refresh_subscription();
    REQUIRE(client.subscribes.size() == 3);
    CHECK(client.subscribes[2].contains("temperature_sensor spark"));
}

TEST_CASE("a refresh asked for during the discovery subscribe is sent once it settles",
          "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    const json app = app_only_subscription();

    ProviderValue pv;
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.hold_next_subscribe = true;
    CHECK_FALSE(with_provider(client, pv).discover_real());
    REQUIRE(client.subscribes.size() == 1);
    CHECK(client.subscribes[0] == app);

    const json plugin = {{"temperature_sensor spark", nullptr}};
    pv.set(plugin);
    client.refresh_subscription();
    CHECK(client.subscribes.size() == 1); // nothing goes out while discovery is in flight

    client.release_held();
    CHECK(client.done);
    REQUIRE(client.subscribes.size() == 2);
    CHECK(client.subscribes[1] == helix::merge_subscription_objects(app, plugin));
}

TEST_CASE("a refresh pending behind one in flight survives another drain",
          "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    const json app = app_only_subscription();
    const json first = {{"temperature_sensor spark", nullptr}};
    const json second = {{"temperature_sensor spark", nullptr},
                         {"temperature_sensor bolt", nullptr}};

    ProviderValue pv;
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    // The discovery-complete callback runs after the subscribe is marked done and
    // before that subscribe drains its pending refresh: the window a refresh callback
    // on the WebSocket thread leaves while the main thread starts another.
    bool staged = false;
    client.set_on_discovery_complete([&](const helix::PrinterDiscovery&, const json&) {
        if (staged)
            return;
        staged = true;
        client.hold_next_subscribe = true;
        pv.set(first);
        client.refresh_subscription(); // in flight, held
        pv.set(second);
        client.refresh_subscription(); // pending behind it
    });
    REQUIRE(with_provider(client, pv).discover_real());
    REQUIRE(staged);
    REQUIRE(client.subscribes.size() == 2);
    CHECK(client.subscribes[1] == helix::merge_subscription_objects(app, first));

    client.release_held();
    REQUIRE(client.subscribes.size() == 3);
    CHECK(client.subscribes[2] == helix::merge_subscription_objects(app, second));
}

TEST_CASE("a refresh asked for while extras are sampled is not stranded",
          "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    const json app = app_only_subscription();
    const json plugin = {{"temperature_sensor spark", nullptr}};

    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    bool armed = false;
    bool fired = false;
    json value = json::object();
    client.set_subscription_extras_provider([&]() {
        if (armed && !fired) {
            fired = true;
            // Test-only breach of the provider contract: stands in for a second thread
            // calling refresh while the first one samples the extras.
            client.refresh_subscription();
            json sampled = value;
            value = plugin;
            return sampled;
        }
        return value;
    });
    REQUIRE(client.discover_real());
    REQUIRE(client.subscribes.size() == 1);

    armed = true;
    client.refresh_subscription(); // samples {}, equal to what was last sent
    REQUIRE(fired);
    REQUIRE(client.subscribes.size() == 2);
    CHECK(client.subscribes[1] == helix::merge_subscription_objects(app, plugin));
}

TEST_CASE("the refresh response reaches status callbacks", "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    ProviderValue pv;
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(with_provider(client, pv).discover_real());

    std::atomic<int> saw_app_object{0};
    client.register_notify_update([&saw_app_object](const json& msg) {
        if (msg.contains("params") && msg["params"].is_array() && !msg["params"].empty() &&
            msg["params"][0].contains("webhooks"))
            saw_app_object.fetch_add(1);
    });

    pv.set({{"temperature_sensor spark", nullptr}});
    client.refresh_subscription();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(saw_app_object.load() >= 1);
}

TEST_CASE("the refresh response is dispatched as whole objects, not a cached snapshot",
          "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    ProviderValue pv;
    RecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(with_provider(client, pv).discover_real());

    std::mutex mutex;
    std::vector<json> frames;
    client.register_notify_update([&](const json& msg) {
        if (msg.contains("params") && msg["params"].is_array() && !msg["params"].empty() &&
            msg["params"][0].contains("webhooks")) {
            std::lock_guard<std::mutex> lock(mutex);
            frames.push_back(msg);
        }
    });

    pv.set({{"temperature_sensor spark", nullptr}});
    client.refresh_subscription();
    helix::ui::UpdateQueue::instance().drain();

    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE_FALSE(frames.empty());
    for (const json& msg : frames) {
        auto frame = helix::parse_status_notification(msg);
        REQUIRE(frame);
        CHECK(frame->whole_objects);
        CHECK_FALSE(frame->from_cached_snapshot);
    }
}

TEST_CASE("plugin-only objects and fields in a status frame leave app state alone",
          "[moonraker][subscription]") {
    LVGLTestFixture fixture;
    helix::PrinterState state;
    state.init_subjects(false);
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array({"extruder", "heater_bed"}));
    state.set_hardware(hw);

    state.update_from_status({{"extruder", {{"temperature", 210.0}, {"target", 215.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    const int temp =
        lv_subject_get_int(state.temperature_state().get_active_extruder_temp_subject());
    REQUIRE(temp == 2100);

    state.update_from_status({{"temperature_sensor plugin_x", {{"temperature", 51.0}}},
                              {"extruder", {{"pressure_advance", 0.04}}}});
    helix::ui::UpdateQueue::instance().drain();
    CHECK(lv_subject_get_int(state.temperature_state().get_active_extruder_temp_subject()) == temp);
}
