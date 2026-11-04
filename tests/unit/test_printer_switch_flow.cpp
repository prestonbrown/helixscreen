// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_switch_flow.cpp
 * @brief PrinterSwitchFlow on its own, with recording restart hooks.
 *
 * The desktop's Application-level cases live in
 * tests/unit/application/test_application_printer_switch.cpp. These cover what the flow adds
 * for a user's pick: nothing happens for the active printer, and a printer that is printing
 * is only left after the user confirms. The switch card on the top layer covers every restart
 * until the new printer connects.
 */

#include "ui_modal.h"
#include "ui_spinner.h"

#include "../test_fixtures.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "boot_crash_guard.h"
#include "config.h"
#include "connection_state.h"
#include "core/update_queue.h"
#include "platform_capabilities.h"
#include "printer_cache_registry.h"
#include "printer_state.h"
#include "printer_switch_flow.h"

#include <algorithm>
#include <functional>
#include <lvgl.h>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrintJobState;
using helix::ui::UpdateQueue;

namespace helix {
class PrinterSwitchFlowTestAccess {
  public:
    static lv_timer_t* connect_timeout(PrinterSwitchFlow& flow) {
        return flow.m_connect_timeout.get();
    }
};
} // namespace helix

namespace {

/// The switch cards on the top layer that are still showing; one being deleted is hidden.
std::vector<lv_obj_t*> visible_switch_cards() {
    std::vector<lv_obj_t*> cards;
    lv_obj_t* top = lv_layer_top();
    for (uint32_t i = 0; i < lv_obj_get_child_count(top); ++i) {
        lv_obj_t* child = lv_obj_get_child(top, static_cast<int32_t>(i));
        const char* name = lv_obj_get_name(child);
        if (name && std::string(name) == "printer_switch_interstitial" &&
            !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            cards.push_back(child);
        }
    }
    return cards;
}

/// "title / phase" of the one card showing, "none" without one, "several" with more.
std::string switch_card() {
    const auto cards = visible_switch_cards();
    if (cards.empty()) {
        return "none";
    }
    if (cards.size() > 1) {
        return "several";
    }
    auto text = [&](const char* name) {
        lv_obj_t* label = lv_obj_find_by_name(cards.front(), name);
        return std::string(label ? lv_label_get_text(label) : "?");
    };
    return text("switch_interstitial_title") + " / " + text("switch_interstitial_phase");
}

class SwitchFlowFixture : public XMLTestFixture {
  public:
    SwitchFlowFixture()
        : flow_(cfg_, async_,
                {[this] {
                     events_.push_back("teardown");
                     active_at_teardown_ = cfg_->get_active_printer_id();
                     card_at_teardown_ = switch_card();
                     locality_at_teardown_ = get_printer_state().moonraker_locality();
                     flushes_at_teardown_ = s_flushes;
                 },
                 [this] {
                     events_.push_back("rebuild");
                     card_at_rebuild_ = switch_card();
                     if (on_rebuild_) {
                         on_rebuild_();
                     }
                     return rebuild_connects_;
                 },
                 [this] { events_.push_back("home"); }}) {
        helix::ui::modal_init_subjects();
        REQUIRE(register_component("modal_dialog"));
        ui_spinner_init();
        REQUIRE(register_component("printer_switch_interstitial"));

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
        set_connection(helix::ConnectionState::CONNECTED);
    }

    ~SwitchFlowFixture() override {
        set_wizard_active(false);
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

    static void set_connection(helix::ConnectionState state) {
        get_printer_state().network_state().set_printer_connection_state_internal(
            static_cast<int>(state), "");
    }

    static void click(lv_obj_t* dialog, const char* name) {
        lv_obj_t* btn = lv_obj_find_by_name(dialog, name);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        UpdateQueue::instance().drain();
        UpdateQueue::instance().drain();
        lv_timer_handler();
    }

    long teardowns() const {
        return std::count(events_.begin(), events_.end(), "teardown");
    }

    helix::Config* cfg_ = nullptr;
    helix::AsyncLifetimeGuard async_;
    std::vector<std::string> events_;
    std::string active_at_teardown_;
    std::string card_at_teardown_;
    helix::MoonrakerLocality locality_at_teardown_ = helix::MoonrakerLocality::Remote;
    int flushes_at_teardown_ = 0;
    static inline int s_flushes = 0;
    std::string card_at_rebuild_;
    helix::SharedCallback<void()> on_rebuild_;
    bool rebuild_connects_ = true;
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

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: picking the active printer while disconnected connects it",
                 "[multi-printer][switch_flow]") {
    const auto state =
        GENERATE(helix::ConnectionState::DISCONNECTED, helix::ConnectionState::FAILED);
    set_connection(state);
    // A job the disconnected printer last reported does not make the pick ask.
    set_job(PrintJobState::PRINTING);

    CHECK(flow_.request_switch("alpha"));

    CHECK(Modal::get_top() == nullptr);
    CHECK(events_ == kFullRestart);
    CHECK(cfg_->get_active_printer_id() == "alpha");
    CHECK(flow_.connected_printer_id() == "alpha");
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: picking the active printer while it connects does nothing",
                 "[multi-printer][switch_flow]") {
    const auto state =
        GENERATE(helix::ConnectionState::CONNECTING, helix::ConnectionState::RECONNECTING);
    set_connection(state);

    CHECK_FALSE(flow_.request_switch("alpha"));
    CHECK(events_.empty());
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: moving to another printer records the one left as the fallback",
                 "[multi-printer][switch_flow]") {
    cfg_->set<int>(helix::kBootCrashStreakKey, 2);

    flow_.request_switch("beta");

    CHECK(cfg_->get<std::string>(helix::kSwitchPreviousPrinterKey, "") == "alpha");
    CHECK(cfg_->get<int>(helix::kBootCrashStreakKey, -1) == 0);
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: re-picking the same printer keeps its fallback and crash run",
                 "[multi-printer][switch_flow]") {
    cfg_->set<std::string>(helix::kSwitchPreviousPrinterKey, "beta");
    cfg_->set<int>(helix::kBootCrashStreakKey, 2);
    set_connection(helix::ConnectionState::DISCONNECTED);

    REQUIRE(flow_.request_switch("alpha"));

    CHECK(cfg_->get<std::string>(helix::kSwitchPreviousPrinterKey, "") == "beta");
    CHECK(cfg_->get<int>(helix::kBootCrashStreakKey, -1) == 2);
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: a switch whose save fails keeps the boot-crash record",
                 "[multi-printer][switch_flow]") {
    cfg_->set<bool>(helix::kBootConnectHoldKey, true);
    cfg_->set<std::string>(helix::kSwitchPreviousPrinterKey, "gamma");
    cfg_->set<int>(helix::kBootCrashStreakKey, 2);
    helix::ConfigTestAccess::read_only_mode(*cfg_) = true;

    CHECK_FALSE(flow_.request_switch("beta"));

    CHECK(cfg_->get_active_printer_id() == "alpha");
    CHECK(cfg_->get<bool>(helix::kBootConnectHoldKey, false));
    CHECK(cfg_->get<std::string>(helix::kSwitchPreviousPrinterKey, "") == "gamma");
    CHECK(cfg_->get<int>(helix::kBootCrashStreakKey, -1) == 2);
    CHECK(events_.empty());
}

TEST_CASE_METHOD(SwitchFlowFixture, "Switch flow: a pick clears the boot-crash connection hold",
                 "[multi-printer][switch_flow]") {
    const std::string pick = GENERATE(std::string("alpha"), std::string("beta"));
    cfg_->set<bool>(helix::kBootConnectHoldKey, true);
    set_connection(helix::ConnectionState::DISCONNECTED);

    flow_.request_switch(pick);

    CHECK_FALSE(cfg_->get<bool>(helix::kBootConnectHoldKey, true));
    CHECK(events_ == kFullRestart);
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
    CHECK(flow_.add_printer("10.0.0.9", 7125));

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

    CHECK(flow_.add_printer("10.0.0.2", 7125));

    CHECK(cfg_->get_printer_ids().size() == 2);
    CHECK(cfg_->get_active_printer_id() == "beta");
    CHECK(events_ == kFullRestart);
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: an add whose save fails keeps the entry and stays",
                 "[multi-printer][switch_flow]") {
    helix::ConfigTestAccess::read_only_mode(*cfg_) = true;

    CHECK_FALSE(flow_.add_printer("10.0.0.9", 7125));

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

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: adding a printer through the wizard records the one left",
                 "[multi-printer][switch_flow]") {
    cfg_->set<bool>(helix::kBootConnectHoldKey, true);
    cfg_->set<int>(helix::kBootCrashStreakKey, 2);

    flow_.add_printer_via_wizard();
    set_wizard_cancel_callback(nullptr);

    CHECK_FALSE(cfg_->get<bool>(helix::kBootConnectHoldKey, true));
    CHECK(cfg_->get<std::string>(helix::kSwitchPreviousPrinterKey, "") == "alpha");
    CHECK(cfg_->get<int>(helix::kBootCrashStreakKey, -1) == 0);
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: cancelling the add-printer wizard puts back the fallback and crash "
                 "run",
                 "[multi-printer][switch_flow]") {
    cfg_->set<bool>(helix::kBootConnectHoldKey, true);
    cfg_->set<std::string>(helix::kSwitchPreviousPrinterKey, "beta");
    cfg_->set<int>(helix::kBootCrashStreakKey, 2);
    flow_.add_printer_via_wizard();
    set_wizard_cancel_callback(nullptr);
    REQUIRE(cfg_->get_active_printer_id() != "alpha");

    flow_.cancel_add_printer_wizard();

    CHECK(cfg_->get_active_printer_id() == "alpha");
    CHECK_FALSE(cfg_->get<bool>(helix::kBootConnectHoldKey, true));
    CHECK(cfg_->get<std::string>(helix::kSwitchPreviousPrinterKey, "") == "beta");
    CHECK(cfg_->get<int>(helix::kBootCrashStreakKey, -1) == 2);
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: a cancel after the add-printer wizard ended restores nothing",
                 "[multi-printer][switch_flow]") {
    cfg_->set<std::string>(helix::kSwitchPreviousPrinterKey, "beta");
    cfg_->set<int>(helix::kBootCrashStreakKey, 2);
    flow_.add_printer_via_wizard();
    set_wizard_cancel_callback(nullptr);
    const std::string added = cfg_->get_active_printer_id();
    REQUIRE(added != "alpha");

    std::string expected_active;
    SECTION("the wizard completed") {
        flow_.clear_wizard_previous_printer_id();
        expected_active = added;
    }
    SECTION("a switch left the wizard") {
        REQUIRE(flow_.request_switch("beta"));
        expected_active = "beta";
    }
    const std::string previous = cfg_->get<std::string>(helix::kSwitchPreviousPrinterKey, "");
    const int streak = cfg_->get<int>(helix::kBootCrashStreakKey, -1);

    flow_.cancel_add_printer_wizard();
    lv_timer_handler();

    CHECK(cfg_->get_active_printer_id() == expected_active);
    CHECK(cfg_->get<std::string>(helix::kSwitchPreviousPrinterKey, "") == previous);
    CHECK(cfg_->get<int>(helix::kBootCrashStreakKey, -1) == streak);
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: a wizard start whose save fails keeps the boot-crash record",
                 "[multi-printer][switch_flow]") {
    cfg_->set<bool>(helix::kBootConnectHoldKey, true);
    cfg_->set<std::string>(helix::kSwitchPreviousPrinterKey, "gamma");
    cfg_->set<int>(helix::kBootCrashStreakKey, 2);
    helix::ConfigTestAccess::read_only_mode(*cfg_) = true;

    flow_.add_printer_via_wizard();

    CHECK(cfg_->get_active_printer_id() == "alpha");
    CHECK(cfg_->get_printer_ids().size() == 2);
    CHECK(cfg_->get<bool>(helix::kBootConnectHoldKey, false));
    CHECK(cfg_->get<std::string>(helix::kSwitchPreviousPrinterKey, "") == "gamma");
    CHECK(cfg_->get<int>(helix::kBootCrashStreakKey, -1) == 2);
    CHECK(flow_.wizard_previous_printer_id().empty());
    CHECK(events_.empty());
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: the switch card covers the restart until the printer connects",
                 "[multi-printer][switch_flow][switch_interstitial]") {
    REQUIRE(switch_card() == "none");

    REQUIRE(flow_.request_switch("beta"));

    CHECK(card_at_teardown_ == "Switching to Beta / Loading...");
    CHECK(card_at_rebuild_ == "Switching to Beta / Loading...");
    CHECK(switch_card() == "Switching to Beta / Connecting...");

    // The previous printer's CONNECTED, still in the subject when the wait starts, is not
    // the new printer connecting.
    UpdateQueue::instance().drain();
    CHECK(switch_card() == "Switching to Beta / Connecting...");
    set_connection(helix::ConnectionState::CONNECTING);
    UpdateQueue::instance().drain();
    CHECK(switch_card() == "Switching to Beta / Connecting...");

    set_connection(helix::ConnectionState::CONNECTED);
    UpdateQueue::instance().drain();
    CHECK(switch_card() == "none");
    CHECK(helix::PrinterSwitchFlowTestAccess::connect_timeout(flow_) == nullptr);
}

TEST_CASE_METHOD(SwitchFlowFixture, "Switch flow: a failed connection drops the switch card",
                 "[multi-printer][switch_flow][switch_interstitial]") {
    REQUIRE(flow_.request_switch("beta"));
    set_connection(helix::ConnectionState::CONNECTING);
    UpdateQueue::instance().drain();
    REQUIRE(switch_card() != "none");

    set_connection(helix::ConnectionState::FAILED);
    UpdateQueue::instance().drain();

    CHECK(switch_card() == "none");
    CHECK(helix::PrinterSwitchFlowTestAccess::connect_timeout(flow_) == nullptr);
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: the switch card steps aside when the printer never connects",
                 "[multi-printer][switch_flow][switch_interstitial]") {
    REQUIRE(flow_.request_switch("beta"));
    set_connection(helix::ConnectionState::RECONNECTING);
    UpdateQueue::instance().drain();
    lv_timer_t* timeout = helix::PrinterSwitchFlowTestAccess::connect_timeout(flow_);
    REQUIRE(timeout != nullptr);
    REQUIRE(switch_card() != "none");

    lv_timer_ready(timeout);
    lv_timer_handler();

    CHECK(switch_card() == "none");
    CHECK(helix::PrinterSwitchFlowTestAccess::connect_timeout(flow_) == nullptr);
    // Nothing the wait left behind fires on a later connection.
    set_connection(helix::ConnectionState::CONNECTED);
    UpdateQueue::instance().drain();
    CHECK(switch_card() == "none");
}

TEST_CASE_METHOD(SwitchFlowFixture, "Switch flow: repeated switches leave no switch cards behind",
                 "[multi-printer][switch_flow][switch_interstitial]") {
    lv_timer_handler();
    const uint32_t before = lv_obj_get_child_count(lv_layer_top());

    for (int i = 0; i < 3; ++i) {
        REQUIRE(flow_.request_switch("beta"));
        // A second switch while the first is still connecting replaces its card.
        REQUIRE(flow_.request_switch("alpha"));
        CHECK(switch_card() == "Switching to Alpha / Connecting...");
        set_connection(helix::ConnectionState::CONNECTING);
        UpdateQueue::instance().drain();
        set_connection(helix::ConnectionState::CONNECTED);
        UpdateQueue::instance().drain();
        CHECK(switch_card() == "none");
    }
    lv_timer_handler();
    lv_timer_handler();

    CHECK(lv_obj_get_child_count(lv_layer_top()) == before);
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: the add-printer wizard takes over from the switch card",
                 "[multi-printer][switch_flow][switch_interstitial]") {
    on_rebuild_ = [] { set_wizard_active(true); };

    flow_.add_printer_via_wizard();
    set_wizard_cancel_callback(nullptr);

    CHECK(card_at_teardown_ == "Adding printer / Loading...");
    CHECK(card_at_rebuild_ == "Adding printer / Loading...");
    CHECK(switch_card() == "none");
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: cancelling the add-printer wizard covers the restart back",
                 "[multi-printer][switch_flow][switch_interstitial]") {
    flow_.add_printer_via_wizard();
    set_wizard_cancel_callback(nullptr);
    set_connection(helix::ConnectionState::CONNECTING);
    set_connection(helix::ConnectionState::CONNECTED);
    UpdateQueue::instance().drain();
    REQUIRE(switch_card() == "none");

    flow_.cancel_add_printer_wizard();
    lv_timer_handler();

    CHECK(card_at_teardown_ == "Switching to Alpha / Loading...");
    CHECK(switch_card() == "Switching to Alpha / Connecting...");
}

TEST_CASE_METHOD(SwitchFlowFixture, "Switch flow: the switch card is painted before the teardown",
                 "[multi-printer][switch_flow][switch_interstitial]") {
    lv_display_t* display = lv_display_get_default();
    REQUIRE(display != nullptr);
    lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) {
        ++s_flushes;
        lv_display_flush_ready(d);
    });
    lv_refr_now(display);
    s_flushes = 0;

    REQUIRE(flow_.request_switch("beta"));

    // The teardown and rebuild block the thread that renders, so a card left for the next
    // timer pass would only appear once they are over.
    CHECK(card_at_teardown_ == "Switching to Beta / Loading...");
    CHECK(flushes_at_teardown_ > 0);

    lv_display_set_flush_cb(
        display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: the switch card spinner moves only on full render tiers",
                 "[multi-printer][switch_flow][switch_interstitial]") {
    lv_subject_t* tier = lv_xml_get_subject(nullptr, "platform_tier");
    static lv_subject_t s_tier;
    if (!tier) {
        lv_subject_init_int(&s_tier, static_cast<int>(helix::PlatformTier::STANDARD));
        lv_xml_register_subject(nullptr, "platform_tier", &s_tier);
        tier = &s_tier;
    }
    const int saved = lv_subject_get_int(tier);

    helix::PlatformTier set_tier = helix::PlatformTier::STANDARD;
    bool expect_motion = true;
    SECTION("standard") {}
    SECTION("basic") {
        set_tier = helix::PlatformTier::BASIC;
        expect_motion = false;
    }
    SECTION("embedded") {
        set_tier = helix::PlatformTier::EMBEDDED;
        expect_motion = false;
    }
    lv_subject_set_int(tier, static_cast<int>(set_tier));

    REQUIRE(flow_.request_switch("beta"));
    const auto cards = visible_switch_cards();
    REQUIRE(cards.size() == 1);
    lv_obj_t* spinner = lv_obj_find_by_name(cards.front(), "switch_interstitial_spinner");
    REQUIRE(spinner != nullptr);

    CHECK((lv_anim_get(spinner, nullptr) != nullptr) == expect_motion);

    lv_subject_set_int(tier, saved);
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: the previous printer's FAILED does not end the switch card",
                 "[multi-printer][switch_flow][switch_interstitial]") {
    set_connection(helix::ConnectionState::FAILED);
    UpdateQueue::instance().drain();

    REQUIRE(flow_.request_switch("beta"));
    UpdateQueue::instance().drain();
    CHECK(switch_card() == "Switching to Beta / Connecting...");

    set_connection(helix::ConnectionState::CONNECTING);
    set_connection(helix::ConnectionState::FAILED);
    UpdateQueue::instance().drain();
    CHECK(switch_card() == "none");
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: the previous printer's queued states wait for the new attempt",
                 "[multi-printer][switch_flow][switch_interstitial]") {
    // The previous printer was reconnecting when the switch began; its CONNECTED and FAILED
    // are still queued behind the attach.
    set_connection(helix::ConnectionState::RECONNECTING);
    UpdateQueue::instance().drain();
    REQUIRE(flow_.request_switch("beta"));
    UpdateQueue::instance().drain();

    set_connection(helix::ConnectionState::CONNECTED);
    UpdateQueue::instance().drain();
    CHECK(switch_card() == "Switching to Beta / Connecting...");
    set_connection(helix::ConnectionState::FAILED);
    UpdateQueue::instance().drain();
    CHECK(switch_card() == "Switching to Beta / Connecting...");

    set_connection(helix::ConnectionState::DISCONNECTED);
    UpdateQueue::instance().drain();
    CHECK(switch_card() == "Switching to Beta / Connecting...");
    set_connection(helix::ConnectionState::CONNECTING);
    set_connection(helix::ConnectionState::CONNECTED);
    UpdateQueue::instance().drain();
    CHECK(switch_card() == "none");
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: a rebuild that never starts connecting drops the switch card",
                 "[multi-printer][switch_flow][switch_interstitial]") {
    rebuild_connects_ = false;

    REQUIRE(flow_.request_switch("beta"));

    CHECK(card_at_rebuild_ == "Switching to Beta / Loading...");
    CHECK(switch_card() == "none");
    CHECK(helix::PrinterSwitchFlowTestAccess::connect_timeout(flow_) == nullptr);
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: leaving a printer drops the connection's locality verdict",
                 "[multi-printer][switch_flow]") {
    auto remote_verdict = [] {
        get_printer_state().set_moonraker_is_remote(true);
        UpdateQueue::instance().drain();
        REQUIRE(get_printer_state().moonraker_locality() == helix::MoonrakerLocality::Remote);
    };

    SECTION("switching") {
        remote_verdict();
        flow_.request_switch("beta");
    }
    SECTION("adding through the wizard") {
        remote_verdict();
        flow_.add_printer_via_wizard();
        set_wizard_cancel_callback(nullptr);
    }
    SECTION("cancelling the wizard") {
        flow_.add_printer_via_wizard();
        set_wizard_cancel_callback(nullptr);
        remote_verdict();
        flow_.cancel_add_printer_wizard();
        lv_timer_handler();
    }

    // Read at the teardown: the rebuild after it can reconnect and settle a new verdict.
    CHECK(locality_at_teardown_ == helix::MoonrakerLocality::Unknown);
}

// Panel code drains the UpdateQueue from its own methods; a teardown run by that drain frees
// the panel still on the stack under it.
TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: a switch queued into an UpdateQueue drain waits for the next tick",
                 "[multi-printer][switch_flow][lifecycle]") {
    long inside_nested_drain = -1;
    SECTION("a drain at the top") {
        helix::ui::queue_update("test::request_switch", [this] { flow_.request_switch("beta"); });
        UpdateQueue::instance().drain();
    }
    SECTION("a drain nested in a queued callback") {
        helix::ui::queue_update("test::populate_page", [this, &inside_nested_drain] {
            helix::ui::queue_update("test::request_switch",
                                    [this] { flow_.request_switch("beta"); });
            UpdateQueue::instance().drain();
            inside_nested_drain = teardowns();
        });
        UpdateQueue::instance().drain();
        CHECK(inside_nested_drain == 0);
    }

    CHECK(teardowns() == 0);
    CHECK(cfg_->get_active_printer_id() == "alpha");

    lv_timer_handler();

    CHECK(teardowns() == 1);
    CHECK(events_ == kFullRestart);
    CHECK(cfg_->get_active_printer_id() == "beta");
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: adding through the wizard from an UpdateQueue drain waits for the "
                 "next tick",
                 "[multi-printer][switch_flow][lifecycle]") {
    helix::ui::queue_update("test::add_printer_via_wizard",
                            [this] { flow_.add_printer_via_wizard(); });
    UpdateQueue::instance().drain();
    CHECK(teardowns() == 0);
    CHECK(cfg_->get_active_printer_id() == "alpha");

    lv_timer_handler();
    set_wizard_cancel_callback(nullptr);

    CHECK(teardowns() == 1);
    CHECK(cfg_->get_active_printer_id() != "alpha");
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: cancelling the add-printer wizard restarts on the next tick, not in "
                 "a drain",
                 "[multi-printer][switch_flow][lifecycle]") {
    flow_.add_printer_via_wizard();
    set_wizard_cancel_callback(nullptr);
    REQUIRE(teardowns() == 1);

    helix::ui::queue_update("test::cancel_add_printer_wizard",
                            [this] { flow_.cancel_add_printer_wizard(); });
    UpdateQueue::instance().drain();
    UpdateQueue::instance().drain();
    CHECK(teardowns() == 1);

    lv_timer_handler();

    CHECK(teardowns() == 2);
    CHECK(cfg_->get_active_printer_id() == "alpha");
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: a confirmed switch restarts on the next tick, not in a drain",
                 "[multi-printer][switch_flow][lifecycle]") {
    set_job(PrintJobState::PRINTING);
    flow_.request_switch("beta");
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    lv_obj_t* btn = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    UpdateQueue::instance().drain();
    CHECK(teardowns() == 0);

    lv_timer_handler();

    CHECK(events_ == kFullRestart);
    CHECK(cfg_->get_active_printer_id() == "beta");
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: of two picks made in one drain, the later one runs",
                 "[multi-printer][switch_flow][lifecycle]") {
    cfg_->add_printer("gamma", nlohmann::json{{"printer_name", "Gamma"}});
    helix::ui::queue_update("test::pick_beta", [this] { flow_.request_switch("beta"); });
    helix::ui::queue_update("test::pick_gamma", [this] { flow_.request_switch("gamma"); });
    UpdateQueue::instance().drain();

    lv_timer_handler();
    lv_timer_handler();

    CHECK(teardowns() == 1);
    CHECK(active_at_teardown_ == "gamma");
    CHECK(cfg_->get_active_printer_id() == "gamma");
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: a pick made in place drops the one waiting for the tick",
                 "[multi-printer][switch_flow][lifecycle]") {
    cfg_->add_printer("gamma", nlohmann::json{{"printer_name", "Gamma"}});
    helix::ui::queue_update("test::pick_beta", [this] { flow_.request_switch("beta"); });
    UpdateQueue::instance().drain();

    REQUIRE(flow_.request_switch("gamma"));
    lv_timer_handler();

    CHECK(teardowns() == 1);
    CHECK(cfg_->get_active_printer_id() == "gamma");
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: a wizard cancel drops a pick still waiting for the tick",
                 "[multi-printer][switch_flow][lifecycle]") {
    flow_.add_printer_via_wizard();
    set_wizard_cancel_callback(nullptr);
    const std::string added = cfg_->get_active_printer_id();
    REQUIRE(teardowns() == 1);

    helix::ui::queue_update("test::pick_beta", [this] { flow_.request_switch("beta"); });
    UpdateQueue::instance().drain();
    flow_.cancel_add_printer_wizard();
    lv_timer_handler();

    CHECK(teardowns() == 2);
    CHECK(cfg_->get_active_printer_id() == "alpha");
    const auto ids = cfg_->get_printer_ids();
    CHECK(std::find(ids.begin(), ids.end(), added) == ids.end());
}

TEST_CASE_METHOD(SwitchFlowFixture,
                 "Switch flow: a pick made after a wizard cancel runs after the cancel's restart",
                 "[multi-printer][switch_flow][lifecycle]") {
    flow_.add_printer_via_wizard();
    set_wizard_cancel_callback(nullptr);
    REQUIRE(teardowns() == 1);

    flow_.cancel_add_printer_wizard();
    std::vector<std::string> active_at_each_teardown;
    on_rebuild_ = [&] { active_at_each_teardown.push_back(active_at_teardown_); };
    helix::ui::queue_update("test::pick_beta", [this] { flow_.request_switch("beta"); });
    UpdateQueue::instance().drain();
    lv_timer_handler();

    CHECK(active_at_each_teardown == std::vector<std::string>{"alpha", "beta"});
    CHECK(cfg_->get_active_printer_id() == "beta");
}
