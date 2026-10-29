// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_change_host_modal_restore.cpp
 * @brief The Change Host / Add Printer modal: Test, Save and the live client.
 *
 * Test Connection asks /server/info over HTTP and never touches the live client, so the
 * screen behind the modal keeps its printer; only Save moves the client.
 */

#include "ui_change_host_modal.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../fake_moonraker_client.h"
#include "../mocks/mock_mdns_discovery.h"
#include "../test_fixtures.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/fake_connection_test.h"
#include "../test_helpers/moonraker_manager_test_access.h"
#include "../test_helpers/scoped_moonraker_client.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "lan_sweep.h"
#include "moonraker_auth.h"
#include "moonraker_manager.h"
#include "platform_info.h"
#include "printer_discovery_list.h"
#include "printer_retarget.h"
#include "printer_state.h"
#include "printer_switch_flow.h"
#include "utils/network_validation.h"

#include <algorithm>
#include <lvgl.h>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

constexpr const char* kSavedHost = "192.168.1.50";
constexpr const char* kTestedUrl = "ws://10.9.9.9:7125/websocket";

class ChangeHostRestoreFixture : public XMLTestFixture {
  public:
    ChangeHostRestoreFixture() {
        helix::ui::modal_init_subjects();
        REQUIRE(register_component("modal_dialog"));
        REQUIRE(register_component("change_host_modal"));

        auto fake = std::make_unique<helix::test::FakeMoonrakerClient>();
        client_ = fake.get();
        helix::MoonrakerManagerTestAccess::install_client(manager_, std::move(fake));
        set_moonraker_manager(&manager_);
        installed_ = std::make_unique<ScopedMoonrakerClient>(client_);

        Config* cfg = Config::get_instance();
        host_key_ = cfg->df() + "moonraker_host";
        port_key_ = cfg->df() + "moonraker_port";
        prev_host_ = cfg->get<std::string>(host_key_, "");
        prev_port_ = cfg->get<int>(port_key_, 7125);
        cfg->set<std::string>(host_key_, kSavedHost);
        cfg->set<int>(port_key_, 7125);
    }

    ~ChangeHostRestoreFixture() override {
        while (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
            UpdateQueue::instance().drain();
        }
        UpdateQueue::instance().drain();
        Config* cfg = Config::get_instance();
        cfg->set<std::string>(host_key_, prev_host_);
        cfg->set<int>(port_key_, prev_port_);
        installed_.reset();
        set_moonraker_manager(nullptr);
    }

    /// Opens the modal, types `host` and presses Test Connection; the fake network has
    /// answered once this returns.
    lv_obj_t* open_and_test(const char* host) {
        helix::ui::show_change_host_modal();
        UpdateQueue::instance().drain();
        lv_obj_t* dialog = Modal::get_top();
        REQUIRE(dialog != nullptr);

        lv_subject_t* ip = lv_xml_get_subject(nullptr, "change_host_ip");
        REQUIRE(ip != nullptr);
        lv_subject_copy_string(ip, host);

        click(dialog, "btn_test_connection");
        REQUIRE_FALSE(net_.asked.empty());
        REQUIRE(net_.asked.front() == std::string(host) + ":7125");
        return dialog;
    }

    static void click(lv_obj_t* dialog, const char* name) {
        lv_obj_t* btn = lv_obj_find_by_name(dialog, name);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        UpdateQueue::instance().drain();
        // An add hands the address over on the next tick; its callback closes over the
        // test's locals, so it runs here rather than in a later test.
        lv_timer_handler();
        UpdateQueue::instance().drain();
    }

    helix::test::FakeMoonrakerClient* client_ = nullptr;
    helix::test::ScopedFakeConnectionTest net_;

  private:
    MoonrakerManager manager_;
    std::unique_ptr<ScopedMoonrakerClient> installed_;
    std::string host_key_, port_key_, prev_host_;
    int prev_port_ = 7125;
};

} // namespace

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: a test and Cancel never move the live client",
                 "[change_host][connection]") {
    net_.answer("10.9.9.9:7125");
    lv_obj_t* dialog = open_and_test("10.9.9.9");
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "change_host_validated")) == 1);

    click(dialog, "modal_cancel_btn");

    CHECK(Modal::get_top() == nullptr);
    CHECK(client_->connect_urls.empty());
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: dismissing after a test leaves the live client alone",
                 "[change_host][connection]") {
    lv_obj_t* dialog = open_and_test("10.9.9.9");

    // Backdrop tap and ESC reach Modal::hide() without the Cancel button.
    Modal::hide(dialog);
    UpdateQueue::instance().drain();

    CHECK(client_->connect_urls.empty());
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: Cancel without a test leaves the client alone",
                 "[change_host][connection]") {
    helix::ui::show_change_host_modal();
    UpdateQueue::instance().drain();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    click(dialog, "modal_cancel_btn");

    CHECK(client_->get_last_url().empty());
}

TEST_CASE_METHOD(ChangeHostRestoreFixture, "Change Host: Save keeps the tested host",
                 "[change_host][connection]") {
    net_.answer("10.9.9.9:7125");
    lv_obj_t* dialog = open_and_test("10.9.9.9");
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "change_host_validated")) == 1);
    click(dialog, "modal_save_btn");
    UpdateQueue::instance().drain();

    CHECK(client_->get_last_url() == kTestedUrl);
    CHECK(Config::get_instance()->get<std::string>(Config::get_instance()->df() +
                                                   "moonraker_host") == "10.9.9.9");
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Add printer: Save hands over the tested host and leaves the active printer alone",
                 "[change_host][connection][multi-printer]") {
    std::string added_host;
    int added_port = 0;
    std::string url_when_added;
    helix::ui::show_add_printer_modal([&](const std::string& host, int port) {
        added_host = host;
        added_port = port;
        url_when_added = client_->get_last_url();
        return false; // not switched
    });
    UpdateQueue::instance().drain();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    // A new printer starts from an empty host, under its own title.
    lv_subject_t* ip = lv_xml_get_subject(nullptr, "change_host_ip");
    REQUIRE(ip != nullptr);
    CHECK(std::string(lv_subject_get_string(ip)).empty());
    lv_obj_t* add_title = lv_obj_find_by_name(dialog, "title_add_printer");
    lv_obj_t* change_title = lv_obj_find_by_name(dialog, "title_change_host");
    REQUIRE(add_title != nullptr);
    REQUIRE(change_title != nullptr);
    CHECK_FALSE(lv_obj_has_flag(add_title, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(change_title, LV_OBJ_FLAG_HIDDEN));

    net_.answer("10.9.9.9:7125");
    lv_subject_copy_string(ip, "10.9.9.9");
    click(dialog, "btn_test_connection");
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "change_host_validated")) == 1);
    click(dialog, "modal_save_btn");
    UpdateQueue::instance().drain();

    CHECK(added_host == "10.9.9.9");
    CHECK(added_port == 7125);
    // The test never moved the client, so a declined switch leaves it where config and UI
    // say it is, with nothing to hand back.
    CHECK(url_when_added.empty());
    CHECK(client_->connect_urls.empty());
    Config* cfg = Config::get_instance();
    CHECK(cfg->get<std::string>(cfg->df() + "moonraker_host") == kSavedHost);
}

TEST_CASE_METHOD(ChangeHostRestoreFixture, "Change Host after Add shows the saved host again",
                 "[change_host][connection][multi-printer]") {
    helix::ui::show_add_printer_modal([](const std::string&, int) { return false; });
    UpdateQueue::instance().drain();
    click(Modal::get_top(), "modal_cancel_btn");

    helix::ui::show_change_host_modal();
    UpdateQueue::instance().drain();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "change_host_ip"))) ==
          kSavedHost);
    lv_obj_t* change_title = lv_obj_find_by_name(dialog, "title_change_host");
    REQUIRE(change_title != nullptr);
    CHECK_FALSE(lv_obj_has_flag(change_title, LV_OBJ_FLAG_HIDDEN));
}

namespace {

/// Installs one mock AMS backend, as printer A's discovery would, and removes it after.
class ScopedAmsBackend {
  public:
    ScopedAmsBackend() {
        auto& ams = helix::AmsState::instance();
        ams.init_subjects(false);
        ams.set_backend(std::make_unique<helix::AmsBackendMock>());
    }
    ~ScopedAmsBackend() {
        helix::AmsState::instance().set_backend(nullptr);
    }
};

} // namespace

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: saving a new host drops the old AMS backends",
                 "[change_host][connection][ams]") {
    ScopedAmsBackend printer_a_ams;
    REQUIRE(helix::AmsState::instance().backend_count() == 1);

    lv_obj_t* dialog = open_and_test("10.9.9.9");
    lv_subject_set_int(lv_xml_get_subject(nullptr, "change_host_validated"), 1);
    click(dialog, "modal_save_btn");
    UpdateQueue::instance().drain();

    // The new host's discovery builds backends only when none exist.
    CHECK(client_->get_last_url() == kTestedUrl);
    CHECK(helix::AmsState::instance().backend_count() == 0);
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: Cancel keeps the saved printer's AMS backends",
                 "[change_host][connection][ams]") {
    ScopedAmsBackend printer_a_ams;
    lv_obj_t* dialog = open_and_test("10.9.9.9");

    click(dialog, "modal_cancel_btn");

    CHECK(client_->connect_urls.empty());
    CHECK(helix::AmsState::instance().backend_count() == 1);
}

namespace {

struct AddResult {
    int calls = 0;
    std::string host;
};

lv_obj_t* open_add_modal(AddResult& result) {
    helix::ui::show_add_printer_modal([&result](const std::string& host, int) {
        ++result.calls;
        result.host = host;
        return false;
    });
    UpdateQueue::instance().drain();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "change_host_ip"), "10.9.9.9");
    return dialog;
}

} // namespace

TEST_CASE_METHOD(ChangeHostRestoreFixture, "Add printer: Save is enabled before any test",
                 "[change_host][multi-printer]") {
    AddResult result;
    lv_obj_t* dialog = open_add_modal(result);
    lv_obj_t* save = lv_obj_find_by_name(dialog, "modal_save_btn");
    REQUIRE(save != nullptr);
    CHECK_FALSE(lv_obj_has_state(save, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(ChangeHostRestoreFixture, "Change Host: Save stays disabled until a test passes",
                 "[change_host][multi-printer]") {
    helix::ui::show_change_host_modal();
    UpdateQueue::instance().drain();
    lv_obj_t* save = lv_obj_find_by_name(Modal::get_top(), "modal_save_btn");
    REQUIRE(save != nullptr);
    CHECK(lv_obj_has_state(save, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Add printer: an untested Save asks, and Save anyway adds",
                 "[change_host][multi-printer]") {
    AddResult result;
    lv_obj_t* dialog = open_add_modal(result);

    click(dialog, "modal_save_btn");
    lv_obj_t* confirm = Modal::get_top();
    REQUIRE(confirm != nullptr);
    REQUIRE(confirm != dialog);
    CHECK(result.calls == 0);

    click(confirm, "btn_primary");
    UpdateQueue::instance().drain();

    CHECK(result.calls == 1);
    CHECK(result.host == "10.9.9.9");
}

TEST_CASE_METHOD(ChangeHostRestoreFixture, "Add printer: Cancel on the question stays on the form",
                 "[change_host][multi-printer]") {
    AddResult result;
    lv_obj_t* dialog = open_add_modal(result);

    click(dialog, "modal_save_btn");
    lv_obj_t* confirm = Modal::get_top();
    REQUIRE(confirm != dialog);
    click(confirm, "btn_secondary");
    UpdateQueue::instance().drain();

    CHECK(result.calls == 0);
    CHECK(Modal::get_top() == dialog);
}

TEST_CASE_METHOD(ChangeHostRestoreFixture, "Add printer: a passed test saves without asking",
                 "[change_host][multi-printer]") {
    AddResult result;
    lv_obj_t* dialog = open_add_modal(result);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "change_host_validated"), 1);

    click(dialog, "modal_save_btn");
    UpdateQueue::instance().drain();

    CHECK(result.calls == 1);
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Add printer: a blank host is refused before the question",
                 "[change_host][multi-printer]") {
    const char* host = GENERATE("", "   ");
    AddResult result;
    lv_obj_t* dialog = open_add_modal(result);
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "change_host_ip"), host);

    click(dialog, "modal_save_btn");
    UpdateQueue::instance().drain();

    CHECK(Modal::get_top() == dialog);
    CHECK(result.calls == 0);
    lv_obj_t* status = lv_obj_find_by_name(dialog, "status_text");
    REQUIRE(status != nullptr);
    CHECK_FALSE(std::string(lv_label_get_text(status)).empty());
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Add printer: typing a host clears the validation message",
                 "[change_host][multi-printer]") {
    AddResult result;
    lv_obj_t* dialog = open_add_modal(result);
    lv_subject_t* ip = lv_xml_get_subject(nullptr, "change_host_ip");
    lv_subject_copy_string(ip, "");
    click(dialog, "modal_save_btn");
    lv_obj_t* status = lv_obj_find_by_name(dialog, "status_text");
    REQUIRE(status != nullptr);
    REQUIRE_FALSE(std::string(lv_label_get_text(status)).empty());

    lv_subject_copy_string(ip, "10.1.2.3");
    UpdateQueue::instance().drain();

    CHECK(std::string(lv_label_get_text(status)).empty());
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: a failed test names what to check, as the wizard does",
                 "[change_host][connect_failure]") {
    const auto kind =
        GENERATE(helix::ConnectFailureKind::Timeout, helix::ConnectFailureKind::Refused,
                 helix::ConnectFailureKind::Unknown);
    net_.failure = kind;
    lv_obj_t* dialog = open_and_test("10.9.9.9");

    // The untouched default also tries port 80 before giving up.
    CHECK(net_.asked == std::vector<std::string>{"10.9.9.9:7125", "10.9.9.9:80"});

    lv_obj_t* status = lv_obj_find_by_name(dialog, "status_text");
    REQUIRE(status != nullptr);
    CHECK(std::string(lv_label_get_text(status)) ==
          helix::connect_failure_status(kind, {7125, 80}));
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: Moonraker found on port 80 is what Save stores",
                 "[change_host][port_fallback]") {
    net_.answer("10.9.9.9:80", "ready");
    lv_obj_t* dialog = open_and_test("10.9.9.9");

    CHECK(net_.asked == std::vector<std::string>{"10.9.9.9:7125", "10.9.9.9:80"});
    CHECK(client_->connect_urls.empty());
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "change_host_port"))) ==
          "80");
    lv_obj_t* status = lv_obj_find_by_name(dialog, "status_text");
    REQUIRE(status != nullptr);
    const std::string shown = lv_label_get_text(status);
    CHECK(shown.find("10.9.9.9:80") != std::string::npos);
    CHECK(shown.find("ready") != std::string::npos);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "change_host_validated")) == 1);

    click(dialog, "modal_save_btn");
    Config* cfg = Config::get_instance();
    CHECK(cfg->get<int>(cfg->df() + "moonraker_port", 0) == 80);
    CHECK(cfg->get<std::string>(cfg->df() + "moonraker_host", "") == "10.9.9.9");
}

TEST_CASE_METHOD(ChangeHostRestoreFixture, "Change Host: a typed port is the only port tried",
                 "[change_host][port_fallback]") {
    helix::ui::show_change_host_modal();
    UpdateQueue::instance().drain();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "change_host_ip"), "10.9.9.9");
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "change_host_port"), "7125");
    click(dialog, "btn_test_connection");

    CHECK(net_.asked == std::vector<std::string>{"10.9.9.9:7125"});
}

// The K-Touch hands Add to PrinterSwitchFlow::add_printer, which reports whether it switched.
// Reached from inside an UpdateQueue drain the flow can only defer and say no, so the modal
// must ask from the next tick, where the answer is final.
TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Add printer: a Save pressed inside a drain gets the switch's final answer",
                 "[change_host][multi-printer][switch_flow]") {
    Config* cfg = Config::get_instance();
    const nlohmann::json saved_data = helix::ConfigTestAccess::data(*cfg);
    const std::string saved_active = helix::ConfigTestAccess::active_printer_id(*cfg);
    const bool saved_read_only = helix::ConfigTestAccess::read_only_mode(*cfg);
    helix::ConfigTestAccess::read_only_mode(*cfg) = false;
    get_printer_state().init_subjects(false);

    std::vector<std::string> events;
    helix::AsyncLifetimeGuard async;
    helix::PrinterSwitchFlow flow(cfg, async,
                                  {[&] { events.push_back("teardown"); },
                                   [&] {
                                       events.push_back("rebuild");
                                       return false;
                                   },
                                   [&] { events.push_back("home"); }});
    flow.set_connected_printer_id(cfg->get_active_printer_id());

    std::vector<bool> answers;
    helix::ui::show_add_printer_modal([&](const std::string& host, int port) {
        answers.push_back(flow.add_printer(host, port));
        return answers.back();
    });
    UpdateQueue::instance().drain();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    net_.answer("10.9.9.9:7125");
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "change_host_ip"), "10.9.9.9");
    click(dialog, "btn_test_connection");
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "change_host_validated")) == 1);

    lv_obj_t* save = lv_obj_find_by_name(dialog, "modal_save_btn");
    REQUIRE(save != nullptr);
    helix::ui::queue_update("test::save_in_drain",
                            [save] { lv_obj_send_event(save, LV_EVENT_CLICKED, nullptr); });
    UpdateQueue::instance().drain();
    lv_timer_handler();
    UpdateQueue::instance().drain();
    lv_timer_handler();
    UpdateQueue::instance().drain();

    CHECK(answers == std::vector<bool>{true});
    CHECK(events == std::vector<std::string>{"teardown", "rebuild", "home"});
    // The test never moved the client, and the modal hands nothing back after a switch.
    CHECK(client_->connect_urls.empty());

    helix::ConfigTestAccess::data(*cfg) = saved_data;
    helix::ConfigTestAccess::active_printer_id(*cfg) = saved_active;
    helix::ConfigTestAccess::read_only_mode(*cfg) = saved_read_only;
}

namespace {

/// Records the stop the modal owes its discovery when it closes.
class StopRecordingDiscovery : public MockMdnsDiscovery {
  public:
    explicit StopRecordingDiscovery(bool& stopped) : stopped_(stopped) {}
    void stop_discovery() override {
        stopped_ = true;
        MockMdnsDiscovery::stop_discovery();
    }

  private:
    bool& stopped_;
};

} // namespace

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Add printer: a printer the sweep found fills host and port",
                 "[change_host][lan_sweep]") {
    bool stopped = false;
    auto mock = std::make_unique<StopRecordingDiscovery>(stopped);
    MockMdnsDiscovery* found = mock.get();
    found->add_fake_printer("", "", "192.168.1.52", 80);
    helix::set_printer_discovery_source(
        [&mock]() -> std::unique_ptr<helix::IMdnsDiscovery> { return std::move(mock); });

    helix::ui::show_add_printer_modal([](const std::string&, int) { return false; });
    UpdateQueue::instance().drain();
    helix::set_printer_discovery_source(nullptr);
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    REQUIRE(found->is_discovering());

    lv_obj_t* section = lv_obj_find_by_name(dialog, "discovery_section");
    REQUIRE(section != nullptr);
    CHECK_FALSE(lv_obj_has_flag(section, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* dropdown = lv_obj_find_by_name(dialog, "printer_dropdown");
    REQUIRE(dropdown != nullptr);
    REQUIRE(lv_dropdown_get_option_count(dropdown) == 2);

    lv_dropdown_set_selected(dropdown, 1);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "change_host_ip"))) ==
          "192.168.1.52");
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "change_host_port"))) ==
          "80");

    click(dialog, "btn_test_connection");
    CHECK(net_.asked == std::vector<std::string>{"192.168.1.52:80"});

    // Closing the modal stops discovery.
    REQUIRE_FALSE(stopped);
    click(dialog, "modal_cancel_btn");
    CHECK(stopped);
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: a saved port other than 7125 is tried alone",
                 "[change_host][port_fallback]") {
    Config* cfg = Config::get_instance();
    cfg->set<int>(cfg->df() + "moonraker_port", 80);
    helix::ui::show_change_host_modal();
    UpdateQueue::instance().drain();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "change_host_port"))) ==
            "80");
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "change_host_ip"), "10.9.9.9");
    click(dialog, "btn_test_connection");

    CHECK(net_.asked == std::vector<std::string>{"10.9.9.9:80"});
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Add printer: a discovered printer on 7125 is tried on 7125 alone",
                 "[change_host][port_fallback][lan_sweep]") {
    auto mock = std::make_unique<MockMdnsDiscovery>();
    mock->add_fake_printer("voron", "voron.local", "192.168.1.112", 7125);
    helix::set_printer_discovery_source(
        [&mock]() -> std::unique_ptr<helix::IMdnsDiscovery> { return std::move(mock); });
    helix::ui::show_add_printer_modal([](const std::string&, int) { return false; });
    UpdateQueue::instance().drain();
    helix::set_printer_discovery_source(nullptr);
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    lv_obj_t* dropdown = lv_obj_find_by_name(dialog, "printer_dropdown");
    REQUIRE(dropdown != nullptr);
    lv_dropdown_set_selected(dropdown, 1);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    click(dialog, "btn_test_connection");

    CHECK(net_.asked == std::vector<std::string>{"192.168.1.112:7125"});
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: with no discovery source the discovered-printers row stays hidden",
                 "[change_host][lan_sweep]") {
    helix::set_printer_discovery_source(nullptr);
    helix::ui::show_change_host_modal();
    UpdateQueue::instance().drain();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    lv_obj_t* section = lv_obj_find_by_name(dialog, "discovery_section");
    REQUIRE(section != nullptr);
    CHECK(lv_obj_has_flag(section, LV_OBJ_FLAG_HIDDEN));
}

// ============================================================================
// The API key field (prestonbrown/helixscreen#1929)
// ============================================================================

namespace {

constexpr const char* kGoodKey = "0123456789abcdef0123456789abcdef";

/// Puts the active printer's saved key back, and records no credential for the next test.
class SavedKeyGuard {
  public:
    SavedKeyGuard() {
        Config* cfg = Config::get_instance();
        key_path_ = cfg->df() + "moonraker_api_key";
        if (const nlohmann::json* saved = cfg->try_get_json(key_path_)) {
            saved_ = *saved;
        }
    }
    ~SavedKeyGuard() {
        Config::get_instance()->set(key_path_, saved_);
        helix::moonraker_auth::set_credential({});
    }

  private:
    std::string key_path_;
    nlohmann::json saved_ = false;
};

void type_api_key(const char* key) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, "change_host_api_key");
    REQUIRE(subject != nullptr);
    lv_subject_copy_string(subject, key);
}

std::string status_text(lv_obj_t* dialog) {
    lv_obj_t* label = lv_obj_find_by_name(dialog, "status_text");
    REQUIRE(label != nullptr);
    return lv_label_get_text(label);
}

nlohmann::json saved_api_key() {
    Config* cfg = Config::get_instance();
    const nlohmann::json* key = cfg->try_get_json(cfg->df() + "moonraker_api_key");
    return key ? *key : nlohmann::json();
}

} // namespace

TEST_CASE_METHOD(ChangeHostRestoreFixture, "Change Host: the field shows the printer's saved key",
                 "[change_host][api-key]") {
    SavedKeyGuard guard;
    Config::get_instance()->set(Config::get_instance()->df() + "moonraker_api_key",
                                std::string(kGoodKey));
    helix::ui::show_change_host_modal();
    UpdateQueue::instance().drain();
    lv_obj_t* key_input = lv_obj_find_by_name(Modal::get_top(), "password_input");
    REQUIRE(key_input != nullptr);

    CHECK(std::string(lv_textarea_get_text(key_input)) == kGoodKey);
    // Masked until the user reveals it.
    CHECK(lv_textarea_get_password_mode(key_input));
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: a key that is not 32 hex digits is refused before any test",
                 "[change_host][api-key]") {
    SavedKeyGuard guard;
    helix::ui::show_change_host_modal();
    UpdateQueue::instance().drain();
    lv_obj_t* dialog = Modal::get_top();
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "change_host_ip"), "10.9.9.9");
    type_api_key("not-a-key");

    click(dialog, "btn_test_connection");

    CHECK(net_.asked.empty());
    CHECK(status_text(dialog) == helix::api_key_problem("not-a-key"));
    CHECK_FALSE(status_text(dialog).empty());
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: Test Connection carries the key and Save stores it with the printer",
                 "[change_host][api-key]") {
    SavedKeyGuard guard;
    net_.require_key("10.9.9.9:7125", kGoodKey);
    helix::ui::show_change_host_modal();
    UpdateQueue::instance().drain();
    lv_obj_t* dialog = Modal::get_top();
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "change_host_ip"), "10.9.9.9");
    // Pasted with a stray space: the key is the 32 digits.
    type_api_key((std::string(" ") + kGoodKey + " ").c_str());

    click(dialog, "btn_test_connection");
    REQUIRE(net_.keys_sent == std::vector<std::string>{kGoodKey});
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "change_host_validated")) == 1);

    click(dialog, "modal_save_btn");
    UpdateQueue::instance().drain();

    CHECK(saved_api_key() == kGoodKey);
    // The reconnect recorded it for that printer, and only that printer.
    CHECK(client_->get_last_url() == kTestedUrl);
    CHECK(helix::moonraker_auth::key_for("10.9.9.9", 7125) == kGoodKey);
    CHECK(helix::moonraker_auth::key_for(kSavedHost, 7125).empty());
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: a printer that refuses the test keeps Save shut and says why",
                 "[change_host][api-key]") {
    SavedKeyGuard guard;
    net_.require_key("10.9.9.9:7125", kGoodKey);
    helix::ui::show_change_host_modal();
    UpdateQueue::instance().drain();
    lv_obj_t* dialog = Modal::get_top();
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "change_host_ip"), "10.9.9.9");

    SECTION("no key: it needs one") {
        click(dialog, "btn_test_connection");
        CHECK(status_text(dialog).find("requires an API key") != std::string::npos);
    }
    SECTION("a wrong key: it was rejected") {
        type_api_key("ffffffffffffffffffffffffffffffff");
        click(dialog, "btn_test_connection");
        CHECK(status_text(dialog).find("rejected the API key") != std::string::npos);
    }
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "change_host_validated")) == 0);
    lv_obj_t* save = lv_obj_find_by_name(dialog, "modal_save_btn");
    REQUIRE(save != nullptr);
    CHECK(lv_obj_has_state(save, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: clearing the key saves the printer with none",
                 "[change_host][api-key]") {
    SavedKeyGuard guard;
    Config::get_instance()->set(Config::get_instance()->df() + "moonraker_api_key",
                                std::string(kGoodKey));
    net_.answer("10.9.9.9:7125");
    helix::ui::show_change_host_modal();
    UpdateQueue::instance().drain();
    lv_obj_t* dialog = Modal::get_top();
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "change_host_ip"), "10.9.9.9");
    type_api_key("");
    click(dialog, "btn_test_connection");
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "change_host_validated")) == 1);

    click(dialog, "modal_save_btn");
    UpdateQueue::instance().drain();

    CHECK(saved_api_key() == false);
    CHECK(helix::moonraker_auth::key_for("10.9.9.9", 7125).empty());
}

TEST_CASE_METHOD(ChangeHostRestoreFixture, "Add printer: the active printer's key is not offered",
                 "[change_host][api-key][multi-printer]") {
    SavedKeyGuard guard;
    Config::get_instance()->set(Config::get_instance()->df() + "moonraker_api_key",
                                std::string(kGoodKey));
    helix::ui::show_add_printer_modal([](const std::string&, int) { return false; });
    UpdateQueue::instance().drain();

    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "change_host_api_key")))
              .empty());
    lv_obj_t* section = lv_obj_find_by_name(Modal::get_top(), "api_key_section");
    REQUIRE(section != nullptr);
    CHECK(lv_obj_has_flag(section, LV_OBJ_FLAG_HIDDEN));
}
