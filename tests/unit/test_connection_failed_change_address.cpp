// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_connection_failed_change_address.cpp
 * @brief The "can't reach the printer" modal must offer a way to fix it.
 *
 * Context (debug bundle XRK8KPTF, K2 Plus): the configured host was stale, so
 * the WebSocket never opened. With the initial-connect escalation in place the
 * user now gets a modal naming the address — but an OK-only modal on a wrong
 * address is still a dead end, and the address itself lives under
 * Settings > Connection > Host with nothing pointing there.
 *
 * So the connection-failed prompt carries a "Change Address" action that opens
 * the existing ChangeHostModal directly. This test drives the real prompt, then
 * clicks the real button, and asserts the host modal actually comes up — the
 * button existing is not the same as the button working.
 */

#include "ui_change_host_modal.h"
#include "ui_modal.h"
#include "ui_printer_switch_menu.h"

#include "../mocks/mock_mdns_discovery.h"
#include "../test_fixtures.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/scoped_config_write_counter.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "host_identity.h"
#include "i_moonraker_client.h"
#include "moonraker_error.h"
#include "printer_discovery.h"
#include "wizard_config_paths.h"

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
    helix::ConnectionState state = helix::ConnectionState::DISCONNECTED;

    void force_reconnect() override {
        ++force_reconnect_calls;
    }

    int connect(const char*, helix::SharedCallback<void()>,
                helix::SharedCallback<void()>) override {
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
                                  helix::SharedCallback<void(const json&)>) override {
        return 0;
    }
    helix::RequestId send_jsonrpc(const std::string&, const json&,
                                  helix::SharedCallback<void(const json&)>,
                                  helix::SharedCallback<void(const MoonrakerError&)>, uint32_t,
                                  bool,
                                  std::optional<helix::rpc_error_policy::CallerIntent>) override {
        return 0;
    }
    int gcode_script(const std::string&) override {
        return 0;
    }
    void get_gcode_store(int, std::function<void(const std::vector<GcodeStoreEntry>&)>,
                         helix::SharedCallback<void(const MoonrakerError&)>) override {}
    void get_temperature_store(std::function<void(const TemperatureStore&)>,
                               helix::SharedCallback<void(const MoonrakerError&)>) override {}
    void discover_printer(helix::SharedCallback<void()>,
                          helix::SharedCallback<void(const std::string&)>) override {}
    helix::PrinterDiscovery hardware() const override {
        return {};
    }
    void parse_objects(const json&) override {}
    void clear_discovery_cache() override {}
    void set_on_hardware_discovered(std::function<void(const helix::PrinterDiscovery&)>) override {}
    void set_on_discovery_complete(
        std::function<void(const helix::PrinterDiscovery&, const json&)>) override {}
    void set_bed_mesh_callback(helix::SharedCallback<void(const json&)>) override {}
    helix::SubscriptionId
    register_notify_update(helix::SharedCallback<void(const json&)>) override {
        return helix::kInvalidSubscriptionId;
    }
    bool unsubscribe_notify_update(helix::SubscriptionId) override {
        return false;
    }
    void register_method_callback(const std::string&, const std::string&,
                                  helix::SharedCallback<void(const json&)>) override {}
    bool unregister_method_callback(const std::string&, const std::string&) override {
        return false;
    }
    void dispatch_status_update(const json&, bool, bool) override {}
    void set_subscription_extras_provider(std::function<json()>) override {}
    void refresh_subscription() override {}
    helix::ConnectionState get_connection_state() const override {
        return state;
    }
    void add_connected_observer(const std::string&, helix::SharedCallback<void()>) override {}
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
    uint64_t connection_generation() const override {
        return 0;
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
    void set_state(helix::ConnectionState state) {
        client_.state = state;
    }

  private:
    ReconnectCountingClient client_;
};

class ConnFailedFixture : public XMLTestFixture {
  public:
    ConnFailedFixture() {
        // The app registers the modal callbacks at startup.
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
    const std::string primary_text = UITest::button_text(Modal::get_top(), "btn_primary");
    CHECK(primary_text.find("Reconnect") != std::string::npos);

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
    const std::string cancel_text = UITest::button_text(Modal::get_top(), "btn_secondary");
    CHECK(cancel_text.find("Change Address") != std::string::npos);

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
    const std::string primary_text = UITest::button_text(Modal::get_top(), "btn_primary");
    CHECK(primary_text.find("Change Address") == std::string::npos);
    CHECK(primary_text.find("Reconnect") != std::string::npos);

    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);
    lv_obj_send_event(primary, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    CHECK(client.reconnect_calls() == 1);
    CHECK(Modal::get_top() == nullptr);

    cfg->set<std::string>(key, prev);
    helix::invalidate_host_identity_cache();
}

TEST_CASE_METHOD(ConnFailedFixture, "A never-connected remote host still offers Change Address",
                 "[modal][connection][change_host]") {
    // moonraker_is_remote settles only on a CONNECTED edge, and this prompt
    // fires exactly when there was none — so the subject still sits at its
    // default (local) here. The locality verdict must come from the ATTEMPTED
    // host instead, or every remote screen with an unreachable printer gets
    // the OK-only alert and loses its primary recovery path. 192.0.2.1 is
    // TEST-NET-1 (RFC 5737): guaranteed never to be one of our interfaces.
    Config* cfg = Config::get_instance();
    const std::string key = cfg->df() + "moonraker_host";
    const std::string prev = cfg->get<std::string>(key, "");
    cfg->set<std::string>(key, "192.0.2.1");
    helix::invalidate_host_identity_cache();

    helix::ui::show_connection_failed_modal("Connection Failed",
                                            "Unable to reach printer at 192.0.2.1:7125.");
    UpdateQueue::instance().drain();
    REQUIRE(Modal::get_top() != nullptr);

    // Confirmation-style dialog, not the alert the same-host path shows:
    // primary is Reconnect, and Change Address survives as the secondary
    // action (the merge of the reconnect-first flow moved it off primary).
    const std::string primary_text = UITest::button_text(Modal::get_top(), "btn_primary");
    CHECK(primary_text.find("Reconnect") != std::string::npos);
    const std::string cancel_text = UITest::button_text(Modal::get_top(), "btn_secondary");
    CHECK(cancel_text.find("Change Address") != std::string::npos);
    CHECK_FALSE(lv_obj_has_flag(lv_obj_find_by_name(Modal::get_top(), "btn_secondary"),
                                LV_OBJ_FLAG_HIDDEN));

    cfg->set<std::string>(key, prev);
    helix::invalidate_host_identity_cache();
}

TEST_CASE_METHOD(ConnFailedFixture, "An unconfigured host still offers Change Address",
                 "[modal][connection][change_host]") {
    // The guard above must key off a host we positively identified as local. An
    // empty host is the case where changing the address is precisely the fix, so
    // it must not be swept in by a "localhost" default.
    Config* cfg = Config::get_instance();
    const std::string key = cfg->df() + "moonraker_host";
    const std::string prev = cfg->get<std::string>(key, "");
    cfg->set<std::string>(key, std::string(""));
    helix::invalidate_host_identity_cache();

    helix::ui::show_connection_failed_modal("Connection Failed", "Unable to reach printer.");
    UpdateQueue::instance().drain();
    REQUIRE(Modal::get_top() != nullptr);

    const std::string cancel_text = UITest::button_text(Modal::get_top(), "btn_secondary");
    CHECK(cancel_text.find("Change Address") != std::string::npos);

    cfg->set<std::string>(key, prev);
    helix::invalidate_host_identity_cache();
}

TEST_CASE_METHOD(ConnFailedFixture,
                 "Connection-failed prompt waits while the printer picker is open",
                 "[modal][connection][change_host][multi-printer]") {
    ScopedGlobalClient client; // reports DISCONNECTED
    REQUIRE(register_component("printer_switch_menu"));
    helix::ui::PrinterSwitchMenu picker;
    picker.show(test_screen(), test_screen());
    REQUIRE(helix::ui::ContextMenu::active() != nullptr);

    helix::ui::show_connection_failed_modal("Connection Failed", "Unable to reach printer.");
    UpdateQueue::instance().drain();
    process_lvgl(1000);
    CHECK(Modal::get_top() == nullptr);

    picker.hide();
    UpdateQueue::instance().drain();
    process_lvgl(1000);
    UpdateQueue::instance().drain();
    CHECK(Modal::get_top() != nullptr);
}

TEST_CASE_METHOD(ConnFailedFixture,
                 "Connection-failed prompt is dropped when the user picks another printer",
                 "[modal][connection][change_host][multi-printer]") {
    ScopedGlobalClient client;
    REQUIRE(register_component("printer_switch_menu"));
    helix::Config* cfg = helix::Config::get_instance();
    const std::string saved_active = helix::ConfigTestAccess::active_printer_id(*cfg);
    helix::ui::PrinterSwitchMenu picker;
    picker.show(test_screen(), test_screen());

    helix::ui::show_connection_failed_modal("Connection Failed", "Unable to reach printer.");
    UpdateQueue::instance().drain();
    helix::ConfigTestAccess::active_printer_id(*cfg) = saved_active + "-other";
    picker.hide();
    UpdateQueue::instance().drain();
    process_lvgl(1000);
    UpdateQueue::instance().drain();

    CHECK(Modal::get_top() == nullptr);
    helix::ConfigTestAccess::active_printer_id(*cfg) = saved_active;
}

TEST_CASE_METHOD(ConnFailedFixture,
                 "Connection-failed prompt is dropped when the picker closes on a selection",
                 "[modal][connection][change_host][multi-printer]") {
    // The menu closes before the switch it dispatches has run, so the held prompt must not
    // wait for the active printer to change.
    ScopedGlobalClient client;
    REQUIRE(register_component("printer_switch_menu"));
    REQUIRE(register_component("components/printer_switch_row"));
    helix::Config* cfg = helix::Config::get_instance();
    const nlohmann::json saved_data = helix::ConfigTestAccess::data(*cfg);
    const std::string saved_active = helix::ConfigTestAccess::active_printer_id(*cfg);
    nlohmann::json data = saved_data;
    data["printers"]["alpha"]["printer_name"] = "Alpha";
    data["printers"]["beta"]["printer_name"] = "Beta";
    helix::ConfigTestAccess::data(*cfg) = data;
    helix::ConfigTestAccess::active_printer_id(*cfg) = "alpha";

    helix::ui::PrinterSwitchMenu picker;
    picker.show(test_screen(), test_screen());
    helix::ui::show_connection_failed_modal("Connection Failed", "Unable to reach printer.");
    UpdateQueue::instance().drain();

    lv_obj_t* row = lv_obj_find_by_name(test_screen(), "beta");
    REQUIRE(row != nullptr);
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(1000);
    UpdateQueue::instance().drain();

    CHECK(Modal::get_top() == nullptr);
    helix::ConfigTestAccess::data(*cfg) = saved_data;
    helix::ConfigTestAccess::active_printer_id(*cfg) = saved_active;
}

// ============================================================================
// Re-discovering a printer whose address changed (#1217)
// ============================================================================

TEST_CASE("find_moved_printer picks the one printer carrying the saved identity",
          "[connection][mdns][rediscover]") {
    using helix::DiscoveredPrinter;
    using helix::ui::find_moved_printer;
    const std::vector<DiscoveredPrinter> found = {
        {"voron", "voron.local", "192.168.1.50", 7125},
        {"k1", "k1.local", "192.168.1.60", 7125},
    };

    SECTION("a single match at another address") {
        auto moved = find_moved_printer(found, "192.168.1.20", 7125, {"voron"});
        REQUIRE(moved.has_value());
        CHECK(moved->ip_address == "192.168.1.50");
    }
    SECTION("hostnames compare without case, .local or a trailing dot") {
        const std::vector<DiscoveredPrinter> fqdn = {
            {"Voron", "Voron.local.", "192.168.1.50", 7125}};
        CHECK(find_moved_printer(fqdn, "192.168.1.20", 7125, {"VORON.local"}).has_value());
    }
    SECTION("no printer carries the identity") {
        CHECK_FALSE(find_moved_printer(found, "192.168.1.20", 7125, {"ender"}).has_value());
    }
    SECTION("two printers carry it") {
        auto twins = found;
        twins.push_back({"voron", "voron.local", "192.168.1.51", 7125});
        CHECK_FALSE(find_moved_printer(twins, "192.168.1.20", 7125, {"voron"}).has_value());
    }
    SECTION("the printer still answers at the saved address") {
        CHECK_FALSE(find_moved_printer(found, "192.168.1.50", 7125, {"voron"}).has_value());
    }
    SECTION("the saved host is a name, which re-resolves on its own") {
        CHECK_FALSE(find_moved_printer(found, "voron.local", 7125, {"voron"}).has_value());
    }
    SECTION("no identity is known") {
        CHECK_FALSE(find_moved_printer(found, "192.168.1.20", 7125, {"", ""}).has_value());
    }
}

namespace {

/// Hands out MockMdnsDiscovery instances answering with `printers`, counting each browse.
struct FakeMdnsSource {
    std::vector<helix::DiscoveredPrinter> printers;
    int browses = 0;

    FakeMdnsSource() {
        helix::ui::set_printer_rediscovery_source([this] {
            ++browses;
            auto mdns = std::make_unique<MockMdnsDiscovery>();
            for (const auto& p : printers) {
                mdns->add_fake_printer(p.name, p.hostname, p.ip_address, p.port);
            }
            return mdns;
        });
    }
    ~FakeMdnsSource() {
        helix::ui::set_printer_rediscovery_source(nullptr);
    }
};

/// Sets the active printer's saved host and hostname for one test.
struct SavedPrinter {
    Config* cfg = Config::get_instance();
    std::string host_key = cfg->df() + "moonraker_host";
    std::string port_key = cfg->df() + "moonraker_port";
    std::string hostname_key = cfg->df() + helix::wizard::kHostname;
    std::string prev_host = cfg->get<std::string>(host_key, "");
    int prev_port = cfg->get<int>(port_key, 7125);
    std::string prev_hostname = cfg->get<std::string>(hostname_key, "");

    SavedPrinter(const std::string& host, const std::string& hostname) {
        cfg->set<std::string>(host_key, host);
        cfg->set<int>(port_key, 7125);
        cfg->set<std::string>(hostname_key, hostname);
        helix::invalidate_host_identity_cache();
    }
    ~SavedPrinter() {
        cfg->set<std::string>(host_key, prev_host);
        cfg->set<int>(port_key, prev_port);
        cfg->set<std::string>(hostname_key, prev_hostname);
        helix::invalidate_host_identity_cache();
    }
};

} // namespace

TEST_CASE_METHOD(ConnFailedFixture, "Connection-failed prompt offers the printer's new address",
                 "[modal][connection][change_host][mdns][rediscover]") {
    ScopedGlobalClient client;
    SavedPrinter saved("192.0.2.1", "voron");
    FakeMdnsSource mdns;
    mdns.printers = {{"voron", "voron.local", "192.0.2.50", 7125}};

    helix::ui::show_connection_failed_modal("Connection Failed",
                                            "Unable to reach printer at 192.0.2.1:7125.");
    UpdateQueue::instance().drain();
    // The prompt waits for the browse, so it can carry what it found.
    CHECK(Modal::get_top() == nullptr);
    process_lvgl(helix::ui::kRediscoveryWindowMs + 100);
    UpdateQueue::instance().drain();

    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    CHECK(mdns.browses == 1);
    lv_obj_t* msg = lv_obj_find_by_name(dialog, "dialog_message");
    REQUIRE(msg != nullptr);
    CHECK(std::string(lv_label_get_text(msg)).find("192.0.2.50:7125") != std::string::npos);
    CHECK(UITest::button_text(dialog, "btn_primary").find("Use New Address") != std::string::npos);
    CHECK(UITest::button_text(dialog, "btn_secondary").find("Change Address") != std::string::npos);

    lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    CHECK(saved.cfg->get<std::string>(saved.host_key, "") == "192.0.2.50");
    CHECK(Modal::get_top() == nullptr);
}

TEST_CASE_METHOD(ConnFailedFixture,
                 "Connection-failed prompt offers nothing new without exactly one match",
                 "[modal][connection][change_host][mdns][rediscover]") {
    ScopedGlobalClient client;
    SavedPrinter saved("192.0.2.1", "voron");
    FakeMdnsSource mdns;

    SECTION("no printer answers") {}
    SECTION("only other printers answer") {
        mdns.printers = {{"k1", "k1.local", "192.0.2.60", 7125}};
    }
    SECTION("two printers carry the name") {
        mdns.printers = {{"voron", "voron.local", "192.0.2.50", 7125},
                         {"voron", "voron.local", "192.0.2.51", 7125}};
    }

    helix::ui::show_connection_failed_modal("Connection Failed",
                                            "Unable to reach printer at 192.0.2.1:7125.");
    UpdateQueue::instance().drain();
    process_lvgl(helix::ui::kRediscoveryWindowMs + 100);
    UpdateQueue::instance().drain();

    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    CHECK(mdns.browses == 1);
    CHECK(UITest::button_text(dialog, "btn_primary").find("Reconnect") != std::string::npos);
    CHECK(saved.cfg->get<std::string>(saved.host_key, "") == "192.0.2.1");
}

TEST_CASE_METHOD(ConnFailedFixture, "A failure escalation browses once",
                 "[modal][connection][change_host][mdns][rediscover]") {
    ScopedGlobalClient client;
    SavedPrinter saved("192.0.2.1", "voron");
    FakeMdnsSource mdns;

    helix::ui::show_connection_failed_modal("Connection Failed", "Unable to reach printer.");
    helix::ui::show_connection_failed_modal("Connection Failed", "Unable to reach printer.");
    UpdateQueue::instance().drain();
    process_lvgl(helix::ui::kRediscoveryWindowMs + 100);
    UpdateQueue::instance().drain();
    CHECK(mdns.browses == 1);
    process_lvgl(3 * helix::ui::kRediscoveryWindowMs);
    CHECK(mdns.browses == 1);

    // One prompt: dismissing it leaves nothing behind.
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    Modal::hide(dialog);
    UpdateQueue::instance().drain();
    process_lvgl(500);
    CHECK(Modal::get_top() == nullptr);
}

TEST_CASE_METHOD(ConnFailedFixture, "A browse outlived by the failure shows no prompt",
                 "[modal][connection][change_host][mdns][rediscover]") {
    ScopedGlobalClient client;
    SavedPrinter saved("192.0.2.1", "voron");
    FakeMdnsSource mdns;
    mdns.printers = {{"voron", "voron.local", "192.0.2.50", 7125}};
    Config* cfg = Config::get_instance();
    const std::string saved_active = helix::ConfigTestAccess::active_printer_id(*cfg);

    helix::ui::show_connection_failed_modal("Connection Failed", "Unable to reach printer.");
    UpdateQueue::instance().drain();
    SECTION("the printer reconnected meanwhile") {
        client.set_state(helix::ConnectionState::CONNECTED);
    }
    SECTION("the user switched printers meanwhile") {
        helix::ConfigTestAccess::active_printer_id(*cfg) = saved_active + "-other";
    }
    process_lvgl(helix::ui::kRediscoveryWindowMs + 100);
    UpdateQueue::instance().drain();
    helix::ConfigTestAccess::active_printer_id(*cfg) = saved_active;

    CHECK(mdns.browses == 1);
    CHECK(Modal::get_top() == nullptr);
}

TEST_CASE_METHOD(ConnFailedFixture, "Use New Address writes nothing after a printer switch",
                 "[modal][connection][change_host][mdns][rediscover]") {
    ScopedGlobalClient client;
    SavedPrinter saved("192.0.2.1", "voron");
    FakeMdnsSource mdns;
    mdns.printers = {{"voron", "voron.local", "192.0.2.50", 7125}};
    Config* cfg = Config::get_instance();
    const std::string saved_active = helix::ConfigTestAccess::active_printer_id(*cfg);

    helix::ui::show_connection_failed_modal("Connection Failed", "Unable to reach printer.");
    UpdateQueue::instance().drain();
    process_lvgl(helix::ui::kRediscoveryWindowMs + 100);
    UpdateQueue::instance().drain();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    REQUIRE(UITest::button_text(dialog, "btn_primary").find("Use New Address") !=
            std::string::npos);

    helix::ConfigTestAccess::active_printer_id(*cfg) = saved_active + "-other";
    lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    helix::ConfigTestAccess::active_printer_id(*cfg) = saved_active;

    CHECK(cfg->get<std::string>(saved.host_key, "") == "192.0.2.1");
}

TEST_CASE_METHOD(ConnFailedFixture, "The printer's hostname is saved only when it changes",
                 "[connection][mdns][rediscover]") {
    SavedPrinter saved("192.0.2.1", "");
    ScopedConfigWriteCounter writes;

    helix::remember_printer_hostname("voron");
    CHECK(saved.cfg->get<std::string>(saved.hostname_key, "") == "voron");
    CHECK(writes.writes() == 1);
    helix::remember_printer_hostname("voron");
    CHECK(writes.writes() == 1);
    helix::remember_printer_hostname("unknown");
    helix::remember_printer_hostname("");
    CHECK(writes.writes() == 1);
    helix::remember_printer_hostname("trident");
    CHECK(writes.writes() == 2);
}

TEST_CASE_METHOD(ConnFailedFixture, "A printer on this machine is never browsed for",
                 "[modal][connection][change_host][mdns][rediscover]") {
    ScopedGlobalClient client;
    SavedPrinter saved("127.0.0.1", "voron");
    FakeMdnsSource mdns;
    mdns.printers = {{"voron", "voron.local", "192.0.2.50", 7125}};

    helix::ui::show_connection_failed_modal("Connection Failed",
                                            "Moonraker is not responding at 127.0.0.1:7125.");
    UpdateQueue::instance().drain();
    REQUIRE(Modal::get_top() != nullptr);
    CHECK(mdns.browses == 0);
}
