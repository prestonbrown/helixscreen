// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The wizard's WiFi step reads the connection through
// WiFiManager::get_status_async(): on wpa_supplicant a status read is a
// control-socket round trip that can wait 10s, so no path may make one on the
// UI thread.

#include "ui_wizard_wifi.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/wizard_wifi_test_access.h"
#include "async_lifetime_guard.h"
#include "core/update_queue.h"
#include "http_executor.h"
#include "runtime_config.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using Access = WizardWifiStepTestAccess;

namespace {

constexpr const char* kSsid = "HomeNet";
constexpr const char* kIp = "192.168.1.77";
constexpr const char* kMockMac = "MAC: de:ad:be:ef:ca:fe"; // WifiBackendMock::get_status

bool names(const std::string& text, const std::string& ssid) {
    return text.find(ssid) != std::string::npos;
}

class WizardWifiStatusFixture : public LVGLUITestFixture {
  protected:
    ScopedRuntimeConfig scoped_config;
    WifiBackendMock* wifi = nullptr;

    WizardWifiStatusFixture() {
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_wifi = false;
        helix::http::HttpExecutor::fast().start();

        step_.init_subjects();
        step_.register_callbacks();
        REQUIRE(step_.create(test_screen()) != nullptr);

        auto backend = std::make_unique<WifiBackendMock>();
        REQUIRE(backend->start().success());
        wifi = backend.get();
        manager_ = std::make_shared<helix::WiFiManager>(std::move(backend), /*silent=*/true);
        manager_->init_self_reference(manager_);
        Access::wifi_manager(step_) = manager_;

        wifi->set_connected_state(true, kSsid, kIp, 70);
        wifi->clear_status_callers();
    }

    ~WizardWifiStatusFixture() override {
        wifi->release_held_status();
    }

    WizardWifiStep& step() {
        return step_;
    }

    bool no_read_on_this_thread() {
        const auto callers = wifi->status_callers();
        REQUIRE_FALSE(callers.empty());
        for (const auto& id : callers) {
            if (id == std::this_thread::get_id()) {
                return false;
            }
        }
        return true;
    }

    /// Answers are delivered in the order they were read, so once a probe
    /// issued now is answered, every earlier answer has been applied.
    void await_earlier_answers() {
        auto delivered = std::make_shared<bool>(false);
        manager_->get_status_async(
            probe_guard_.token(),
            [delivered](const WifiBackend::ConnectionStatus&) { *delivered = true; });
        REQUIRE(wait_until([&]() { return *delivered; }));
    }

    /// Stands in for the XML dialog: the widgets the connect handler looks up.
    lv_obj_t* make_modal() {
        lv_obj_t* modal = lv_obj_create(test_screen());
        REQUIRE(modal != nullptr);
        lv_obj_t* input = lv_textarea_create(modal);
        lv_obj_set_name(input, "password_input");
        lv_textarea_set_text(input, "some-password");
        lv_obj_t* status = lv_label_create(modal);
        lv_obj_set_name(status, "modal_status");
        return modal;
    }

    /// A network the mock stocks, so its join stays in flight for the mock's
    /// 2-3s connect delay instead of being refused on the spot.
    std::string stocked_ssid() {
        std::vector<WiFiNetwork> networks;
        REQUIRE(wifi->get_scan_results(networks).success());
        REQUIRE_FALSE(networks.empty());
        const auto strongest =
            std::max_element(networks.begin(), networks.end(), [](const auto& a, const auto& b) {
                return a.signal_strength < b.signal_strength;
            });
        return strongest->ssid;
    }

    bool shows_connection() {
        return names(Access::status(step_), kSsid) && Access::ip(step_) == kIp &&
               Access::mac(step_) == kMockMac;
    }

  private:
    WizardWifiStep step_;
    std::shared_ptr<helix::WiFiManager> manager_;
    helix::AsyncLifetimeGuard probe_guard_;
};

} // namespace

TEST_CASE_METHOD(WizardWifiStatusFixture,
                 "wizard wifi: applying backend state reads the connection off the UI thread",
                 "[wizard_wifi][wifi_status_async]") {
    Access::apply_backend_state(step());
    REQUIRE(wait_until([&]() { return shows_connection(); }));
    CHECK(no_read_on_this_thread());
}

TEST_CASE_METHOD(WizardWifiStatusFixture,
                 "wizard wifi: the network list marks the connected network from an async read",
                 "[wizard_wifi][wifi_status_async]") {
    Access::refresh_list(step(), {WiFiNetwork("Other", 90, true), WiFiNetwork(kSsid, 70, true)});

    auto connected_rows = [&]() {
        lv_obj_t* list = Access::network_list(step());
        int n = 0;
        for (uint32_t i = 0; list && i < lv_obj_get_child_count(list); ++i) {
            if (lv_obj_has_state(lv_obj_get_child(list, i), LV_STATE_CHECKED)) {
                n++;
            }
        }
        return n;
    };
    REQUIRE(wait_until([&]() { return connected_rows() == 1 && shows_connection(); }));
    CHECK(no_read_on_this_thread());
}

TEST_CASE_METHOD(WizardWifiStatusFixture,
                 "wizard wifi: a successful join shows its address from an async read",
                 "[wizard_wifi][wifi_status_async]") {
    Access::set_current_ssid(step(), kSsid);
    Access::announce_connected(step());
    REQUIRE(wait_until([&]() { return shows_connection(); }));
    CHECK(no_read_on_this_thread());
}

TEST_CASE_METHOD(WizardWifiStatusFixture,
                 "wizard wifi: a status answer landing after a join starts keeps the join's status",
                 "[wizard_wifi][wifi_status_async]") {
    // The apply's read takes "connected to HomeNet", then parks.
    wifi->hold_next_status();
    Access::apply_backend_state(step());
    REQUIRE(wait_until([&]() { return !wifi->status_callers().empty(); }));

    const std::string joining = stocked_ssid();
    REQUIRE(joining != kSsid);
    Access::set_current_ssid(step(), joining);
    Access::password_modal(step()) = make_modal();
    Access::password_connect_clicked(step());
    const std::string connecting = Access::status(step());
    REQUIRE(names(connecting, joining));

    wifi->release_held_status();
    await_earlier_answers();
    CHECK(Access::status(step()) == connecting);
}

TEST_CASE_METHOD(WizardWifiStatusFixture,
                 "wizard wifi: clearing the network list runs no queued callback",
                 "[wizard_wifi][drain_reentrancy]") {
    REQUIRE(Access::network_list(step()) != nullptr);
    bool ran = false;
    helix::ui::UpdateQueue::instance().queue("test::drain_canary", [&ran]() { ran = true; });
    Access::clear_list(step());
    CHECK_FALSE(ran);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(ran);
}
