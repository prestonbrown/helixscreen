// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The home network tile reads WiFi through WiFiManager::get_status_async(): on
// wpa_supplicant a status read is a control-socket round trip that can wait
// 10s, so neither detection nor the signal poll may make one on the UI thread.

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/network_widget_test_access.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "async_lifetime_guard.h"
#include "core/http_executor.h"
#include "ethernet_backend_mock.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "panel_widget_manager.h"
#include "runtime_config.h"
#include "src/ui/panel_widgets/network_widget.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <memory>
#include <thread>

#include "../catch_amalgamated.hpp"

using helix::NetworkWidget;
using helix::NetworkWidgetTestAccess;

namespace {

constexpr int kIconEthernet = 5;

struct NetworkWidgetStatusFixture : LVGLUITestFixture {
    ScopedRuntimeConfig scoped_config_;
    WifiBackendMock* wifi = nullptr;
    std::unique_ptr<NetworkWidget> widget;

    explicit NetworkWidgetStatusFixture(bool ethernet_up = false) {
        auto* rc = get_runtime_config();
        rc->test_mode = true;
        rc->use_real_wifi = false;
        EthernetBackendMock::set_default_connected(ethernet_up);
        helix::http::HttpExecutor::fast().start();
        PanelWidgetManager::instance().init_widget_subjects();
        // A module static: without this a previous test's icon satisfies a wait.
        lv_subject_set_int(lv_xml_get_subject(nullptr, "home_network_icon_state"), 0);

        auto wm = helix::get_wifi_manager();
        wifi = dynamic_cast<WifiBackendMock*>(helix::WiFiManagerTestAccess::backend(*wm));
        REQUIRE(wifi != nullptr);
    }

    ~NetworkWidgetStatusFixture() override {
        wifi->release_held_status();
        widget.reset();
        wifi->set_connected_state(false);
        EthernetBackendMock::set_default_connected(true);
    }

    void attach() {
        lv_obj_t* tile =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_network", nullptr));
        REQUIRE(tile != nullptr);
        widget = std::make_unique<NetworkWidget>();
        widget->attach(tile, test_screen());
    }

    static int icon() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "home_network_icon_state"));
    }

    /// Answers are delivered in the order they were read, so once a probe
    /// issued now is answered, every earlier answer has been applied.
    void await_earlier_answers() {
        auto delivered = std::make_shared<bool>(false);
        helix::get_wifi_manager()->get_status_async(
            probe_guard_.token(),
            [delivered](const WifiBackend::ConnectionStatus&) { *delivered = true; });
        REQUIRE(wait_until([&]() { return *delivered; }));
    }

    helix::AsyncLifetimeGuard probe_guard_;

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
};

struct EthernetUpFixture : NetworkWidgetStatusFixture {
    EthernetUpFixture() : NetworkWidgetStatusFixture(true) {}
};

} // namespace

TEST_CASE_METHOD(NetworkWidgetStatusFixture,
                 "network tile: detection reads WiFi off the UI thread and shows its strength",
                 "[network_widget]") {
    wifi->set_connected_state(true, "HomeNet", "192.168.1.100", 80);
    wifi->clear_status_callers();

    attach();
    REQUIRE(wait_until([&]() { return icon() == 4; }));
    CHECK(no_read_on_this_thread());
}

TEST_CASE_METHOD(NetworkWidgetStatusFixture,
                 "network tile: a signal poll tick reads off the UI thread and the icon follows",
                 "[network_widget]") {
    wifi->set_connected_state(true, "HomeNet", "192.168.1.100", 80);
    attach();
    REQUIRE(wait_until([&]() { return icon() == 4; }));
    lv_timer_t* poll = NetworkWidgetTestAccess::signal_poll_timer(*widget);
    REQUIRE(poll != nullptr);

    wifi->set_connected_state(true, "HomeNet", "192.168.1.100", 40);
    wifi->clear_status_callers();
    // The test harness runs only timers with a finite repeat count; lend this
    // periodic one a count for a single tick. Not 1: LVGL deletes a timer
    // whose count runs out, and the widget still holds this one.
    lv_timer_set_repeat_count(poll, 2);
    lv_timer_ready(poll);
    const bool updated = wait_until([&]() { return icon() == 2; });
    lv_timer_set_repeat_count(poll, -1);
    REQUIRE(updated);
    CHECK(no_read_on_this_thread());
}

TEST_CASE_METHOD(EthernetUpFixture,
                 "network tile: a WiFi answer landing after the Ethernet upgrade keeps Ethernet",
                 "[network_widget]") {
    // The WiFi read is parked until the Ethernet probe has upgraded the tile.
    wifi->set_connected_state(true, "HomeNet", "192.168.1.100", 80);
    wifi->clear_status_callers();
    wifi->hold_next_status();

    attach();
    REQUIRE(wait_until([&]() { return icon() == kIconEthernet; }));
    REQUIRE(wait_until([&]() { return !wifi->status_callers().empty(); }));

    wifi->release_held_status();
    await_earlier_answers();
    CHECK(icon() == kIconEthernet);
}

TEST_CASE_METHOD(NetworkWidgetStatusFixture,
                 "network tile: a WiFi answer landing after on_deactivate starts no poll timer",
                 "[network_widget]") {
    wifi->set_connected_state(true, "HomeNet", "192.168.1.100", 80);
    wifi->clear_status_callers();
    wifi->hold_next_status();

    attach();
    REQUIRE(wait_until([&]() { return !wifi->status_callers().empty(); }));
    widget->on_deactivate();

    wifi->release_held_status();
    await_earlier_answers();
    REQUIRE(icon() == 4); // the answer was applied
    CHECK(NetworkWidgetTestAccess::signal_poll_timer(*widget) == nullptr);
}

TEST_CASE_METHOD(NetworkWidgetStatusFixture,
                 "network tile: a DISCONNECTED event on a tile showing WiFi reaches Disconnected",
                 "[network_widget][1059]") {
    wifi->set_connected_state(true, "HomeNet", "192.168.1.100", 80);
    attach();
    REQUIRE(wait_until([&]() { return icon() == 4; }));

    // Clears the mock's link, then fires the event exactly as the backend does.
    helix::WiFiManagerTestAccess::fire_disconnected(*helix::get_wifi_manager());
    CHECK(wait_until([&]() { return icon() == 0; }));
}
