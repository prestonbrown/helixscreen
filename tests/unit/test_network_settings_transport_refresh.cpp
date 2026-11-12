// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_network_settings_transport_refresh.cpp
 * @brief #1398 — an open overlay refreshes its transport rows when the
 *        backend flips state underneath it.
 *
 * The overlay re-queried wifi/ethernet status only from its own action
 * callbacks. A backend-initiated transport flip — on netd's single-transport
 * platforms a join or leave downs/ups the OTHER transport too — therefore
 * left both rows stale until the user tapped something. on_activate()
 * subscribes to WiFiManager state changes and calls refresh_transport_status();
 * this pins that wiring by firing a CONNECTED event the overlay did not cause
 * and asserting its subjects moved with no action callback involved.
 */

#include "ui_nav_manager.h"
#include "ui_overlay_ipv4_settings.h"
#include "ui_overlay_network_settings.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/network_settings_overlay_test_access.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "../ui_test_utils.h"
#include "core/update_queue.h"
#include "ethernet_backend_mock.h"
#include "ipv4_config_system.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using Access = NetworkSettingsOverlayTestAccess;

namespace {

// LVGLUITestFixture: the overlay is created from XML, so assets, theme,
// widgets, and components must all be registered exactly as production does.
class TransportRefreshFixture : public LVGLUITestFixture {
  protected:
    ScopedRuntimeConfig scoped_config;

    /// A locally owned manager in test mode (idle mock backend, no
    /// wpa_supplicant probing), same shape as the observer-notification
    /// fixture.
    std::shared_ptr<helix::WiFiManager> make_manager() {
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_wifi = false;
        auto wm = std::make_shared<helix::WiFiManager>(/*silent=*/true);
        wm->init_self_reference(wm);
        return wm;
    }
};

} // namespace

TEST_CASE_METHOD(TransportRefreshFixture,
                 "NetworkSettingsOverlay refreshes transport rows on a backend state change",
                 "[network_settings][1398][observers]") {
    auto wm = make_manager();
    auto overlay = std::make_unique<NetworkSettingsOverlay>();
    overlay->init_subjects();
    overlay->register_callbacks();
    // Point the overlay at OUR manager before create(): create() would
    // otherwise grab the process-global singleton, and a local manager keeps
    // this test independent of every other wifi test's state.
    Access::wifi_manager(*overlay) = wm;
    REQUIRE(overlay->create(test_screen()) != nullptr);

    // The overlay opens — on_activate() is where the transport refresh
    // subscribes (not create(): on_deactivate() invalidates the token).
    overlay->on_activate();
    REQUIRE(lv_subject_get_int(&Access::wifi_connected(*overlay)) == 0);
    lv_obj_t* ipv4_row = lv_obj_find_by_name(test_screen(), "wifi_ipv4_row");
    REQUIRE(ipv4_row != nullptr);
    CHECK(lv_obj_has_flag(ipv4_row, LV_OBJ_FLAG_HIDDEN));
    // The IPv4 row reads the setting on open, and again on each transport change.
    const auto ipv4_row_reads = [this](const helix::net::Ipv4Config& config) {
        return wait_until([&] {
            helix::ui::UpdateQueue::instance().drain();
            return lv_subject_get_string(lv_xml_get_subject(nullptr, "wifi_ipv4_summary")) ==
                   helix::settings::ipv4_row_summary(config);
        });
    };
    CHECK(ipv4_row_reads(helix::net::Ipv4Config{}));
    helix::net::Ipv4Config fixed;
    fixed.dhcp = false;
    fixed.address = "192.168.1.50";
    fixed.prefix = 24;
    fixed.gateway = "192.168.1.1";
    REQUIRE(helix::net::mock_ipv4_store(helix::net::Ipv4Link::Wifi).apply(fixed).ok());

    // A backend-initiated flip: no overlay action callback runs, only the
    // state observer -> refresh_transport_status() -> update_wifi_status().
    helix::WiFiManagerTestAccess::fire_connected(*wm, "");
    helix::ui::UpdateQueue::instance().drain();

    // Convergence wait, not a single drain: the refresh chain's ethernet half
    // resolves on an HttpExecutor worker and can re-queue under load, and the
    // suite's 96-way shard burst makes one-shot drains racy (tests/CLAUDE.md
    // "join the worker, then drain" - there is no joinable handle here).
    REQUIRE(wait_until([&] {
        helix::ui::UpdateQueue::instance().drain();
        return lv_subject_get_int(&Access::wifi_connected(*overlay)) == 1;
    }));
    CHECK(ipv4_row_reads(fixed));
    helix::net::mock_ipv4_store(helix::net::Ipv4Link::Wifi).apply(helix::net::Ipv4Config{});

    CHECK_FALSE(lv_obj_has_flag(ipv4_row, LV_OBJ_FLAG_HIDDEN));

    // On DHCP the form opens with the address the printer has now.
    helix::settings::open_ipv4_settings(test_screen(), helix::net::Ipv4Link::Wifi, "192.168.1.77");
    CHECK(wait_until([&] {
        helix::ui::UpdateQueue::instance().drain();
        return std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "ipv4_address"))) ==
               "192.168.1.77";
    }));
    NavigationManager::instance().go_back();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(TransportRefreshFixture,
                 "NetworkSettingsOverlay clears its network list without running queued work",
                 "[network_settings][drain_reentrancy]") {
    auto wm = make_manager();
    auto overlay = std::make_unique<NetworkSettingsOverlay>();
    overlay->init_subjects();
    overlay->register_callbacks();
    Access::wifi_manager(*overlay) = wm;
    REQUIRE(overlay->create(test_screen()) != nullptr);

    bool ran = false;
    helix::ui::UpdateQueue::instance().queue("test::drain_canary", [&ran]() { ran = true; });
    Access::clear_network_list(*overlay);
    CHECK_FALSE(ran);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(ran);
}

TEST_CASE_METHOD(TransportRefreshFixture,
                 "NetworkSettingsOverlay keeps one state observer across screensaver wakes",
                 "[network_settings][observers][suspend]") {
    auto wm = make_manager();
    auto overlay = std::make_unique<NetworkSettingsOverlay>();
    overlay->init_subjects();
    overlay->register_callbacks();
    Access::wifi_manager(*overlay) = wm;
    REQUIRE(overlay->create(test_screen()) != nullptr);
    overlay->on_activate();
    const size_t registered = helix::WiFiManagerTestAccess::state_observer_count(*wm);
    REQUIRE(registered > 0);

    // A wake keeps the observers the opening registered; adding more would run
    // every refresh once per screensaver cycle.
    for (int i = 0; i < 2; ++i) {
        overlay->on_deactivate(DeactivateReason::Suspended);
        overlay->on_activate();
    }
    CHECK(helix::WiFiManagerTestAccess::state_observer_count(*wm) == registered);
}

TEST_CASE_METHOD(TransportRefreshFixture,
                 "NetworkSettingsOverlay follows an Ethernet cable change while open",
                 "[network_settings][1976][observers]") {
    auto wm = make_manager();
    auto overlay = std::make_unique<NetworkSettingsOverlay>();
    overlay->init_subjects();
    overlay->register_callbacks();
    Access::wifi_manager(*overlay) = wm;
    auto eth = std::make_shared<EthernetBackendMock>();
    Access::ethernet_manager(*overlay) = std::make_unique<EthernetManager>(eth);
    // Releases the held probe however the case ends: a held worker would starve the fast lane.
    std::shared_ptr<void> release_held(nullptr, [eth](void*) { eth->release_held_info(); });
    const auto probes_idle = [&] {
        helix::ui::UpdateQueue::instance().drain();
        return Access::eth_refresh_idle(*overlay);
    };
    eth->hold_next_info();
    REQUIRE(overlay->create(test_screen()) != nullptr);
    overlay->on_activate();

    // on_activate() waits behind the probe create() left out: two probes out
    // together can answer in either order, and a stale "connected" landing
    // last would hide the pulled cable below.
    CHECK_FALSE(wait_until(probes_idle, 200));

    // Closed while that probe is out, its answer is dropped, so reopening
    // must not wait for it.
    overlay->on_deactivate(DeactivateReason::NavigateAway);
    helix::ui::UpdateQueue::instance().drain();
    overlay->on_activate();
    REQUIRE(wait_until([&] {
        return probes_idle() && lv_subject_get_int(&Access::eth_connected(*overlay)) == 1;
    }));
    eth->release_held_info();

    // The cable comes out; only the manager's link watch sees it.
    eth->set_connected_state(false);
    Access::ethernet_manager(*overlay)->check_link();
    REQUIRE(wait_until([&] {
        helix::ui::UpdateQueue::instance().drain();
        return lv_subject_get_int(&Access::eth_connected(*overlay)) == 0;
    }));
}
