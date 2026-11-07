// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_network_settings_hidden_connect.cpp
 * @brief The shared hidden-network modal must ask the backend for a hidden join.
 *
 * `nmcli device wifi connect <ssid>` matches the SSID against NetworkManager's
 * scan cache, and a hidden SSID is by definition absent from it — so the
 * modal's Connect button has to carry the is_hidden flag all the way to the
 * backend or the join can only ever fail with "No network with SSID found".
 * Network settings and the setup wizard both open this one modal.
 */

#include "ui_hidden_network_modal.h"
#include "ui_overlay_network_settings.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/network_settings_overlay_test_access.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "../ui_test_utils.h"
#include "core/update_queue.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::HiddenJoin;
using Access = NetworkSettingsOverlayTestAccess;

namespace {

/// An SSID no mock backend knows, so the join is refused with the backend's own
/// NETWORK_NOT_FOUND reason - a failure that is NOT a credential rejection.
constexpr const char* kUnknownSsid = "StealthNet";

class HiddenConnectFixture : public LVGLUITestFixture {
  protected:
    WifiBackendMock* backend_ = nullptr;
    std::shared_ptr<helix::WiFiManager> wm_;
    std::vector<HiddenJoin> events_;

    HiddenConnectFixture() {
        auto backend = std::make_unique<WifiBackendMock>();
        REQUIRE(backend->start().success());
        backend_ = backend.get();
        wm_ = std::make_shared<helix::WiFiManager>(std::move(backend), /*silent=*/true);
        wm_->init_self_reference(wm_);
    }
    // A join's report lands through the UpdateQueue and writes events_, so it runs
    // while the fixture's members are alive.
    ~HiddenConnectFixture() override {
        helix::WiFiManagerTestAccess::settle(*wm_);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Opens the modal with `ssid` typed and security set to "None" (it defaults to
    /// WPA), so Connect needs no password.
    lv_obj_t* open(const char* ssid) {
        REQUIRE(helix::ui::show_hidden_network_modal(
            wm_, [this](HiddenJoin event, const std::string&) { events_.push_back(event); }));
        lv_obj_t* modal = Modal::get_top();
        REQUIRE(modal != nullptr);
        lv_obj_t* ssid_input = lv_obj_find_by_name(modal, "ssid_input");
        lv_obj_t* security = lv_obj_find_by_name(modal, "security_dropdown");
        REQUIRE(ssid_input != nullptr);
        REQUIRE(security != nullptr);
        lv_textarea_set_text(ssid_input, ssid);
        lv_dropdown_set_selected(security, 0);
        return modal;
    }

    static lv_obj_t* named(lv_obj_t* modal, const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(modal, name);
        REQUIRE(obj != nullptr);
        return obj;
    }

    static void connect(lv_obj_t* modal) {
        lv_obj_send_event(named(modal, "connect_btn"), LV_EVENT_CLICKED, nullptr);
    }
};

} // namespace

TEST_CASE_METHOD(HiddenConnectFixture,
                 "the hidden-network modal asks for a hidden join and shows the backend's reason",
                 "[network_settings][hidden][1542][wifi]") {
    lv_obj_t* modal = open(kUnknownSsid);

    connect(modal);
    REQUIRE(helix::WiFiManagerTestAccess::settle(*wm_));
    helix::ui::UpdateQueue::instance().drain();

    CHECK(backend_->last_connect_hidden());
    CHECK(events_ == std::vector<HiddenJoin>{HiddenJoin::Started, HiddenJoin::Failed});
    // A refused join keeps the form open and says why, not "check your credentials".
    REQUIRE(Modal::get_top() == modal);
    lv_obj_t* error_label = named(modal, "error_label");
    CHECK_FALSE(lv_obj_has_flag(error_label, LV_OBJ_FLAG_HIDDEN));
    const std::string shown = lv_label_get_text(error_label);
    CHECK(shown != lv_tr("Connection failed. Check credentials."));
    CHECK(shown.find(kUnknownSsid) != std::string::npos);
}

TEST_CASE_METHOD(HiddenConnectFixture,
                 "the hidden modal refuses an empty SSID without touching the backend",
                 "[network_settings][hidden]") {
    lv_obj_t* modal = open("");
    CHECK(lv_obj_has_flag(named(modal, "error_label"), LV_OBJ_FLAG_HIDDEN));

    connect(modal);

    CHECK_FALSE(lv_obj_has_flag(named(modal, "error_label"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(backend_->last_connect_hidden());
    CHECK(events_.empty());
}

TEST_CASE_METHOD(HiddenConnectFixture,
                 "the hidden modal swaps its form and Connect for a spinner while joining",
                 "[network_settings][hidden]") {
    // A network the mock stocks stays in flight for its connect delay.
    lv_obj_t* modal = open("Guest-Access");
    lv_obj_t* form = named(modal, "hidden_form_container");
    lv_obj_t* spinner = named(modal, "connecting_container");
    lv_obj_t* connect_btn = named(modal, "connect_btn");
    CHECK_FALSE(lv_obj_has_flag(form, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(spinner, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(connect_btn, LV_OBJ_FLAG_HIDDEN));

    connect(modal);

    CHECK(lv_obj_has_flag(form, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(spinner, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(connect_btn, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(named(modal, "cancel_btn"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(HiddenConnectFixture, "Add Hidden Network in network settings opens the modal",
                 "[network_settings][hidden]") {
    auto overlay = std::make_unique<NetworkSettingsOverlay>();
    overlay->init_subjects();
    overlay->register_callbacks();
    Access::wifi_manager(*overlay) = wm_;
    REQUIRE(overlay->create(test_screen()) != nullptr);
    lv_subject_set_int(&Access::wifi_hardware_available(*overlay), 1);

    Access::add_other_clicked(*overlay);

    lv_obj_t* modal = Modal::get_top();
    REQUIRE(modal != nullptr);
    CHECK(lv_obj_find_by_name(modal, "hidden_form_container") != nullptr);
}
