// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_list_add_printer.cpp
 * @brief Add Printer and a picked printer open what they open after the Printers list closes,
 *        and a tap on the active printer reaches the switch only while it is disconnected.
 *
 * Closing the list pops an overlay, and the pop hides every stray child of the screen. What
 * the add or switch callback puts on screen must arrive after that sweep, or it is hidden as
 * it opens.
 */

#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_printer_list_overlay.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "config.h"
#include "connection_state.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "lvgl/lvgl.h"
#include "printer_cache_registry.h"
#include "printer_state.h"
#include "printer_switch_flow.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

class AddPrinterFixture : public LVGLUITestFixture {
  public:
    AddPrinterFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        auto& nav = NavigationManager::instance();
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = lv_obj_create(test_screen());
        panels[static_cast<int>(PanelId::Settings)] = lv_obj_create(test_screen());
        lv_obj_add_flag(panels[static_cast<int>(PanelId::Settings)], LV_OBJ_FLAG_HIDDEN);
        nav.set_panels(panels);

        list_overlay_ = lv_obj_create(test_screen());
        lv_obj_add_flag(list_overlay_, LV_OBJ_FLAG_HIDDEN);
        nav.register_overlay_instance(list_overlay_, nullptr);
        nav.push_overlay(list_overlay_);
        drain();
    }

    ~AddPrinterFixture() override {
        auto& nav = NavigationManager::instance();
        nav.set_printer_callbacks(nullptr, nullptr);
        nav.unregister_overlay_instance(list_overlay_);
        drain();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    /// Runs queued work, then the next tick, where a switch the queue requested runs.
    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        lv_timer_handler();
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    lv_obj_t* list_overlay_ = nullptr;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(AddPrinterFixture, "Printers list: Add Printer opens after the list closes",
                 "[multi-printer][navigation]") {
    lv_obj_t* opened = nullptr;
    NavigationManager::instance().set_printer_callbacks(
        [](const std::string&) {}, [&] { opened = lv_obj_create(test_screen()); });

    helix::ui::get_printer_list_overlay().handle_add_printer();
    drain();

    REQUIRE(opened != nullptr);
    CHECK_FALSE(lv_obj_has_flag(opened, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(AddPrinterFixture,
                 "Printers list: a tap on the active printer switches only while disconnected",
                 "[multi-printer][navigation]") {
    get_printer_state().init_subjects(false);
    const std::string active = Config::get_instance()->get_active_printer_id();
    std::vector<std::string> picked;
    NavigationManager::instance().set_printer_callbacks(
        [&](const std::string& id) { picked.push_back(id); }, [] {});

    const auto state = GENERATE(ConnectionState::CONNECTED, ConnectionState::DISCONNECTED);
    get_printer_state().network_state().set_printer_connection_state_internal(
        static_cast<int>(state), "");

    helix::ui::get_printer_list_overlay().handle_switch_printer(active);
    drain();

    if (state == ConnectionState::CONNECTED) {
        CHECK(picked.empty());
    } else {
        CHECK(picked == std::vector<std::string>{active});
    }
}

TEST_CASE_METHOD(AddPrinterFixture, "Navigation: closing an overlay leaves an open modal visible",
                 "[multi-printer][navigation]") {
    lv_obj_t* dialog =
        helix::ui::modal_confirm("Title", "Message", ModalSeverity::Info, "OK", [] {});
    REQUIRE(dialog != nullptr);
    NavigationManager::instance().go_back();
    drain();

    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);
    CHECK_FALSE(lv_obj_has_flag(backdrop, LV_OBJ_FLAG_HIDDEN));
    Modal::hide(dialog);
    drain();
}

TEST_CASE_METHOD(AddPrinterFixture, "Navigation: a panel switch leaves an open modal visible",
                 "[multi-printer][navigation]") {
    lv_obj_t* dialog =
        helix::ui::modal_confirm("Title", "Message", ModalSeverity::Info, "OK", [] {});
    REQUIRE(dialog != nullptr);
    // The navbar-tap path: request_panel() runs switch_to_panel_impl().
    REQUIRE(NavigationManager::instance().request_panel(
                PanelId::Settings, NavigationManager::SwitchDispatch::Queued) ==
            NavigationManager::PanelRequest::Switched);
    drain();

    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);
    CHECK_FALSE(lv_obj_has_flag(backdrop, LV_OBJ_FLAG_HIDDEN));
    Modal::hide(dialog);
    drain();
}

namespace {

/// The Printers list wired to a real PrinterSwitchFlow, with the connected printer printing.
class PickerSwitchFixture : public AddPrinterFixture {
  public:
    PickerSwitchFixture()
        : flow_(cfg_, async_,
                {[this] { events_.push_back("teardown"); },
                 [this] {
                     events_.push_back("rebuild");
                     return true;
                 },
                 [this] { events_.push_back("home"); }}) {
        cfg_ = Config::get_instance();
        saved_data_ = ConfigTestAccess::data(*cfg_);
        saved_active_ = ConfigTestAccess::active_printer_id(*cfg_);
        saved_read_only_ = ConfigTestAccess::read_only_mode(*cfg_);
        nlohmann::json data;
        data["config_version"] = 3;
        data["active_printer_id"] = "alpha";
        data["printers"]["alpha"]["printer_name"] = "Alpha";
        data["printers"]["beta"]["printer_name"] = "Beta";
        ConfigTestAccess::data(*cfg_) = data;
        ConfigTestAccess::active_printer_id(*cfg_) = "alpha";
        ConfigTestAccess::read_only_mode(*cfg_) = false;
        PrinterCacheRegistry::instance().clear();
        flow_.set_connected_printer_id("alpha");

        NavigationManager::instance().set_printer_callbacks(
            [this](const std::string& id) { flow_.request_switch(id); }, nullptr);
        helix::test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
        drain();
    }

    ~PickerSwitchFixture() override {
        while (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
            drain();
        }
        helix::test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
        drain();
        ConfigTestAccess::data(*cfg_) = saved_data_;
        ConfigTestAccess::active_printer_id(*cfg_) = saved_active_;
        ConfigTestAccess::read_only_mode(*cfg_) = saved_read_only_;
    }

    /// Picks `id` in the list and returns the confirmation, asserting it is on screen.
    lv_obj_t* pick_and_expect_confirm(const char* id) {
        helix::ui::get_printer_list_overlay().handle_switch_printer(id);
        drain();
        lv_obj_t* dialog = Modal::get_top();
        REQUIRE(dialog != nullptr);
        lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
        REQUIRE(backdrop != nullptr);
        CHECK_FALSE(lv_obj_has_flag(backdrop, LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_flag(dialog, LV_OBJ_FLAG_HIDDEN));
        return dialog;
    }

    static void click(lv_obj_t* dialog, const char* name) {
        lv_obj_t* btn = lv_obj_find_by_name(dialog, name);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        drain();
    }

    Config* cfg_ = nullptr;
    AsyncLifetimeGuard async_;
    std::vector<std::string> events_;
    PrinterSwitchFlow flow_;

  private:
    nlohmann::json saved_data_;
    std::string saved_active_;
    bool saved_read_only_ = false;
};

const std::vector<std::string> kFullRestart = {"teardown", "rebuild", "home"};

} // namespace

TEST_CASE_METHOD(PickerSwitchFixture,
                 "Printers list: picking a printer while printing shows the confirmation",
                 "[multi-printer][navigation][switch_flow]") {
    lv_obj_t* dialog = pick_and_expect_confirm("beta");
    CHECK(events_.empty());

    click(dialog, "btn_primary");

    CHECK(events_ == kFullRestart);
    CHECK(cfg_->get_active_printer_id() == "beta");
}

TEST_CASE_METHOD(PickerSwitchFixture,
                 "Printers list: a cancelled or dismissed confirmation lets the next pick ask",
                 "[multi-printer][navigation][switch_flow]") {
    const bool cancel = GENERATE(true, false);
    lv_obj_t* dialog = pick_and_expect_confirm("beta");

    if (cancel) {
        click(dialog, "btn_secondary");
    } else {
        Modal::hide(dialog, ModalCloseReason::BackdropTap);
        drain();
    }
    REQUIRE(Modal::get_top() == nullptr);
    CHECK(events_.empty());

    // The list is gone; the badge's switch path reaches the same flow.
    NavigationManager::instance().trigger_printer_switch("beta");
    drain();
    REQUIRE(Modal::get_top() != nullptr);
    click(Modal::get_top(), "btn_primary");
    CHECK(events_ == kFullRestart);
}

TEST_CASE_METHOD(
    PickerSwitchFixture,
    "Switch flow: a confirmation hidden without a dismissal does not block the next pick",
    "[multi-printer][switch_flow]") {
    lv_obj_t* dialog = pick_and_expect_confirm("beta");
    lv_obj_add_flag(ModalStack::instance().backdrop_for(dialog), LV_OBJ_FLAG_HIDDEN);

    flow_.request_switch("beta");
    drain();

    lv_obj_t* fresh = Modal::get_top();
    REQUIRE(fresh != nullptr);
    CHECK(fresh != dialog);
    CHECK(ModalStack::instance().backdrop_for(dialog) == nullptr);
}

TEST_CASE_METHOD(PickerSwitchFixture,
                 "Switch flow: a hidden confirmation is closed, not left on the modal stack",
                 "[multi-printer][switch_flow]") {
    lv_obj_t* dialog = pick_and_expect_confirm("beta");
    lv_obj_add_flag(ModalStack::instance().backdrop_for(dialog), LV_OBJ_FLAG_HIDDEN);
    helix::test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
    drain();

    // Idle now, so the pick switches without asking and opens nothing new.
    flow_.request_switch("beta");
    drain();

    CHECK(ModalStack::instance().empty());
    CHECK(events_ == kFullRestart);
}

TEST_CASE_METHOD(PickerSwitchFixture,
                 "Switch flow: a late dismissal of one confirmation does not unblock the next",
                 "[multi-printer][switch_flow]") {
    lv_obj_t* first = pick_and_expect_confirm("beta");

    // Dismissed and picked again before the dismissal's deferred work runs.
    Modal::hide(first, ModalCloseReason::BackdropTap);
    flow_.request_switch("beta");
    lv_obj_t* second = Modal::get_top();
    REQUIRE(second != nullptr);
    REQUIRE(second != first);
    drain();

    flow_.request_switch("beta");
    drain();
    CHECK(Modal::get_top() == second);
    CHECK(events_.empty());
}
