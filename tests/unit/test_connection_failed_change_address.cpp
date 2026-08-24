// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_connection_failed_change_address.cpp
 * @brief The "can't reach the printer" modal must offer a way to fix it.
 *
 * Context (debug bundle XRK8KPTF, K2 Plus): the configured host was stale, so
 * the WebSocket never opened. With the initial-connect escalation in place the
 * user now gets a modal naming the address — but an OK-only modal on a wrong
 * address is still a dead end, and the address itself lives four levels deep
 * under Settings > System > Printer Host with nothing pointing there.
 *
 * So the connection-failed prompt carries a "Change Address" action that opens
 * the existing ChangeHostModal directly. This test drives the real prompt, then
 * clicks the real button, and asserts the host modal actually comes up — the
 * button existing is not the same as the button working.
 */

#include "ui_change_host_modal.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "app_globals.h"
#include "config.h"
#include "host_identity.h"
#include "i_moonraker_client.h"
#include "moonraker_error.h"
#include "printer_discovery.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

// Minimal IMoonrakerClient stand-in that records force_reconnect() calls.
// Everything else is a no-op — the prompt under test only ever asks the
// global client to force_reconnect().
class ReconnectCountingClient : public helix::IMoonrakerClient {
  public:
    int force_reconnect_calls = 0;

    void force_reconnect() override {
        ++force_reconnect_calls;
    }

    int connect(const char*, std::function<void()>, std::function<void()>) override {
        return 0;
    }
    void disconnect() override {}
    const std::string& get_last_url() const override {
        return empty_;
    }
    void set_auto_reconnect(bool) override {}
    int send_jsonrpc(const std::string&) override {
        return 0;
    }
    int send_jsonrpc(const std::string&, const json&) override {
        return 0;
    }
    helix::RequestId send_jsonrpc(const std::string&, const json&,
                                  std::function<void(const json&)>) override {
        return 0;
    }
    helix::RequestId send_jsonrpc(const std::string&, const json&, std::function<void(const json&)>,
                                  std::function<void(const MoonrakerError&)>, uint32_t, bool,
                                  std::optional<helix::rpc_error_policy::CallerIntent>) override {
        return 0;
    }
    int gcode_script(const std::string&) override {
        return 0;
    }
    void get_gcode_store(int, std::function<void(const std::vector<GcodeStoreEntry>&)>,
                         std::function<void(const MoonrakerError&)>) override {}
    void get_temperature_store(std::function<void(const TemperatureStore&)>,
                               std::function<void(const MoonrakerError&)>) override {}
    void discover_printer(std::function<void()>, std::function<void(const std::string&)>) override {
    }
    helix::PrinterDiscovery hardware() const override {
        return {};
    }
    void parse_objects(const json&) override {}
    void clear_discovery_cache() override {}
    void set_on_hardware_discovered(std::function<void(const helix::PrinterDiscovery&)>) override {}
    void set_on_discovery_complete(
        std::function<void(const helix::PrinterDiscovery&, const json&)>) override {}
    void set_bed_mesh_callback(std::function<void(const json&)>) override {}
    helix::SubscriptionId register_notify_update(std::function<void(const json&)>) override {
        return helix::INVALID_SUBSCRIPTION_ID;
    }
    bool unsubscribe_notify_update(helix::SubscriptionId) override {
        return false;
    }
    void register_method_callback(const std::string&, const std::string&,
                                  std::function<void(const json&)>) override {}
    bool unregister_method_callback(const std::string&, const std::string&) override {
        return false;
    }
    void dispatch_status_update(const json&, bool) override {}
    helix::ConnectionState get_connection_state() const override {
        return helix::ConnectionState::DISCONNECTED;
    }
    void add_connected_observer(const std::string&, std::function<void()>) override {}
    bool remove_connected_observer(const std::string&) override {
        return false;
    }
    void register_event_handler(helix::MoonrakerEventCallback) override {}
    void suppress_disconnect_modal(uint32_t) override {}
    bool is_disconnect_modal_suppressed() const override {
        return false;
    }
    bool cancel_request(helix::RequestId) override {
        return false;
    }
    void set_state_change_callback(
        std::function<void(helix::ConnectionState, helix::ConnectionState)>) override {}
    void set_connection_timeout(uint32_t) override {}
    void set_default_request_timeout(uint32_t) override {}
    void configure_timeouts(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void process_timeouts() override {}
    void toggle_filament_runout_simulation() override {}
    std::weak_ptr<bool> lifetime_weak() const override {
        return std::make_shared<bool>(true);
    }

  private:
    std::string empty_;
};

/// Publishes a counting client as the global for the test's lifetime.
class ScopedGlobalClient {
  public:
    ScopedGlobalClient() {
        set_moonraker_client(&client_);
    }
    ~ScopedGlobalClient() {
        set_moonraker_client(nullptr);
    }

    int reconnect_calls() const {
        return client_.force_reconnect_calls;
    }

  private:
    ReconnectCountingClient client_;
};

class ConnFailedFixture : public XMLTestFixture {
  public:
    ConnFailedFixture() {
        // modal_configure() silently no-ops without these, leaving the button
        // captions at their defaults — the app does this at startup.
        helix::ui::modal_init_subjects();
        REQUIRE(register_component("modal_dialog"));
        REQUIRE(register_component("change_host_modal"));
    }
    ~ConnFailedFixture() override {
        while (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
            UpdateQueue::instance().drain();
        }
        UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(ConnFailedFixture, "Connection-failed prompt offers Reconnect",
                 "[modal][connection][change_host][reconnect]") {
    ScopedGlobalClient client;
    helix::ui::show_connection_failed_modal("Connection Failed",
                                            "Unable to reach printer at 192.168.1.171:7125.");
    UpdateQueue::instance().drain();

    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    // Reconnect is the primary action now: a mid-print WiFi drop on Android
    // left users with a connection the retry loop could not revive, and the
    // old prompt's only actions were "fix the address" or dismiss.
    const char* primary_text = static_cast<const char*>(
        lv_subject_get_pointer(helix::ui::modal_get_primary_text_subject()));
    REQUIRE(primary_text != nullptr);
    CHECK(std::string(primary_text).find("Reconnect") != std::string::npos);

    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);
    lv_obj_send_event(primary, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();

    CHECK(client.reconnect_calls() == 1);
    // The prompt got out of the way — it must not sit on top of the UI it
    // just asked to reconnect.
    CHECK(Modal::get_top() == nullptr);
}

TEST_CASE_METHOD(ConnFailedFixture, "Connection-failed prompt offers Change Address",
                 "[modal][connection][change_host]") {
    helix::ui::show_connection_failed_modal("Connection Failed",
                                            "Unable to reach printer at 192.168.1.171:7125.");
    // The real caller is the libhv thread, so the prompt marshals itself to the
    // main thread. Nothing exists until the queue drains — asserting before this
    // would pass against a version that never marshalled at all.
    UpdateQueue::instance().drain();

    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    // The address the user has to correct must be in front of them.
    lv_obj_t* msg = lv_obj_find_by_name(dialog, "dialog_message");
    if (msg) {
        CHECK(std::string(lv_label_get_text(msg)).find("192.168.1.171:7125") != std::string::npos);
    }

    // Address fixing is demoted to the secondary action, but still one tap.
    const char* cancel_text = static_cast<const char*>(
        lv_subject_get_pointer(helix::ui::modal_get_cancel_text_subject()));
    REQUIRE(cancel_text != nullptr);
    CHECK(std::string(cancel_text).find("Change Address") != std::string::npos);

    lv_obj_t* secondary = lv_obj_find_by_name(dialog, "btn_secondary");
    REQUIRE(secondary != nullptr);

    // Press it. This is the half that a "button exists" assertion would miss.
    lv_obj_send_event(secondary, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();

    lv_obj_t* now_top = Modal::get_top();
    REQUIRE(now_top != nullptr);
    CHECK(now_top != dialog);
    // The change-host modal owns a host input; finding it proves we landed on
    // the right dialog rather than merely dismissing the first one.
    CHECK(lv_obj_find_by_name(now_top, "host_input") != nullptr);
}

TEST_CASE_METHOD(ConnFailedFixture, "Connection-failed prompt can be dismissed without changes",
                 "[modal][connection][change_host]") {
    ScopedGlobalClient client;
    helix::ui::show_connection_failed_modal("Connection Failed",
                                            "Unable to reach printer at 10.0.0.5:7125.");
    UpdateQueue::instance().drain();

    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    // Backdrop-tap dismissal — the path a user takes when they want neither
    // action. Modal::hide() is what the dialog's own backdrop handler calls.
    Modal::hide(dialog);
    UpdateQueue::instance().drain();

    // Dismissing must not leave the host modal (or anything else) behind, and
    // must not fire a reconnect either.
    CHECK(Modal::get_top() == nullptr);
    CHECK(client.reconnect_calls() == 0);
}

TEST_CASE_METHOD(ConnFailedFixture,
                 "Connection-failed prompt drops Change Address when the printer IS this machine",
                 "[modal][connection][change_host]") {
    // AD5X bundles TAU4PW4H / 865DXBQ7: HelixScreen runs ON the printer, dials
    // 127.0.0.1:7125, and gets an instant refusal because Moonraker never
    // started. "Change Address" there sends the user to edit an address that is
    // already correct, and the body text told them to check that the printer was
    // powered on — while they were holding its screen.
    Config* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);
    const std::string key = cfg->df() + "moonraker_host";
    const std::string prev = cfg->get<std::string>(key, "");
    cfg->set<std::string>(key, "127.0.0.1");
    helix::invalidate_host_identity_cache();
    ScopedGlobalClient client;

    helix::ui::show_connection_failed_modal("Connection Failed",
                                            "Moonraker is not responding at 127.0.0.1:7125.");
    UpdateQueue::instance().drain();

    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    // Single-action acknowledgement of a local Moonraker that is not up — no
    // address-editing action, but retrying the services IS meaningful here, so
    // the one action is Reconnect.
    const char* primary_text = static_cast<const char*>(
        lv_subject_get_pointer(helix::ui::modal_get_primary_text_subject()));
    REQUIRE(primary_text != nullptr);
    CHECK(std::string(primary_text).find("Change Address") == std::string::npos);
    CHECK(std::string(primary_text).find("Reconnect") != std::string::npos);

    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);
    lv_obj_send_event(primary, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    CHECK(client.reconnect_calls() == 1);
    CHECK(Modal::get_top() == nullptr);

    cfg->set<std::string>(key, prev);
    helix::invalidate_host_identity_cache();
}

TEST_CASE_METHOD(ConnFailedFixture, "An unconfigured host still offers Change Address",
                 "[modal][connection][change_host]") {
    // The guard above must key off a host we positively identified as local. An
    // empty host is the case where changing the address is precisely the fix, so
    // it must not be swept in by a "localhost" default.
    Config* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);
    const std::string key = cfg->df() + "moonraker_host";
    const std::string prev = cfg->get<std::string>(key, "");
    cfg->set<std::string>(key, std::string(""));
    helix::invalidate_host_identity_cache();

    helix::ui::show_connection_failed_modal("Connection Failed", "Unable to reach printer.");
    UpdateQueue::instance().drain();
    REQUIRE(Modal::get_top() != nullptr);

    const char* cancel_text = static_cast<const char*>(
        lv_subject_get_pointer(helix::ui::modal_get_cancel_text_subject()));
    REQUIRE(cancel_text != nullptr);
    CHECK(std::string(cancel_text).find("Change Address") != std::string::npos);

    cfg->set<std::string>(key, prev);
    helix::invalidate_host_identity_cache();
}
