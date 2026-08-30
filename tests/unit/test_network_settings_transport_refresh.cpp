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

#include "ui_overlay_network_settings.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/network_settings_overlay_test_access.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../ui_test_utils.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using Access = NetworkSettingsOverlayTestAccess;

namespace helix {
// Friend accessor, same shape as test_wifi_observer_notification.cpp: drives
// the manager's private connection handler so the event fires from the test
// thread deterministically.
class WiFiManagerTestAccess {
  public:
    static void fire_connected(WiFiManager& wm, const std::string& data = "") {
        // Match production ordering: the backend updates its state BEFORE
        // firing CONNECTED, so is_connected() is already true when the state
        // observer's refresh runs (prestonbrown/helixscreen#1059).
        if (auto* mock = dynamic_cast<WifiBackendMock*>(wm.backend_.get())) {
            mock->set_connected_state(true, "Studio 5G", "10.0.0.4", 70);
        }
        wm.handle_connected(data);
    }
};
} // namespace helix

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

    // A backend-initiated flip: no overlay action callback runs, only the
    // state observer -> refresh_transport_status() -> update_wifi_status().
    helix::WiFiManagerTestAccess::fire_connected(*wm, "");
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(lv_subject_get_int(&Access::wifi_connected(*overlay)) == 1);
}
