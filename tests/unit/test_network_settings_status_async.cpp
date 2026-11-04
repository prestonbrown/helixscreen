// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The Network settings overlay reads the connection through
// WiFiManager::get_status_async(): on wpa_supplicant a status read is a
// control-socket round trip that can wait 10s, so no path may make one on the
// UI thread, and an answer the user has since overtaken must not land.

#include "ui_modal.h"
#include "ui_overlay_network_settings.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/network_settings_overlay_test_access.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "../ui_test_utils.h"
#include "async_lifetime_guard.h"
#include "core/http_executor.h"
#include "runtime_config.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using Access = NetworkSettingsOverlayTestAccess;

namespace {

constexpr const char* kSsid = "HomeNet";
constexpr const char* kIp = "192.168.1.77";
constexpr const char* kMockMac = "de:ad:be:ef:ca:fe"; // WifiBackendMock::get_status

class OverlayStatusFixture : public LVGLUITestFixture {
  protected:
    ScopedRuntimeConfig scoped_config;
    WifiBackendMock* wifi = nullptr;

    OverlayStatusFixture() {
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_wifi = false;
        helix::http::HttpExecutor::fast().start();

        auto backend = std::make_unique<WifiBackendMock>();
        REQUIRE(backend->start().success());
        wifi = backend.get();
        manager_ = std::make_shared<helix::WiFiManager>(std::move(backend), /*silent=*/true);
        manager_->init_self_reference(manager_);

        overlay_ = std::make_unique<NetworkSettingsOverlay>();
        overlay_->init_subjects();
        overlay_->register_callbacks();
        Access::wifi_manager(*overlay_) = manager_;
        REQUIRE(overlay_->create(test_screen()) != nullptr);
        // create() issues a status read of its own; settle it first.
        await_earlier_answers();

        wifi->set_connected_state(true, kSsid, kIp, 70);
        wifi->clear_status_callers();
    }

    ~OverlayStatusFixture() override {
        wifi->release_held_status();
        Access::password_modal(*overlay_) = nullptr;
    }

    NetworkSettingsOverlay& overlay() {
        return *overlay_;
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

    bool shows_connection() {
        return lv_subject_get_int(&Access::wifi_connected(*overlay_)) == 1 &&
               Access::ssid(*overlay_) == kSsid && Access::ip(*overlay_) == kIp &&
               Access::mac(*overlay_) == kMockMac;
    }

    /// A network the mock stocks, so its join stays in flight for the mock's
    /// 2-3s connect delay instead of being refused on the spot.
    std::string stocked_ssid() {
        std::vector<WiFiNetwork> networks;
        REQUIRE(wifi->get_scan_results(networks).success());
        REQUIRE_FALSE(networks.empty());
        return std::max_element(networks.begin(), networks.end(),
                                [](const auto& a, const auto& b) {
                                    return a.signal_strength < b.signal_strength;
                                })
            ->ssid;
    }

    lv_obj_t* make_password_modal() {
        lv_obj_t* modal = lv_obj_create(lv_screen_active());
        REQUIRE(modal != nullptr);
        lv_obj_t* input = lv_textarea_create(modal);
        lv_obj_set_name(input, "password_input");
        lv_textarea_set_text(input, "some-password");
        lv_obj_t* status = lv_label_create(modal);
        lv_obj_set_name(status, "modal_status");
        return modal;
    }

    helix::WiFiManager& manager() {
        return *manager_;
    }

  private:
    std::shared_ptr<helix::WiFiManager> manager_;
    std::unique_ptr<NetworkSettingsOverlay> overlay_;
    helix::AsyncLifetimeGuard probe_guard_;
};

} // namespace

TEST_CASE_METHOD(OverlayStatusFixture,
                 "network settings: the WiFi status comes from an async read off the UI thread",
                 "[network_settings][wifi_status_async]") {
    Access::update_wifi_status(overlay());
    REQUIRE(wait_until([&]() { return shows_connection(); }));
    CHECK(no_read_on_this_thread());
}

TEST_CASE_METHOD(OverlayStatusFixture,
                 "network settings: the list marks the connected row from an async read",
                 "[network_settings][wifi_status_async]") {
    Access::scan_completed(overlay(),
                           {WiFiNetwork("Other", 90, true), WiFiNetwork(kSsid, 70, true)});

    auto connected_rows = [&]() {
        lv_obj_t* list = Access::networks_list(overlay());
        int n = 0;
        for (uint32_t i = 0; list && i < lv_obj_get_child_count(list); ++i) {
            if (lv_obj_has_state(lv_obj_get_child(list, i), LV_STATE_CHECKED)) {
                n++;
            }
        }
        return n;
    };
    REQUIRE(wait_until([&]() { return connected_rows() == 1; }));
    CHECK(no_read_on_this_thread());
}

TEST_CASE_METHOD(OverlayStatusFixture,
                 "network settings: Forget asks the backend off the UI thread",
                 "[network_settings][wifi_status_async]") {
    Access::forget_clicked(overlay());
    REQUIRE(wait_until([&]() { return Access::pending_forget_ssid(overlay()) == kSsid; }));
    CHECK(no_read_on_this_thread());
}

TEST_CASE_METHOD(OverlayStatusFixture,
                 "network settings: a status answer landing after a join starts is dropped",
                 "[network_settings][wifi_status_async]") {
    // The display shows no connection; a read then takes "connected to
    // HomeNet" and parks.
    REQUIRE(lv_subject_get_int(&Access::wifi_connected(overlay())) == 0);
    wifi->hold_next_status();
    Access::update_wifi_status(overlay());
    REQUIRE(wait_until([&]() { return !wifi->status_callers().empty(); }));

    Access::password_modal(overlay()) = make_password_modal();
    Access::set_current_ssid(overlay(), stocked_ssid());
    Access::password_connect_clicked(overlay());

    wifi->release_held_status();
    await_earlier_answers();
    CHECK(lv_subject_get_int(&Access::wifi_connected(overlay())) == 0);
    CHECK(Access::ssid(overlay()).empty());
}

TEST_CASE_METHOD(OverlayStatusFixture,
                 "network settings: Forget tapped twice during its read opens one dialog",
                 "[network_settings][wifi_status_async]") {
    wifi->hold_next_status();
    Access::forget_clicked(overlay());
    REQUIRE(wait_until([&]() { return !wifi->status_callers().empty(); }));
    Access::forget_clicked(overlay());

    wifi->release_held_status();
    await_earlier_answers();
    REQUIRE(Access::pending_forget_ssid(overlay()) == kSsid);

    // Closing the top dialog must leave none: a second tap's dialog would sit
    // beneath it.
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    Modal::hide(dialog);
    CHECK(ModalStack::instance().empty());
}

TEST_CASE_METHOD(OverlayStatusFixture, "network settings: a failed join reads the status again",
                 "[network_settings][wifi_status_async]") {
    // The join's start drops any answer in flight, so the failure has to ask
    // again or the header keeps whatever it showed before.
    REQUIRE(lv_subject_get_int(&Access::wifi_connected(overlay())) == 0);
    Access::password_modal(overlay()) = make_password_modal();
    Access::set_current_ssid(overlay(), "NoSuchNetwork"); // the mock refuses it at once
    Access::password_connect_clicked(overlay());

    REQUIRE(wait_until([&]() { return shows_connection(); }));
    CHECK(no_read_on_this_thread());
}

TEST_CASE_METHOD(OverlayStatusFixture,
                 "network settings: Forget behind a stalled lane says WiFi isn't responding",
                 "[network_settings][wifi_status_async][wifi_control_lane]") {
    // A CONNECTED event leaves the cache connected with no network name, and a
    // stalled lane answers polls from that cache.
    helix::WiFiManagerTestAccess::set_breaker_config(manager(), {50, 3, 400, 2000});
    helix::WiFiManagerTestAccess::fire_connected(manager());
    wifi->hold_next_status();
    helix::AsyncLifetimeGuard parked;
    manager().get_status_async(parked.token(), [](const WifiBackend::ConnectionStatus&) {});
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    auto toasts = std::make_shared<std::vector<std::string>>();
    helix::ui::set_test_notification_event_hook(
        [toasts](const helix::ui::NotificationEvent& e) { toasts->push_back(e.message); });
    Access::forget_clicked(overlay());
    const bool toasted = wait_until([&]() { return !toasts->empty(); }, 2000);
    helix::ui::set_test_notification_event_hook(nullptr);

    REQUIRE(toasted);
    CHECK(toasts->front() == "WiFi isn't responding");
    CHECK(Access::pending_forget_ssid(overlay()).empty());
}
