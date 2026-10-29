// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_network_settings_capabilities.cpp
 * @brief The Forget control, the WiFi radio toggle and the country row are
 *        shown only when the backend can perform them
 *        (prestonbrown/helixscreen#1429).
 */

#include "ui_overlay_network_settings.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/lvgl_event_hook_probe.h"
#include "../test_helpers/network_settings_overlay_test_access.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "system_settings_manager.h"
#include "wifi_backend_mock.h"
#include "wifi_country.h"
#include "wifi_manager.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using Access = NetworkSettingsOverlayTestAccess;

namespace {

/// A backend that owns neither the credential store nor the radio, the shape
/// of a printer whose network daemon manages WiFi.
class NoForgetNoToggleBackend : public WifiBackendMock {
  public:
    bool supports_forget() const override {
        return false;
    }
    bool supports_radio_toggle() const override {
        return false;
    }
    bool supports_country() const override {
        return false;
    }
};

class CapabilityFixture : public LVGLUITestFixture {
  protected:
    ScopedRuntimeConfig scoped_config;

    // A radio country these tests set is remembered; it would be sent to the
    // next test's world-domain mock radio.
    CapabilityFixture() {
        helix::SystemSettingsManager::instance().set_wifi_country_last_seen("");
    }
    ~CapabilityFixture() {
        helix::SystemSettingsManager::instance().set_wifi_country_last_seen("");
    }

    lv_obj_t* open_overlay(NetworkSettingsOverlay& overlay, std::unique_ptr<WifiBackend> backend) {
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_wifi = false;
        auto wm = std::make_shared<helix::WiFiManager>(std::move(backend));
        wm->init_self_reference(wm);
        overlay.init_subjects();
        overlay.register_callbacks();
        Access::wifi_manager(overlay) = wm;
        lv_obj_t* root = overlay.create(test_screen());
        REQUIRE(root != nullptr);
        overlay.on_activate();
        return root;
    }

    static bool hidden(lv_obj_t* root, const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(root, name);
        REQUIRE(obj != nullptr);
        return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
};

} // namespace

TEST_CASE_METHOD(CapabilityFixture,
                 "Network settings offers Forget and the radio toggle when supported",
                 "[network_settings][1429]") {
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::make_unique<WifiBackendMock>());

    CHECK_FALSE(hidden(root, "forget_network_icon"));
    CHECK_FALSE(hidden(root, "wlan_toggle"));
}

TEST_CASE_METHOD(
    CapabilityFixture,
    "Network settings hides Forget and the radio toggle when the backend cannot do them",
    "[network_settings][1429]") {
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::make_unique<NoForgetNoToggleBackend>());

    CHECK(hidden(root, "forget_network_icon"));
    CHECK(hidden(root, "wlan_toggle"));
}

TEST_CASE_METHOD(CapabilityFixture,
                 "Network settings hints at 5 GHz under a world-domain dual-band radio",
                 "[network_settings][wifi_country]") {
    helix::SystemSettingsManager::instance().set_wifi_country("");
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::make_unique<WifiBackendMock>());

    // The country is read on the control lane; the not-set row's label lands
    // with the answer.
    CHECK_FALSE(hidden(root, "wifi_country_row"));
    lv_obj_t* dropdown = lv_obj_find_by_name(root, "wifi_country_dropdown");
    REQUIRE(dropdown != nullptr);
    auto shown = [dropdown] {
        const char* text = lv_dropdown_get_text(dropdown);
        return std::string(text ? text : "");
    };
    REQUIRE(wait_until([&] { return shown() == "2.4 GHz only"; }));
    CHECK(lv_dropdown_get_selected(dropdown) == 0);
}

TEST_CASE_METHOD(CapabilityFixture,
                 "Network settings shows the system's country and drops the hint",
                 "[network_settings][wifi_country]") {
    helix::SystemSettingsManager::instance().set_wifi_country("");
    auto backend = std::make_unique<WifiBackendMock>();
    backend->set_country_for_test("DE");
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::move(backend));

    const int de = helix::wifi::country::dropdown_index("DE");
    REQUIRE(
        wait_until([&] { return lv_subject_get_int(&Access::wifi_country_index(overlay)) == de; }));
    lv_obj_t* dropdown = lv_obj_find_by_name(root, "wifi_country_dropdown");
    REQUIRE(dropdown != nullptr);
    CHECK(static_cast<int>(lv_dropdown_get_selected(dropdown)) == de);
    // Closed, it shows the code alone; the open list carries the name.
    REQUIRE(lv_dropdown_get_text(dropdown) != nullptr);
    CHECK(std::string(lv_dropdown_get_text(dropdown)) == "DE");
}

TEST_CASE_METHOD(CapabilityFixture,
                 "Network settings re-translates the country dropdown on a language switch",
                 "[network_settings][wifi_country][i18n]") {
    auto& settings = helix::SystemSettingsManager::instance();
    settings.set_wifi_country("");
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::make_unique<WifiBackendMock>());
    lv_obj_t* dropdown = lv_obj_find_by_name(root, "wifi_country_dropdown");
    REQUIRE(dropdown != nullptr);
    auto shown = [dropdown] {
        const char* text = lv_dropdown_get_text(dropdown);
        return std::string(text ? text : "");
    };
    REQUIRE(wait_until([&] { return shown() == "2.4 GHz only"; }));

    settings.set_language("ru");
    helix::ui::UpdateQueue::instance().drain();
    const std::string hint = lv_tr("2.4 GHz only");
    const std::string not_set = lv_tr("Not set");
    REQUIRE(hint != "2.4 GHz only");
    CHECK(shown() == hint);
    CHECK(std::string(lv_dropdown_get_options(dropdown)).rfind(not_set + "\n", 0) == 0);
    settings.set_language("en");
}

TEST_CASE_METHOD(CapabilityFixture,
                 "Network settings keeps the chosen country across a language switch",
                 "[network_settings][wifi_country][i18n]") {
    auto& settings = helix::SystemSettingsManager::instance();
    settings.set_wifi_country("");
    auto backend = std::make_unique<WifiBackendMock>();
    backend->set_country_for_test("DE");
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::move(backend));
    const int de = helix::wifi::country::dropdown_index("DE");
    REQUIRE(
        wait_until([&] { return lv_subject_get_int(&Access::wifi_country_index(overlay)) == de; }));
    lv_obj_t* dropdown = lv_obj_find_by_name(root, "wifi_country_dropdown");
    REQUIRE(dropdown != nullptr);

    settings.set_language("ru");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(static_cast<int>(lv_dropdown_get_selected(dropdown)) == de);
    REQUIRE(lv_dropdown_get_text(dropdown) != nullptr);
    CHECK(std::string(lv_dropdown_get_text(dropdown)) == "DE");
    settings.set_language("en");
}

TEST_CASE_METHOD(CapabilityFixture,
                 "Network settings leaves no language handler on its dropdown when destroyed",
                 "[network_settings][wifi_country][teardown]") {
    auto overlay = std::make_unique<NetworkSettingsOverlay>();
    lv_obj_t* root = open_overlay(*overlay, std::make_unique<WifiBackendMock>());
    lv_obj_t* dropdown = lv_obj_find_by_name(root, "wifi_country_dropdown");
    REQUIRE(dropdown != nullptr);
    const void* dead = overlay.get();
    REQUIRE(event_hook_installed(dropdown, dead));

    overlay.reset();
    CHECK_FALSE(event_hook_installed(dropdown, dead));
}

TEST_CASE_METHOD(CapabilityFixture,
                 "Network settings drops its dropdown's language handler on cleanup",
                 "[network_settings][wifi_country][teardown]") {
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::make_unique<WifiBackendMock>());
    lv_obj_t* dropdown = lv_obj_find_by_name(root, "wifi_country_dropdown");
    REQUIRE(dropdown != nullptr);
    REQUIRE(event_hook_installed(dropdown, &overlay));

    // A closed overlay's tree lives on after cleanup(); its handler must not.
    overlay.cleanup();
    REQUIRE(lv_obj_is_valid(dropdown));
    CHECK_FALSE(event_hook_installed(dropdown, &overlay));
}

TEST_CASE_METHOD(CapabilityFixture, "Picking a country in the dropdown sets it on the radio",
                 "[network_settings][wifi_country]") {
    helix::SystemSettingsManager::instance().set_wifi_country("");
    auto owned = std::make_unique<WifiBackendMock>();
    WifiBackendMock* backend = owned.get();
    // The dropdown's XML callback routes through the process-wide overlay, so
    // this path can only be driven on that instance.
    auto& overlay = get_network_settings_overlay();
    auto previous = Access::wifi_manager(overlay);
    lv_obj_t* root = open_overlay(overlay, std::move(owned));
    lv_obj_t* dropdown = lv_obj_find_by_name(root, "wifi_country_dropdown");
    REQUIRE(dropdown != nullptr);

    lv_dropdown_set_selected(dropdown,
                             static_cast<uint32_t>(helix::wifi::country::dropdown_index("NZ")));
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    // Saved once the radio accepts it.
    REQUIRE(wait_until(
        [] { return helix::SystemSettingsManager::instance().get_wifi_country() == "NZ"; }));
    std::string code;
    REQUIRE(backend->get_country(code).success());
    CHECK(code == "NZ");
    helix::SystemSettingsManager::instance().set_wifi_country("");
    overlay.cleanup();
    Access::wifi_manager(overlay) = previous;
}

TEST_CASE_METHOD(CapabilityFixture, "Network settings forgets its country dropdown on cleanup",
                 "[network_settings][wifi_country]") {
    NetworkSettingsOverlay overlay;
    open_overlay(overlay, std::make_unique<WifiBackendMock>());
    REQUIRE(Access::country_dropdown(overlay) != nullptr);
    overlay.cleanup();
    CHECK(Access::country_dropdown(overlay) == nullptr);
}

TEST_CASE_METHOD(CapabilityFixture, "Micro screens give the Ethernet MAC line up",
                 "[network_settings]") {
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::make_unique<WifiBackendMock>());
    lv_obj_t* mac = lv_obj_find_by_name(root, "eth_mac_label");
    REQUIRE(mac != nullptr);
    lv_obj_t* row = lv_obj_get_parent(mac);
    lv_subject_t* bp = lv_xml_get_subject(nullptr, "ui_breakpoint");
    REQUIRE(bp != nullptr);
    const int32_t saved = lv_subject_get_int(bp);

    lv_subject_set_int(bp, 0);
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(bp, 1);
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(bp, saved);
}

TEST_CASE_METHOD(CapabilityFixture,
                 "Network settings hides the country row when the backend cannot set it",
                 "[network_settings][wifi_country]") {
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::make_unique<NoForgetNoToggleBackend>());
    helix::ui::UpdateQueue::instance().drain();

    CHECK(hidden(root, "wifi_country_row"));
}

TEST_CASE_METHOD(CapabilityFixture,
                 "Network settings shows WiFi isn't responding only while control is unresponsive",
                 "[network_settings][wifi_control_lane]") {
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::make_unique<WifiBackendMock>());

    WifiBackend::ConnectionStatus status;
    status.connected = true;
    status.ssid = "Home";
    Access::apply_wifi_status(overlay, status);
    CHECK(hidden(root, "wifi_unresponsive_state"));

    status.unresponsive = true;
    Access::apply_wifi_status(overlay, status);
    CHECK_FALSE(hidden(root, "wifi_unresponsive_state"));
    // The rows above keep the last known state.
    CHECK_FALSE(hidden(root, "wifi_connected_state"));
}
