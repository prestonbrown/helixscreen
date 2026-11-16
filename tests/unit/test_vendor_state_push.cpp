// SPDX-License-Identifier: GPL-3.0-or-later
// Vendor state pushed by the HelixPrint plugin: the push-vs-poll rule over
// every plugin state, the tracker's resync and fallback, and the AD5X IFS and
// ACE backends applying a push through the same function as their poll.

#include "../fake_moonraker_client.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ace_backend_test_helper.h"
#include "../test_helpers/ace_test_access.h"
#include "../test_helpers/ad5x_ifs_test_access.h"
#include "../test_helpers/registered_backend.h"
#include "ams_backend_ace.h"
#include "ams_backend_ad5x_ifs.h"
#include "app_globals.h"
#include "helix_plugin_features.h"
#include "printer_state.h"
#include "test_helpers/backend_harness.h"
#include "vendor_state_push.h"

#include <chrono>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

PluginFeatureEntry entry(int version, bool available, bool enabled) {
    PluginFeatureEntry e;
    e.version = version;
    e.available = available;
    e.enabled = enabled;
    e.reason = available ? "" : "no_vendor";
    return e;
}

/// The source a backend picks for feature `vendor_x` at contract 1.
VendorStateSource source_for(bool plugin, const PluginFeatureMap& features) {
    return vendor_state_source(plugin_feature_state(plugin, features, "vendor_x", 1));
}

json notification(const json& message) {
    return json{{"jsonrpc", "2.0"},
                {"method", kVendorStateNotifyMethod},
                {"params", json::array({message})}};
}

/// The plugin as PrinterState holds it, applied the way a probe applies it.
void set_plugin(bool installed, PluginFeatureMap features) {
    auto& plugin = get_printer_state().plugin_status_state();
    plugin.set_installed(installed);
    plugin.set_features(std::move(features));
}

struct PluginStateFixture : LVGLUITestFixture {
    ~PluginStateFixture() override {
        set_plugin(false, {});
    }
};

constexpr const char* kJsonA = R"({"FFMInfo":{"ffmColor1":"#FF0000","ffmType1":"PLA"}})";
constexpr const char* kJsonB = R"({"FFMInfo":{"ffmColor1":"#00FF00","ffmType1":"PETG"}})";

json ace_slots(const char* material) {
    return json{{"slots",
                 {{{"index", 0}, {"color", "#FF0000"}, {"material", material}, {"status", "ready"}},
                  {{"index", 1}, {"color", "#00FF00"}, {"material", "PETG"}, {"status", "ready"}},
                  {{"index", 2}, {"color", "#0000FF"}, {"material", "ABS"}, {"status", "ready"}},
                  {{"index", 3}, {"color", "#FFFFFF"}, {"material", "TPU"}, {"status", "ready"}}}}};
}

} // namespace

// ============================================================================
// The rule
// ============================================================================

TEST_CASE("vendor state: push only while the plugin's feature is ready", "[plugin_vendor]") {
    SECTION("no plugin") {
        CHECK(source_for(false, {{"vendor_x", entry(1, true, true)}}) == VendorStateSource::Poll);
    }
    SECTION("a plugin without the features map") {
        CHECK(source_for(true, {}) == VendorStateSource::Poll);
    }
    SECTION("disabled") {
        CHECK(source_for(true, {{"vendor_x", entry(1, true, false)}}) == VendorStateSource::Poll);
    }
    SECTION("unavailable on this printer") {
        CHECK(source_for(true, {{"vendor_x", entry(1, false, false)}}) == VendorStateSource::Poll);
    }
    SECTION("a contract below the minimum") {
        CHECK(source_for(true, {{"vendor_x", entry(0, true, true)}}) == VendorStateSource::Poll);
    }
    SECTION("ready") {
        CHECK(source_for(true, {{"vendor_x", entry(1, true, true)}}) == VendorStateSource::Push);
    }
}

TEST_CASE("vendor state: a notification yields only its own vendor's payload", "[plugin_vendor]") {
    const json payload = {{"content", "x"}};
    CHECK(vendor_state_payload(notification({{"vendor", "ad5x"}, {"payload", payload}}), "ad5x") ==
          payload);
    CHECK_FALSE(
        vendor_state_payload(notification({{"vendor", "ace"}, {"payload", payload}}), "ad5x"));
    CHECK_FALSE(vendor_state_payload(notification({{"vendor", "ad5x"}}), "ad5x"));
    CHECK_FALSE(vendor_state_payload(notification(json::array()), "ad5x"));
    CHECK_FALSE(vendor_state_payload(json{{"params", json::array()}}, "ad5x"));
    CHECK_FALSE(vendor_state_payload(json::object(), "ad5x"));
}

TEST_CASE("vendor state: the tracker owes a read on each start of a push", "[plugin_vendor]") {
    VendorStateTracker tracker;
    CHECK_FALSE(tracker.pushing());

    CHECK_FALSE(tracker.update(PluginFeatureState::Absent, 1, 7));
    CHECK_FALSE(tracker.pushing());

    // The push starts: one read now, then none while nothing changes
    CHECK(tracker.update(PluginFeatureState::Ready, 1, 7));
    CHECK(tracker.pushing());
    CHECK_FALSE(tracker.update(PluginFeatureState::Ready, 1, 7));

    // A later probe (a reconnect, a restarted plugin) may have missed a change
    CHECK(tracker.update(PluginFeatureState::Ready, 2, 7));
    CHECK_FALSE(tracker.update(PluginFeatureState::Ready, 2, 7));

    // The plugin is gone: back to polling, nothing owed
    CHECK_FALSE(tracker.update(PluginFeatureState::Absent, 3, 7));
    CHECK_FALSE(tracker.pushing());
    CHECK_FALSE(tracker.update(PluginFeatureState::Disabled, 3, 7));
    CHECK_FALSE(tracker.pushing());

    CHECK(tracker.update(PluginFeatureState::Ready, 3, 7));
    CHECK(tracker.pushing());
}

TEST_CASE("vendor state: a Ready from before a reconnect waits for the next probe",
          "[plugin_vendor]") {
    VendorStateTracker tracker;
    REQUIRE(tracker.update(PluginFeatureState::Ready, 4, 1));

    // Reconnected; the feature map is still the old connection's
    CHECK_FALSE(tracker.update(PluginFeatureState::Ready, 4, 2));
    CHECK_FALSE(tracker.pushing());

    // Discovery probes on the new connection
    CHECK(tracker.update(PluginFeatureState::Ready, 5, 2));
    CHECK(tracker.pushing());
}

TEST_CASE("vendor state: reset goes back to polling", "[plugin_vendor]") {
    VendorStateTracker tracker;
    REQUIRE(tracker.update(PluginFeatureState::Ready, 1, 0));
    tracker.reset();
    CHECK_FALSE(tracker.pushing());
    // Nothing remembered: the same probe starts a push and owes a read again
    CHECK(tracker.update(PluginFeatureState::Ready, 1, 0));
}

TEST_CASE("vendor state: the backends keep a slow backstop poll while pushing", "[plugin_vendor]") {
    using std::chrono::seconds;
    CHECK(kVendorPushBackstop == seconds(30));
    SECTION("AD5X IFS") {
        CHECK_FALSE(AmsBackendAd5xIfs::should_poll_json(false, false, seconds(29), true));
        CHECK(AmsBackendAd5xIfs::should_poll_json(false, false, seconds(30), true));
        // The printing edge does not shortcut it: the plugin pushes the revert
        CHECK_FALSE(AmsBackendAd5xIfs::should_poll_json(false, true, seconds(1), true));
        CHECK(AmsBackendAd5xIfs::should_poll_json(false, false, seconds(5), false));
    }
    SECTION("ACE") {
        CHECK(AceTestAccess::rest_data_poll_due(false, seconds(0)));
        CHECK_FALSE(AceTestAccess::rest_data_poll_due(true, seconds(29)));
        CHECK(AceTestAccess::rest_data_poll_due(true, seconds(30)));
    }
}

// ============================================================================
// AD5X IFS
// ============================================================================

TEST_CASE("AD5X IFS ignores a push while it polls", "[plugin_vendor][ad5x_ifs]") {
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;

    Ad5xIfsTestAccess::on_vendor_state(backend,
                                       {{"file", "Adventurer5M.json"}, {"content", kJsonA}});
    // Still unseen: the push was not applied
    CHECK(Ad5xIfsTestAccess::note_json_content(backend, kJsonA));
}

TEST_CASE_METHOD(PluginStateFixture,
                 "AD5X IFS takes the plugin's push while ready and polls again without it",
                 "[plugin_vendor][ad5x_ifs]") {
    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
    auto& backend = *backend_reg;
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, false);
    const auto long_ago = std::chrono::steady_clock::now() - std::chrono::minutes(5);
    const json frame = {{"extruder", {{"temperature", 25.0}}}};

    set_plugin(true, {{"vendor_ad5x", entry(1, true, true)}});
    Ad5xIfsTestAccess::set_last_json_poll_kick(backend, long_ago);
    backend.fire_status(frame);
    REQUIRE(Ad5xIfsTestAccess::vendor_pushing(backend));
    // Starting the push read the file once
    CHECK(Ad5xIfsTestAccess::last_json_poll_kick(backend) > long_ago);

    // Past the idle cadence but inside the backstop: no poll while pushing
    const auto ten_s_ago = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    Ad5xIfsTestAccess::set_last_json_poll_kick(backend, ten_s_ago);
    backend.fire_status(frame);
    CHECK(Ad5xIfsTestAccess::last_json_poll_kick(backend) == ten_s_ago);

    SECTION("a push lands through the poll's apply: parse, then the GET_ZCOLOR follow-up") {
        const uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
        Ad5xIfsTestAccess::on_vendor_state(backend, {{"content", kJsonA}});
        CHECK(backend.get_slot_info(0).color_rgb == 0xFF0000);
        CHECK(backend.get_slot_info(0).material == "PLA");
        CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before + 1);

        // The same body again is no change, as for a poll
        Ad5xIfsTestAccess::on_vendor_state(backend, {{"content", kJsonA}});
        CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before + 1);
        CHECK_FALSE(Ad5xIfsTestAccess::note_json_content(backend, kJsonA));

        Ad5xIfsTestAccess::on_vendor_state(backend, {{"content", kJsonB}});
        CHECK(backend.get_slot_info(0).color_rgb == 0x00FF00);
        CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before + 2);
    }

    SECTION("a payload without content is ignored") {
        Ad5xIfsTestAccess::on_vendor_state(backend, {{"file", "Adventurer5M.json"}});
        CHECK(Ad5xIfsTestAccess::note_json_content(backend, kJsonA));
    }

    SECTION("the backstop polls once its interval passes") {
        Ad5xIfsTestAccess::set_last_json_poll_kick(backend, long_ago);
        backend.fire_status(frame);
        CHECK(Ad5xIfsTestAccess::vendor_pushing(backend));
        CHECK(Ad5xIfsTestAccess::last_json_poll_kick(backend) > long_ago);
    }

    SECTION("stopping the backend forgets the push") {
        Ad5xIfsTestAccess::on_stopping(backend);
        CHECK_FALSE(Ad5xIfsTestAccess::vendor_pushing(backend));
    }

    SECTION("losing the plugin brings the poll back on the next frame") {
        set_plugin(false, {});
        backend.fire_status(frame);
        CHECK_FALSE(Ad5xIfsTestAccess::vendor_pushing(backend));
        CHECK(Ad5xIfsTestAccess::last_json_poll_kick(backend) > ten_s_ago);

        // A push arriving late is ignored once the backend polls
        Ad5xIfsTestAccess::on_vendor_state(backend, {{"content", kJsonA}});
        CHECK(Ad5xIfsTestAccess::note_json_content(backend, kJsonA));
    }

    SECTION("a plugin that reconnects without the feature falls back too") {
        set_plugin(true, {});
        backend.fire_status(frame);
        CHECK_FALSE(Ad5xIfsTestAccess::vendor_pushing(backend));
        CHECK(Ad5xIfsTestAccess::last_json_poll_kick(backend) > ten_s_ago);
    }
}

TEST_CASE_METHOD(PluginStateFixture,
                 "AD5X IFS takes a push through the client from start until it stops",
                 "[plugin_vendor][ad5x_ifs]") {
    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
    auto& backend = *backend_reg;
    auto& client = backend.wire().fake_client();
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, false);
    const json push = notification({{"vendor", "ad5x"}, {"payload", {{"content", kJsonA}}}});

    // Nothing listens until the backend's first state query answers.
    CHECK_FALSE(client.fire_notification(kVendorStateNotifyMethod, push));

    REQUIRE_FALSE(client.rpc_calls.empty());
    client.rpc_calls.back().success_cb(json{{"result", {{"status", json::object()}}}});
    helix::ui::UpdateQueue::instance().drain();

    set_plugin(true, {{"vendor_ad5x", entry(1, true, true)}});
    backend.fire_status({{"extruder", {{"temperature", 25.0}}}});
    REQUIRE(Ad5xIfsTestAccess::vendor_pushing(backend));

    REQUIRE(client.fire_notification(kVendorStateNotifyMethod, push));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(backend.get_slot_info(0).color_rgb == 0xFF0000);
    CHECK(backend.get_slot_info(0).material == "PLA");

    // A stopped backend leaves no handler behind on the shared client.
    Ad5xIfsTestAccess::on_stopping(backend);
    CHECK_FALSE(client.fire_notification(kVendorStateNotifyMethod, push));
}

// ============================================================================
// ACE
// ============================================================================

TEST_CASE_METHOD(PluginStateFixture,
                 "ACE applies the plugin's pushed bridge answers as its REST poll would",
                 "[plugin_vendor][ace]") {
    AmsBackendAceTestHelper backend;
    backend.test_parse_info_response(json{{"slot_count", 4}});
    AceTestAccess::set_rest_fallback(backend, true);
    const json frame = {{"extruder", {{"temperature", 25.0}}}};

    SECTION("ignored while the backend polls") {
        AceTestAccess::on_vendor_state(backend, {{"slots", ace_slots("PLA")}});
        CHECK(backend.get_slot_info(0).material.empty());
    }

    SECTION("pushing") {
        set_plugin(true, {{"vendor_ace", entry(1, true, true)}});
        AceTestAccess::handle_status(backend, frame);
        REQUIRE(AceTestAccess::vendor_pushing(backend));

        AceTestAccess::on_vendor_state(backend, {{"slots", ace_slots("PLA")}});
        CHECK(backend.get_slot_info(0).material == "PLA");
        CHECK(backend.get_slot_info(0).color_rgb == 0xFF0000);

        SECTION("an error answer is not applied") {
            AceTestAccess::on_vendor_state(backend,
                                           {{"slots", {{"error", "instance 0 is unavailable"}}}});
            CHECK(backend.get_slot_info(0).material == "PLA");
        }

        SECTION("a pushed status error counts as a failed poll would") {
            const json failed = {{"status", {{"error", "unreadable"}}},
                                 {"slots", {{"error", "unreadable"}}}};
            AceTestAccess::on_vendor_state(backend, failed);
            AceTestAccess::on_vendor_state(backend, failed);
            CHECK(AceTestAccess::data_fetch_failures(backend) == 2);
            // Data again resets the count, as for the poll
            AceTestAccess::on_vendor_state(backend, {{"slots", ace_slots("PLA")}});
            CHECK(AceTestAccess::data_fetch_failures(backend) == 0);
        }

        SECTION("stopping the backend forgets the push") {
            AceTestAccess::on_stopping(backend);
            CHECK_FALSE(AceTestAccess::vendor_pushing(backend));
        }

        SECTION("losing the plugin brings the REST poll back") {
            set_plugin(false, {});
            AceTestAccess::handle_status(backend, frame);
            CHECK_FALSE(AceTestAccess::vendor_pushing(backend));

            AceTestAccess::on_vendor_state(backend, {{"slots", ace_slots("ASA")}});
            CHECK(backend.get_slot_info(0).material == "PLA");
        }
    }

    SECTION("a backend reading the ACE objects takes no push") {
        set_plugin(true, {{"vendor_ace", entry(1, true, true)}});
        AceTestAccess::set_rest_fallback(backend, false);
        AceTestAccess::handle_status(backend, frame);
        CHECK_FALSE(AceTestAccess::vendor_pushing(backend));
        AceTestAccess::on_vendor_state(backend, {{"slots", ace_slots("PLA")}});
        CHECK(backend.get_slot_info(0).material.empty());
    }
}

TEST_CASE_METHOD(PluginStateFixture,
                 "ACE takes a push through the client and its REST thread skips polls meanwhile",
                 "[plugin_vendor][ace]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper backend(&client);
    backend.test_parse_info_response(json{{"slot_count", 4}});
    backend.test_on_started();
    // No ACE object carries slots: the backend takes the REST bridge path
    REQUIRE_FALSE(client.rpc_calls.empty());
    client.rpc_calls.back().success_cb(json{{"result", json{{"status", json::object()}}}});
    helix::ui::UpdateQueue::instance().drain();

    SECTION("polling: every pass polls") {
        REQUIRE(wait_until([&] { return AceTestAccess::rest_data_polls(backend) >= 2; }, 3000));
    }

    SECTION("pushing") {
        set_plugin(true, {{"vendor_ace", entry(1, true, true)}});
        AceTestAccess::handle_status(backend, {{"extruder", {{"temperature", 25.0}}}});
        REQUIRE(AceTestAccess::vendor_pushing(backend));

        const int polls = AceTestAccess::rest_data_polls(backend);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        // The thread passed twice more; at most the pass already under way polled
        CHECK(AceTestAccess::rest_data_polls(backend) <= polls + 1);

        const json frame =
            notification({{"vendor", "ace"}, {"payload", {{"slots", ace_slots("PETG")}}}});
        REQUIRE(client.fire_notification(kVendorStateNotifyMethod, frame));
        helix::ui::UpdateQueue::instance().drain();
        CHECK(backend.get_slot_info(0).material == "PETG");

        // Another vendor's payload is not this backend's
        client.fire_notification(
            kVendorStateNotifyMethod,
            notification({{"vendor", "ad5x"}, {"payload", {{"content", "{}"}}}}));
        helix::ui::UpdateQueue::instance().drain();
        CHECK(backend.get_slot_info(0).material == "PETG");

        AceTestAccess::on_stopping(backend);
        CHECK_FALSE(client.fire_notification(kVendorStateNotifyMethod, frame));
    }
}
