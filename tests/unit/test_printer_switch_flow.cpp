// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_switch_flow.cpp
 * @brief PrinterSwitchFlow on its own, with recording restart hooks.
 *
 * The desktop's Application-level cases live in
 * tests/unit/application/test_application_printer_switch.cpp. These cover what the flow adds
 * for a user's pick: nothing happens for the active printer, and a printer that is printing
 * is only left after the user confirms.
 */

#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "config.h"
#include "printer_cache_registry.h"
#include "printer_state.h"
#include "printer_switch_flow.h"

#include <lvgl.h>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrintJobState;
using helix::ui::UpdateQueue;

namespace {

class SwitchFlowFixture : public XMLTestFixture {
  public:
    SwitchFlowFixture()
        : flow_(cfg_, async_,
                {[this] {
                     events_.push_back("teardown");
                     active_at_teardown_ = cfg_->get_active_printer_id();
                 },
                 [this] { events_.push_back("rebuild"); }, [this] { events_.push_back("home"); }}) {
        helix::ui::modal_init_subjects();
        REQUIRE(register_component("modal_dialog"));

        cfg_ = helix::Config::get_instance();
        saved_data_ = helix::ConfigTestAccess::data(*cfg_);
        saved_active_ = helix::ConfigTestAccess::active_printer_id(*cfg_);
        nlohmann::json data;
        data["config_version"] = 3;
        data["active_printer_id"] = "alpha";
        data["printers"]["alpha"]["printer_name"] = "Alpha";
        data["printers"]["beta"]["printer_name"] = "Beta";
        helix::ConfigTestAccess::data(*cfg_) = data;
        helix::ConfigTestAccess::active_printer_id(*cfg_) = "alpha";
        helix::PrinterCacheRegistry::instance().clear();

        saved_read_only_ = helix::ConfigTestAccess::read_only_mode(*cfg_);
        helix::ConfigTestAccess::read_only_mode(*cfg_) = false;
        flow_.set_connected_printer_id("alpha");

        get_printer_state().init_subjects(false);
        set_job(PrintJobState::STANDBY);
    }

    ~SwitchFlowFixture() override {
        while (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
            UpdateQueue::instance().drain();
        }
        UpdateQueue::instance().drain();
        set_job(PrintJobState::STANDBY);
        helix::ConfigTestAccess::data(*cfg_) = saved_data_;
        helix::ConfigTestAccess::active_printer_id(*cfg_) = saved_active_;
        helix::ConfigTestAccess::read_only_mode(*cfg_) = saved_read_only_;
    }

    static void set_job(PrintJobState state) {
        helix::test::set_wire_state(get_printer_state(), state);
        UpdateQueue::instance().drain();
    }

    static void click(lv_obj_t* dialog, const char* name) {
        lv_obj_t* btn = lv_obj_find_by_name(dialog, name);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        UpdateQueue::instance().drain();
        UpdateQueue::instance().drain();
    }

    helix::Config* cfg_ = nullptr;
    helix::AsyncLifetimeGuard async_;
    std::vector<std::string> events_;
    std::string active_at_teardown_;
    helix::PrinterSwitchFlow flow_;

  private:
    nlohmann::json saved_data_;
    std::string saved_active_;
    bool saved_read_only_ = false;
};

const std::vector<std::string> kFullRestart = {"teardown", "rebuild", "home"};

} // namespace

TEST_CASE_METHOD(SwitchFlowFixture, "Switch flow: an idle printer switches at once",
                 "[multi-printer][switch_flow]") {
    flow_.request_switch("beta");

    CHECK(Modal::get_top() == nullptr);
    CHECK(events_ == kFullRestart);
    CHECK(active_at_teardown_ == "beta");
    CHECK(cfg_->get_active_printer_id() == "beta");
}

TEST_CASE_METHOD(SwitchFlowFixture, "Switch flow: picking the active printer does nothing",
                 "[multi-printer][switch_flow]") {
    set_job(PrintJobState::PRINTING);
    flow_.request_switch("alpha");

    CHECK(Modal::get_top() == nullptr);
    CHECK(events_.empty());
}

TEST_CASE_METHOD(SwitchFlowFixture, "Switch flow: leaving a printing printer asks first",
                 "[multi-printer][switch_flow]") {
    const PrintJobState state = GENERATE(PrintJobState::PRINTING, PrintJobState::PAUSED);
    set_job(state);

    flow_.request_switch("beta");
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    CHECK(events_.empty());
    CHECK(cfg_->get_active_printer_id() == "alpha");

    // A second pick while the question is open is ignored.
    flow_.request_switch("beta");
    CHECK(events_.empty());

    click(dialog, "btn_primary");

    CHECK(events_ == kFullRestart);
    CHECK(cfg_->get_active_printer_id() == "beta");
}

TEST_CASE_METHOD(SwitchFlowFixture, "Switch flow: Cancel stays on the printing printer",
                 "[multi-printer][switch_flow]") {
    set_job(PrintJobState::PRINTING);
    flow_.request_switch("beta");
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    click(dialog, "btn_secondary");

    CHECK(events_.empty());
    CHECK(cfg_->get_active_printer_id() == "alpha");

    // The question was answered, so the next pick asks again rather than being ignored.
    flow_.request_switch("beta");
    CHECK(Modal::get_top() != nullptr);
}

TEST_CASE_METHOD(SwitchFlowFixture, "Switch flow: dismissing the question lets the next pick ask",
                 "[multi-printer][switch_flow]") {
    set_job(PrintJobState::PRINTING);
    flow_.request_switch("beta");
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    Modal::hide(dialog, ModalCloseReason::BackdropTap);
    UpdateQueue::instance().drain();
    REQUIRE(Modal::get_top() == nullptr);

    flow_.request_switch("beta");
    CHECK(Modal::get_top() != nullptr);
    CHECK(events_.empty());
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: deleting the connected printer switches to the one config moved to",
                 "[multi-printer][switch_flow]") {
    // Config::remove_printer() moves the active id to the remaining printer before the
    // Printers list asks for a switch to that same id.
    cfg_->remove_printer("alpha");
    REQUIRE(cfg_->get_active_printer_id() == "beta");

    flow_.request_switch("beta");

    CHECK(events_ == kFullRestart);
    CHECK(flow_.connected_printer_id() == "beta");
}

TEST_CASE_METHOD(SwitchFlowFixture, "Switch flow: a switch records the newly connected printer",
                 "[multi-printer][switch_flow]") {
    flow_.request_switch("beta");
    REQUIRE(flow_.connected_printer_id() == "beta");
    events_.clear();

    flow_.request_switch("beta");
    CHECK(events_.empty());
}

TEST_CASE_METHOD(SwitchFlowFixture, "Switch flow: a failed save stays on the current printer",
                 "[multi-printer][switch_flow]") {
    helix::ConfigTestAccess::read_only_mode(*cfg_) = true;

    flow_.request_switch("beta");

    CHECK(events_.empty());
    CHECK(cfg_->get_active_printer_id() == "alpha");
    CHECK(flow_.connected_printer_id() == "alpha");
}

TEST_CASE_METHOD(SwitchFlowFixture, "Switch flow: adding a new address creates it and switches",
                 "[multi-printer][switch_flow]") {
    flow_.add_printer("10.0.0.9", 7125);

    const std::string id = cfg_->get_active_printer_id();
    CHECK(id != "alpha");
    CHECK(id != "beta");
    CHECK(cfg_->get<std::string>("/printers/" + id + "/moonraker_host") == "10.0.0.9");
    CHECK(cfg_->get<int>("/printers/" + id + "/moonraker_port") == 7125);
    CHECK(events_ == kFullRestart);
    CHECK(flow_.connected_printer_id() == id);
}

TEST_CASE_METHOD(SwitchFlowFixture, "Switch flow: adding a known address switches to that printer",
                 "[multi-printer][switch_flow]") {
    nlohmann::json& data = helix::ConfigTestAccess::data(*cfg_);
    data["printers"]["beta"]["moonraker_host"] = "10.0.0.2";
    data["printers"]["beta"]["moonraker_port"] = 7125;

    flow_.add_printer("10.0.0.2", 7125);

    CHECK(cfg_->get_printer_ids().size() == 2);
    CHECK(cfg_->get_active_printer_id() == "beta");
    CHECK(events_ == kFullRestart);
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: an add whose save fails keeps the entry and stays",
                 "[multi-printer][switch_flow]") {
    helix::ConfigTestAccess::read_only_mode(*cfg_) = true;

    flow_.add_printer("10.0.0.9", 7125);

    CHECK(cfg_->get_printer_ids().size() == 3);
    CHECK(cfg_->get_active_printer_id() == "alpha");
    CHECK(events_.empty());
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: deleting the connected printer while it prints does not ask again",
                 "[multi-printer][switch_flow]") {
    // The Printers list already confirmed the removal; "still printing" would name a printer
    // the list no longer has, and Cancel would leave the app on it.
    set_job(PrintJobState::PRINTING);
    cfg_->remove_printer("alpha");

    flow_.request_switch("beta");

    CHECK(Modal::get_top() == nullptr);
    CHECK(events_ == kFullRestart);
}
