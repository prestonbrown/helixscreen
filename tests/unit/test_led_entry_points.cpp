// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_nav_manager.h"
#include "ui_print_light_timelapse.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_home_layout.h"
#include "config.h"
#include "core/update_queue.h"
#include "led/led_controller.h"
#include "led/ui_led_control_overlay.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/led_widget.h"

#include <array>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::led;

namespace {

void drain() {
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

/// Native `chamber_light` and `sb_leds` on a mock API, with the XML the overlay
/// is built from registered.
struct EntryFixture : public LVGLUITestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState& ps = state();
    std::unique_ptr<MoonrakerAPIMock> api;

    EntryFixture() {
        api = std::make_unique<MoonrakerAPIMock>(client, ps);
        auto& ctrl = LedController::instance();
        ctrl.deinit();
        ctrl.init(api.get(), &client);
        for (const char* id : {"neopixel chamber_light", "neopixel sb_leds"}) {
            LedStripInfo s;
            s.id = id;
            s.name = id;
            s.backend = LedBackendType::NATIVE;
            s.supports_color = true;
            s.supports_white = true;
            ctrl.native().add_strip(s);
        }

        helix::ui::destroy_static_panels();
        drain();
        std::array<lv_obj_t*, kUiPanelCount> panels{};
        for (auto& p : panels) {
            p = lv_obj_create(test_screen());
        }
        NavigationManager::instance().set_panels(panels.data());
    }

    ~EntryFixture() override {
        drain();
        NavigationManager::instance().shutdown();
        helix::ui::destroy_static_panels();
        drain();
        LedController::instance().deinit();
    }
};

} // namespace

TEST_CASE_METHOD(EntryFixture, "light chip follows the chamber light", "[led][entry]") {
    auto& ctrl = LedController::instance();
    ctrl.update_from_status({{"neopixel sb_leds", {{"color_data", {{1.0, 1.0, 1.0, 0.0}}}}}});
    CHECK_FALSE(helix::led::chamber_light_on());
    ctrl.update_from_status({{"neopixel chamber_light", {{"color_data", {{1.0, 1.0, 1.0, 0.0}}}}}});
    CHECK(helix::led::chamber_light_on());
}

TEST_CASE_METHOD(EntryFixture, "the LED controls tile opens on the last focused device",
                 "[led][entry]") {
    REQUIRE(helix::open_led_control_overlay(test_screen(), "neopixel sb_leds") != nullptr);
    drain();
    NavigationManager::instance().go_back();
    drain();
    REQUIRE(helix::open_led_control_overlay(test_screen()) != nullptr); // what the tile calls
    drain();
    CHECK(get_led_control_overlay().focused_device() == "neopixel sb_leds");
    NavigationManager::instance().go_back();
    drain();
}

TEST_CASE_METHOD(EntryFixture, "a light button's › opens the overlay on that button's device",
                 "[led][entry][light_button]") {
    struct Case {
        const char* key;
        const char* expected;
        const char* focused_before; ///< differs from expected, so "last focused" cannot pass
    };
    const Case c = GENERATE(Case{"neopixel sb_leds", "neopixel sb_leds", "neopixel chamber_light"},
                            Case{"all", "neopixel chamber_light", "neopixel sb_leds"});
    INFO("light key: " << c.key);

    REQUIRE(helix::open_led_control_overlay(test_screen(), c.focused_before) != nullptr);
    drain();
    NavigationManager::instance().go_back();
    drain();
    REQUIRE_FALSE(NavigationManager::instance().has_open_overlays());

    LedWidget w("led", ps, api.get());
    w.set_config({{"led", c.key}});
    lv_obj_t* root = lv_obj_create(test_screen());
    lv_obj_t* more = lv_obj_create(root);
    lv_obj_add_event_cb(more, LedWidget::light_more_cb, LV_EVENT_CLICKED, nullptr);
    w.attach_tile(root, test_screen());

    lv_obj_send_event(more, LV_EVENT_CLICKED, nullptr);
    drain();

    CHECK(NavigationManager::instance().has_open_overlays());
    CHECK(get_led_control_overlay().focused_device() == c.expected);
    NavigationManager::instance().go_back();
    drain();
    w.detach_tile();
}

TEST_CASE_METHOD(EntryFixture,
                 "with no home light button, the print-status light toggles only the chamber light",
                 "[led][entry]") {
    helix::test::ScopedHomeLayout layout(nlohmann::json::array());
    auto& ctrl = LedController::instance();
    PrintLightTimelapseControls controls;
    controls.handle_light_button();
    drain();
    CHECK(ctrl.native().has_strip_color("neopixel chamber_light"));
    CHECK_FALSE(ctrl.native().has_strip_color("neopixel sb_leds"));
}

TEST_CASE_METHOD(EntryFixture,
                 "the print-status light drives the light the home light button drives",
                 "[led][entry][light_button]") {
    helix::test::ScopedHomeLayout layout(
        nlohmann::json::array({helix::test::placed_light("led", 0)}));
    auto* cfg = Config::get_instance();
    cfg->set(cfg->df() + kLightButtonPendingPath, std::string("neopixel sb_leds"));
    auto& ctrl = LedController::instance();
    PrintLightTimelapseControls controls;
    controls.handle_light_button();
    drain();
    CHECK(ctrl.native().has_strip_color("neopixel sb_leds"));
    CHECK_FALSE(ctrl.native().has_strip_color("neopixel chamber_light"));
}

TEST_CASE_METHOD(EntryFixture, "the print-status light icon shows the home light button's light",
                 "[led][entry][light_button]") {
    constexpr const char* kBulbOff = "\xF3\xB0\x8C\xB6";
    constexpr const char* kBulbOn = "\xF3\xB0\x9B\xA8";
    helix::test::ScopedHomeLayout layout(
        nlohmann::json::array({helix::test::placed_light("led", 0)}));
    auto* cfg = Config::get_instance();
    cfg->set(cfg->df() + kLightButtonPendingPath, std::string("neopixel sb_leds"));
    auto& ctrl = LedController::instance();
    PrintLightTimelapseControls controls;
    controls.init_subjects();
    lv_subject_t* icon = lv_xml_get_subject(nullptr, "light_button_icon");
    REQUIRE(icon != nullptr);
    drain();
    REQUIRE(std::string(lv_subject_get_string(icon)) == kBulbOff);

    ctrl.update_from_status({{"neopixel chamber_light", {{"color_data", {{1.0, 1.0, 1.0, 0.0}}}}}});
    drain();
    CHECK(std::string(lv_subject_get_string(icon)) == kBulbOff);

    ctrl.update_from_status({{"neopixel sb_leds", {{"color_data", {{1.0, 1.0, 1.0, 0.0}}}}}});
    drain();
    CHECK(std::string(lv_subject_get_string(icon)) == kBulbOn);
}

TEST_CASE_METHOD(
    EntryFixture,
    "with no home light button, the print-status light button shows the chamber light's state",
    "[led][entry]") {
    helix::test::ScopedHomeLayout layout(nlohmann::json::array());
    constexpr const char* kBulbOff = "\xF3\xB0\x8C\xB6";
    constexpr const char* kBulbOn = "\xF3\xB0\x9B\xA8";
    auto& ctrl = LedController::instance();
    PrintLightTimelapseControls controls;
    controls.init_subjects();
    lv_subject_t* icon = lv_xml_get_subject(nullptr, "light_button_icon");
    REQUIRE(icon != nullptr);
    drain();
    REQUIRE(std::string(lv_subject_get_string(icon)) == kBulbOff);

    // Another light coming on is not the chamber light.
    ctrl.update_from_status({{"neopixel sb_leds", {{"color_data", {{1.0, 1.0, 1.0, 0.0}}}}}});
    drain();
    CHECK(std::string(lv_subject_get_string(icon)) == kBulbOff);

    ctrl.update_from_status({{"neopixel chamber_light", {{"color_data", {{1.0, 1.0, 1.0, 0.0}}}}}});
    drain();
    CHECK(std::string(lv_subject_get_string(icon)) == kBulbOn);

    ctrl.update_from_status({{"neopixel chamber_light", {{"color_data", {{0.0, 0.0, 0.0, 0.0}}}}}});
    drain();
    CHECK(std::string(lv_subject_get_string(icon)) == kBulbOff);

    controls.deinit_subjects();
}
