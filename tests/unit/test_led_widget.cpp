// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_context_menu.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/scoped_home_layout.h"
#include "app_globals.h"
#include "config.h"
#include "grid_layout.h"
#include "led/led_controller.h"
#include "led/led_devices.h"
#include "light_button_config.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/led_widget.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::led;
using helix::test::placed_light;
using helix::test::ScopedHomeLayout;

namespace {
struct LedWidgetFixture : public LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState ps;
    std::unique_ptr<MoonrakerAPIMock> api;

    LedWidgetFixture() {
        ps.init_subjects(false);
        ps.set_klippy_state_sync(KlippyState::READY);
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
        LedMacroInfo lamp;
        lamp.display_name = "Lamp";
        lamp.type = MacroLedType::TOGGLE;
        lamp.toggle_macro = "LIGHT_TOGGLE";
        LedMacroInfo party;
        party.display_name = "Party";
        party.type = MacroLedType::PRESET;
        party.presets = {"LED_PARTY"};
        ctrl.set_configured_macros({lamp, party});
        ctrl.rebuild_macro_backend();
    }
    ~LedWidgetFixture() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        LedController::instance().set_on_wled_settled(nullptr);
        LedController::instance().deinit();
    }

    std::string name_of(const std::string& instance_id) {
        return lv_subject_get_string(
            lv_xml_get_subject(nullptr, (instance_id + "_led_name").c_str()));
    }
};

size_t scripts_naming(const MoonrakerClientMock& client, const std::string& what) {
    const auto& h = client.gcode_script_history();
    return static_cast<size_t>(std::count_if(h.begin(), h.end(), [&](const std::string& s) {
        return s.find(what) != std::string::npos;
    }));
}
} // namespace

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: config round trip", "[led][light_button]") {
    LedWidget w("led:1", ps, api.get());
    w.set_config({{"led", "neopixel sb_leds"}});
    CHECK(w.light_key() == "neopixel sb_leds");
    CHECK(w.targets() == std::vector<std::string>{"neopixel sb_leds"});
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: unset means the chamber light",
                 "[led][light_button]") {
    LedWidget w("led", ps, api.get());
    w.set_config(nlohmann::json::object());
    CHECK(w.targets() == std::vector<std::string>{"neopixel chamber_light"});
    CHECK(w.overlay_device() == "neopixel chamber_light");
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: a vanished device resolves to the chamber light",
                 "[led][light_button]") {
    // A strip renamed in printer.cfg, or a macro device deleted in Settings.
    const std::string saved = GENERATE(std::string("neopixel gone"), std::string("macro:Gone"));
    INFO("saved key: " << saved);
    auto& ctrl = LedController::instance();
    const auto switchable = ctrl.switchable_ids();
    REQUIRE(std::find(switchable.begin(), switchable.end(), saved) == switchable.end());

    std::string chamber_name;
    for (const auto& d : ctrl.all_selectable_strips()) {
        if (d.id == "neopixel chamber_light") {
            chamber_name = device_display_name(d);
        }
    }
    REQUIRE_FALSE(chamber_name.empty());

    LedWidget w("led", ps, api.get());
    w.set_config({{"led", saved}});
    lv_obj_t* root = lv_obj_create(test_screen());
    lv_obj_t* button = lv_obj_create(root);
    lv_obj_add_event_cb(button, LedWidget::light_toggle_cb, LV_EVENT_CLICKED, nullptr);
    w.attach_tile(root, test_screen());

    CHECK(w.light_key() == saved);
    CHECK(name_of("led") == chamber_name);
    CHECK(w.targets() == std::vector<std::string>{"neopixel chamber_light"});
    CHECK(w.overlay_device() == "neopixel chamber_light");

    client.clear_gcode_script_history();
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(scripts_naming(client, "chamber_light") > 0);
    w.detach_tile();
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: All lights toggles every switchable device",
                 "[led][light_button]") {
    LedWidget w("led", ps, api.get());
    w.set_config({{"led", "all"}});
    const auto t = w.targets();
    CHECK(t == LedController::instance().switchable_ids());
    CHECK(std::find(t.begin(), t.end(), "macro:Party") == t.end());
    CHECK(w.overlay_device() == "neopixel chamber_light");
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: the picker offers no PRESET device",
                 "[led][light_button]") {
    std::vector<std::string> ids;
    for (const auto& d : light_picker_devices()) {
        ids.push_back(d.id);
    }
    CHECK(std::find(ids.begin(), ids.end(), "macro:Party") == ids.end());
    CHECK(ids == LedController::instance().switchable_ids());
}

TEST_CASE("light_tile_is_wide: two cells or more", "[led][light_button]") {
    CHECK_FALSE(light_tile_is_wide(helix::GridLayout::TRACKS_PER_CELL));
    CHECK_FALSE(light_tile_is_wide(2 * helix::GridLayout::TRACKS_PER_CELL - 1));
    CHECK(light_tile_is_wide(2 * helix::GridLayout::TRACKS_PER_CELL));
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: a fresh button names and toggles the chamber light",
                 "[led][light_button]") {
    LedWidget w("led", ps, api.get());
    w.set_config(nlohmann::json::object());
    lv_obj_t* root = lv_obj_create(test_screen());
    lv_obj_t* button = lv_obj_create(root);
    lv_obj_add_event_cb(button, LedWidget::light_toggle_cb, LV_EVENT_CLICKED, nullptr);
    w.attach_tile(root, test_screen());

    CHECK(name_of("led") == "Chamber Light");

    client.clear_gcode_script_history();
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(scripts_naming(client, "chamber_light") > 0);
    CHECK(scripts_naming(client, "sb_leds") == 0);
    w.detach_tile();
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: the name follows the chosen device",
                 "[led][light_button]") {
    LedWidget w("led", ps, api.get());
    w.set_config({{"led", "neopixel sb_leds"}});
    w.attach_tile(lv_obj_create(test_screen()), test_screen());
    CHECK(name_of("led") == "Sb LEDs");
    w.detach_tile();

    LedWidget all("led:1", ps, api.get());
    all.set_config({{"led", "all"}});
    all.attach_tile(lv_obj_create(test_screen()), test_screen());
    CHECK(name_of("led:1") == "All lights");
    all.detach_tile();
}

TEST_CASE_METHOD(LedWidgetFixture,
                 "LedWidget: a staged selection lands in every unset home light button",
                 "[led][light_button]") {
    ScopedHomeLayout layout(
        nlohmann::json::array({placed_light("led", 0), placed_light("led:1", 2)}));
    auto* cfg = Config::get_instance();
    cfg->set(cfg->df() + kLightButtonPendingPath, std::string("neopixel sb_leds"));

    LedWidget first("led", ps, api.get());
    LedWidget second("led:1", ps, api.get());
    first.set_panel_id("home");
    second.set_panel_id("home");
    first.set_config(nlohmann::json::object());
    second.set_config(nlohmann::json::object());

    first.attach_tile(lv_obj_create(test_screen()), test_screen());
    second.attach_tile(lv_obj_create(test_screen()), test_screen());

    CHECK(first.light_key() == "neopixel sb_leds");
    CHECK(second.light_key() == "neopixel sb_leds");
    CHECK(first.targets() == std::vector<std::string>{"neopixel sb_leds"});
    CHECK(cfg->try_get_json(cfg->df() + kLightButtonPendingPath)->is_null());
    first.detach_tile();
    second.detach_tile();
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: the name follows a change in the set of devices",
                 "[led][light_button]") {
    auto& ctrl = LedController::instance();
    const auto name_for = [&](const std::string& id) {
        for (const auto& d : ctrl.all_selectable_strips()) {
            if (d.id == id) {
                return device_display_name(d);
            }
        }
        return std::string();
    };

    // Bound to a WLED strip before WLED discovery has answered.
    LedWidget wled("led", ps, api.get());
    wled.set_config({{"led", "printer_led"}});
    wled.attach(lv_obj_create(test_screen()), test_screen());
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(name_of("led") == "Chamber Light");
    ctrl.discover_wled_strips();
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE_FALSE(name_for("printer_led").empty());
    CHECK(name_of("led") == name_for("printer_led"));
    wled.detach();

    // Bound to a macro device that is then deleted in Settings.
    LedWidget macro("led:1", ps, api.get());
    macro.set_config({{"led", "macro:Lamp"}});
    macro.attach(lv_obj_create(test_screen()), test_screen());
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(name_of("led:1") == "Lamp");
    std::vector<LedMacroInfo> kept;
    for (const auto& m : ctrl.configured_macros()) {
        if (m.display_name != "Lamp") {
            kept.push_back(m);
        }
    }
    ctrl.set_configured_macros(kept);
    ctrl.rebuild_macro_backend();
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(name_of("led:1") == "Chamber Light");
    macro.detach();
}

TEST_CASE_METHOD(LedWidgetFixture,
                 "settle_light_buttons: a staged selection with no light button on home is "
                 "dropped",
                 "[led][light_button]") {
    ScopedHomeLayout layout(nlohmann::json::array());
    auto* cfg = Config::get_instance();
    cfg->set(cfg->df() + kLightButtonPendingPath, std::string("neopixel sb_leds"));

    settle_light_buttons();
    CHECK(cfg->try_get_json(cfg->df() + kLightButtonPendingPath)->is_null());

    // A light button added to home later defaults to the chamber light.
    ScopedHomeLayout later(nlohmann::json::array({placed_light("led", 0)}));
    LedWidget added("led", ps, api.get());
    added.set_panel_id("home");
    added.set_config(nlohmann::json::object());
    added.attach(lv_obj_create(test_screen()), test_screen());
    CHECK(added.light_key().empty());
    CHECK(added.targets() == std::vector<std::string>{"neopixel chamber_light"});
    added.detach();
}

namespace helix {
void register_led_widget();
} // namespace helix

namespace {
struct LedPickerFixture : public XMLTestFixture {
    LedPickerFixture() {
        helix::ui::ContextMenu::register_shared_callbacks();
        register_led_widget();
        REQUIRE(register_component("led_picker"));
        auto& ctrl = LedController::instance();
        ctrl.deinit();
        ctrl.init(&api(), &client());
        add_strips({"neopixel chamber_light", "neopixel sb_leds"});
    }
    ~LedPickerFixture() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        LedController::instance().deinit();
    }
    static void add_strips(const std::vector<std::string>& ids) {
        for (const auto& id : ids) {
            LedStripInfo s;
            s.id = id;
            s.name = id;
            s.backend = LedBackendType::NATIVE;
            LedController::instance().native().add_strip(s);
        }
    }
};
} // namespace

TEST_CASE_METHOD(LedPickerFixture, "LedWidget: a picker tap picks the row the user saw",
                 "[led][light_button]") {
    LedWidget w("led", state(), &api());
    w.set_config(nlohmann::json::object());
    w.attach_tile(lv_obj_create(test_screen()), test_screen());
    w.on_edit_configure();
    lv_obj_t* row = lv_obj_find_by_name(test_screen(), "led_picker_row_1");
    REQUIRE(row != nullptr);

    // The device list reorders while the picker is open.
    LedController::instance().native().clear();
    add_strips({"neopixel sb_leds", "neopixel chamber_light"});
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(w.light_key() == "neopixel sb_leds");
    w.detach_tile();
}

namespace {
/// A home light button bound to the WLED strip the mock reports as off.
nlohmann::json wled_bound_light() {
    nlohmann::json e = placed_light("led", 0);
    e["config"] = {{"led", "enclosure_led"}};
    return e;
}

void drain_queue() {
    helix::ui::UpdateQueue::instance().drain_until_empty();
}
} // namespace

TEST_CASE_METHOD(LedWidgetFixture,
                 "LED on at Start waits for WLED when discovery-complete lands first",
                 "[led][light_button][startup]") {
    ScopedHomeLayout layout(nlohmann::json::array({wled_bound_light()}));
    auto& ctrl = LedController::instance();
    ctrl.set_on_wled_settled(settle_light_buttons); // as Application wires it
    ctrl.set_led_on_at_start(true);
    api->rest_mock().mock_hold_wled_strips();
    ctrl.discover_wled_strips();
    client.clear_gcode_script_history();

    settle_light_buttons(); // discovery-complete, WLED still unanswered
    drain_queue();
    CHECK(scripts_naming(client, "chamber_light") == 0);

    api->rest_mock().mock_release_wled_strips();
    drain_queue();
    CHECK(ctrl.device_state("enclosure_led").power == PowerState::On);
    CHECK(scripts_naming(client, "chamber_light") == 0);
}

TEST_CASE_METHOD(LedWidgetFixture, "LED on at Start when WLED answers before discovery-complete",
                 "[led][light_button][startup]") {
    ScopedHomeLayout layout(nlohmann::json::array({wled_bound_light()}));
    auto& ctrl = LedController::instance();
    ctrl.set_on_wled_settled(settle_light_buttons);
    ctrl.set_led_on_at_start(true);
    client.clear_gcode_script_history();

    ctrl.discover_wled_strips();
    drain_queue();
    CHECK(ctrl.device_state("enclosure_led").power == PowerState::On);

    settle_light_buttons(); // discovery-complete: the attempt is spent
    drain_queue();
    CHECK(scripts_naming(client, "chamber_light") == 0);
}

TEST_CASE_METHOD(LedWidgetFixture,
                 "LED on at Start falls back to the chamber light when WLED never answers",
                 "[led][light_button][startup]") {
    ScopedHomeLayout layout(nlohmann::json::array({wled_bound_light()}));
    auto& ctrl = LedController::instance();
    ctrl.set_on_wled_settled(settle_light_buttons);
    ctrl.set_led_on_at_start(true);
    api->rest_mock().mock_hold_wled_strips();
    ctrl.discover_wled_strips();
    client.clear_gcode_script_history();

    settle_light_buttons();
    drain_queue();
    REQUIRE(scripts_naming(client, "chamber_light") == 0);

    process_lvgl(LedController::kWledDiscoveryTimeoutMs + 100);
    drain_queue();
    CHECK(scripts_naming(client, "chamber_light") > 0);
}

TEST_CASE_METHOD(LedWidgetFixture,
                 "A PRESET-only printer offers the LED Controls tile but no light button",
                 "[led][light_button]") {
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(api.get(), &client);
    LedMacroInfo party;
    party.display_name = "Party";
    party.type = MacroLedType::PRESET;
    party.presets = {"LED_PARTY"};
    ctrl.set_configured_macros({party});
    REQUIRE(ctrl.switchable_ids().empty());

    nlohmann::json controls = placed_light("led_controls", 2);
    ScopedHomeLayout layout(nlohmann::json::array({placed_light("led", 0), controls}));
    const auto ids = PanelWidgetManager::instance().compute_visible_widget_ids("home", 0);
    CHECK(std::find(ids.begin(), ids.end(), "led~gated") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "led_controls") != ids.end());

    // With no LED device at all, the tile is gated too.
    ctrl.set_configured_macros({});
    const auto none = PanelWidgetManager::instance().compute_visible_widget_ids("home", 0);
    CHECK(std::find(none.begin(), none.end(), "led_controls~gated") != none.end());
}

TEST_CASE("light_icon_look: lit by any on target, dark otherwise", "[led][light_button]") {
    DeviceState off{PowerState::Off, 0, 0xFF0000, true};
    DeviceState unknown;
    DeviceState dim_red{PowerState::On, 20, 0xFF0000, true};
    DeviceState bright_plain{PowerState::On, 80, 0xFFFFFF, false};

    CHECK(light_icon_look({unknown, unknown}).unknown);
    CHECK(light_icon_look({unknown, unknown}).brightness == 0);

    const auto dark = light_icon_look({off, unknown});
    CHECK_FALSE(dark.unknown);
    CHECK(dark.brightness == 0);

    const auto lit = light_icon_look({bright_plain, dim_red, off});
    CHECK(lit.brightness == 80);
    CHECK(lit.has_rgb);
    CHECK(lit.rgb == 0xFF0000);

    CHECK_FALSE(light_icon_look({bright_plain}).has_rgb);
}

TEST_CASE_METHOD(LedWidgetFixture,
                 "LedWidget: All lights lights in the theme color, not a strip's hue",
                 "[led][light_button]") {
    auto& ctrl = LedController::instance();
    ctrl.update_from_status({{"neopixel chamber_light", {{"color_data", {{0.0, 0.0, 0.0, 0.0}}}}},
                             {"neopixel sb_leds", {{"color_data", {{1.0, 0.0, 0.0, 0.0}}}}}});

    LedWidget one("led", ps, api.get());
    one.set_config({{"led", "neopixel sb_leds"}});
    const auto red = one.icon_look();
    REQUIRE(red.brightness > 0);
    CHECK(red.has_rgb);
    CHECK(red.rgb == 0xFF0000);

    LedWidget all("led:1", ps, api.get());
    all.set_config({{"led", "all"}});
    const auto look = all.icon_look();
    CHECK(look.brightness > 0);
    CHECK_FALSE(look.has_rgb);
}

TEST_CASE_METHOD(LedWidgetFixture, "toggle_lights: switches its ids and reports the state sent",
                 "[led][light_button]") {
    client.clear_gcode_script_history();
    CHECK(toggle_lights({}, "test").outcome == LightToggle::NoTarget);
    CHECK(client.gcode_script_history().empty());

    CHECK(toggle_lights({"neopixel sb_leds"}, "test").outcome == LightToggle::On);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(scripts_naming(client, "sb_leds") == 1);
    CHECK(scripts_naming(client, "chamber_light") == 0);
}

TEST_CASE_METHOD(LedWidgetFixture,
                 "toggle_lights: unconfirmed only when no target reports its power",
                 "[led][light_button]") {
    auto& ctrl = LedController::instance();
    // A TOGGLE macro has no readable state; a native strip has none until Klipper reports it.
    const auto lamp = toggle_lights({"macro:Lamp"}, "test");
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(lamp.outcome == LightToggle::On);
    CHECK(lamp.unconfirmed);

    ctrl.update_from_status({{"neopixel sb_leds", {{"color_data", {{0.0, 0.0, 0.0, 0.0}}}}}});
    REQUIRE(ctrl.device_state("neopixel sb_leds").power == PowerState::Off);
    const auto mixed = toggle_lights({"macro:Lamp", "neopixel sb_leds"}, "test");
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK_FALSE(mixed.unconfirmed);

    CHECK_FALSE(toggle_lights({}, "test").unconfirmed);
}

TEST_CASE_METHOD(LedWidgetFixture,
                 "toggle_lights: refuses while a light command awaits its ACK, for every button",
                 "[led][light_button]") {
    LedWidget w("led", ps, api.get());
    w.set_config(nlohmann::json::object());
    lv_obj_t* root = lv_obj_create(test_screen());
    lv_obj_t* button = lv_obj_create(root);
    lv_obj_add_event_cb(button, LedWidget::light_toggle_cb, LV_EVENT_CLICKED, nullptr);
    w.attach_tile(root, test_screen());

    // Not drained: its ACK has not landed.
    LedController::instance().set_power({"neopixel sb_leds"}, true);
    REQUIRE(LedController::instance().light_command_in_flight());
    client.clear_gcode_script_history();
    CHECK(toggle_lights({"neopixel chamber_light"}, "test").outcome == LightToggle::Busy);
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    CHECK(client.gcode_script_history().empty());

    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE_FALSE(LedController::instance().light_command_in_flight());
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(scripts_naming(client, "chamber_light") > 0);
    w.detach_tile();
}
