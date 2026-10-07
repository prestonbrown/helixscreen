// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_change_host_modal_restore.cpp
 * @brief Leaving the Change Host modal without saving keeps the saved printer connected.
 *
 * Test Connection borrows the live client: it disconnects from the saved host and connects
 * to the one being typed. Closing the modal any way other than Save must put the client
 * back on the saved host, or the app stays attached to a printer its config does not name.
 */

#include "ui_change_host_modal.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../fake_moonraker_client.h"
#include "../test_fixtures.h"
#include "../test_helpers/moonraker_manager_test_access.h"
#include "../test_helpers/scoped_moonraker_client.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "moonraker_manager.h"
#include "printer_retarget.h"

#include <lvgl.h>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

constexpr const char* kSavedHost = "192.168.1.50";
constexpr const char* kSavedUrl = "ws://192.168.1.50:7125/websocket";
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

    /// Opens the modal, types `host` and presses Test Connection.
    lv_obj_t* open_and_test(const char* host) {
        helix::ui::show_change_host_modal();
        UpdateQueue::instance().drain();
        lv_obj_t* dialog = Modal::get_top();
        REQUIRE(dialog != nullptr);

        lv_subject_t* ip = lv_xml_get_subject(nullptr, "change_host_ip");
        REQUIRE(ip != nullptr);
        lv_subject_copy_string(ip, host);

        click(dialog, "btn_test_connection");
        REQUIRE(client_->get_last_url() == kTestedUrl);
        return dialog;
    }

    static void click(lv_obj_t* dialog, const char* name) {
        lv_obj_t* btn = lv_obj_find_by_name(dialog, name);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        UpdateQueue::instance().drain();
    }

    helix::test::FakeMoonrakerClient* client_ = nullptr;

  private:
    MoonrakerManager manager_;
    std::unique_ptr<ScopedMoonrakerClient> installed_;
    std::string host_key_, port_key_, prev_host_;
    int prev_port_ = 7125;
};

} // namespace

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: Cancel after a test reconnects the saved host",
                 "[change_host][connection]") {
    lv_obj_t* dialog = open_and_test("10.9.9.9");

    click(dialog, "modal_cancel_btn");

    CHECK(Modal::get_top() == nullptr);
    CHECK(client_->get_last_url() == kSavedUrl);
}

TEST_CASE_METHOD(ChangeHostRestoreFixture,
                 "Change Host: dismissing after a test reconnects the saved host",
                 "[change_host][connection]") {
    lv_obj_t* dialog = open_and_test("10.9.9.9");

    // Backdrop tap and ESC reach Modal::hide() without the Cancel button.
    Modal::hide(dialog);
    UpdateQueue::instance().drain();

    CHECK(client_->get_last_url() == kSavedUrl);
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
    lv_obj_t* dialog = open_and_test("10.9.9.9");

    // The fake never calls back, so mark the test passed the way on_test_success does.
    lv_subject_t* validated = lv_xml_get_subject(nullptr, "change_host_validated");
    REQUIRE(validated != nullptr);
    lv_subject_set_int(validated, 1);
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

    lv_subject_copy_string(ip, "10.9.9.9");
    click(dialog, "btn_test_connection");
    REQUIRE(client_->get_last_url() == kTestedUrl);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "change_host_validated"), 1);
    click(dialog, "modal_save_btn");
    UpdateQueue::instance().drain();

    CHECK(added_host == "10.9.9.9");
    CHECK(added_port == 7125);
    // The client is back on the saved printer before the caller decides whether to switch,
    // so a declined or failed switch leaves it where config and UI say it is.
    CHECK(url_when_added == kSavedUrl);
    Config* cfg = Config::get_instance();
    CHECK(cfg->get<std::string>(cfg->df() + "moonraker_host") == kSavedHost);
}

TEST_CASE_METHOD(ChangeHostRestoreFixture, "Change Host after Add shows the saved host again",
                 "[change_host][connection][multi-printer]") {
    helix::ui::show_add_printer_modal([](const std::string&, int) {});
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

    CHECK(client_->get_last_url() == kSavedUrl);
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
