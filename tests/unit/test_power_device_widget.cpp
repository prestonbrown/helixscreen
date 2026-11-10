// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_power_device_widget.cpp
 * @brief The home power tile: following its device across a Klipper restart,
 *        and the user colour it paints with
 */

#include "ui_icon.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/power_device_widget_test_access.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "device_display_name.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_types.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "power_device_state.h"
#include "power_device_widget.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "tile_color.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using Access = helix::PowerDeviceWidgetTestAccess;

namespace {

constexpr uint32_t kBlue = 0x1E88E5;
constexpr const char* kPanel = "power_tile_test_panel";

uint32_t rgb(lv_color_t c) {
    return lv_color_to_u32(c) & 0xFFFFFF;
}

class PowerTileFixture : public LVGLUITestFixture {
  public:
    PowerTileFixture() {
        init_widget_registrations();
        tile_color::register_subjects();
        PowerDeviceState::instance().deinit_subjects();
        PowerDeviceState::instance().subscribe(api_);
    }
    ~PowerTileFixture() override {
        harness_.reset();
        PowerDeviceState::instance().unsubscribe(api_);
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    /// A notify_power_changed frame, delivered as the WebSocket delivers it.
    void notify(const char* device, const char* status) {
        client_.dispatch_method_callback(
            "notify_power_changed",
            {{"method", "notify_power_changed"},
             {"params", nlohmann::json::array({{{"device", device}, {"status", status}}})}});
        drain();
    }

    /// The real panel_widget_power_device component with a widget attached.
    PowerDeviceWidget& make_tile(const nlohmann::json& config) {
        harness_ = std::make_unique<PanelWidgetHarness<PowerDeviceWidget>>(
            test_screen(), HarnessConfig{config}, "power_device:1");
        drain(); // the status observer delivers its first value through the queue
        return harness_->widget();
    }

    std::string status_text() {
        lv_obj_t* label = harness_->child("power_device_status");
        REQUIRE(label != nullptr);
        return lv_label_get_text(label);
    }
    uint32_t icon_rgb() {
        lv_obj_t* icon = harness_->child("power_icon");
        REQUIRE(icon != nullptr);
        return rgb(lv_obj_get_style_text_color(icon, LV_PART_MAIN));
    }
    uint32_t badge_rgb() {
        lv_obj_t* badge = harness_->child("power_badge");
        REQUIRE(badge != nullptr);
        return rgb(lv_obj_get_style_bg_color(badge, LV_PART_MAIN));
    }

    MoonrakerClientMock client_{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api_{client_, get_printer_state()};
    std::unique_ptr<PanelWidgetHarness<PowerDeviceWidget>> harness_;
};

/// The colour the "muted" icon variant paints, read off a scratch label.
uint32_t muted_variant_rgb(lv_obj_t* screen) {
    lv_obj_t* probe = lv_label_create(screen);
    helix::ui::icon::set_variant(probe, "muted");
    const uint32_t c = rgb(lv_obj_get_style_text_color(probe, LV_PART_MAIN));
    lv_obj_delete(probe);
    return c;
}

} // namespace

TEST_CASE_METHOD(PowerTileFixture, "Power tile: follows its device across a Klipper restart",
                 "[power_device_widget][rediscovery]") {
    const std::vector<PowerDevice> devices = {{"printer_psu", "gpio", "off", false}};
    PowerDeviceState::instance().set_devices(devices);
    make_tile({{"device", "printer_psu"}});
    REQUIRE(status_text() == "OFF");

    // notify_klippy_ready re-runs discovery with the same list; the home panel
    // keeps the tile, so the tile keeps its observer.
    PowerDeviceState::instance().set_devices(devices);

    notify("printer_psu", "on");
    CHECK(status_text() == "ON");
    notify("printer_psu", "off");
    CHECK(status_text() == "OFF");
}

// With no user colour each tile wears its own state's colours.
TEST_CASE_METHOD(PowerTileFixture, "Power tile: two tiles keep their own state",
                 "[power_device_widget][tile_color]") {
    PowerDeviceState::instance().set_devices(
        {{"printer_psu", "gpio", "on", false}, {"bed_lamp", "gpio", "off", false}});
    PanelWidgetHarness<PowerDeviceWidget> psu(
        test_screen(), HarnessConfig{{{"device", "printer_psu"}}, true}, "power_device:1");
    PanelWidgetHarness<PowerDeviceWidget> lamp(
        test_screen(), HarnessConfig{{{"device", "bed_lamp"}}, true}, "power_device:2");
    psu.create_tile(test_screen());
    lamp.create_tile(test_screen());
    drain();
    auto text = [](PanelWidgetHarnessBase& h, const char* name) {
        return std::string(lv_label_get_text(h.child(name)));
    };
    auto badge = [](PanelWidgetHarnessBase& h) {
        return rgb(lv_obj_get_style_bg_color(h.child("power_badge"), LV_PART_MAIN));
    };
    auto icon = [](PanelWidgetHarnessBase& h) {
        return rgb(lv_obj_get_style_text_color(h.child("power_icon"), LV_PART_MAIN));
    };
    const uint32_t danger = rgb(theme_manager_get_color("danger"));
    const uint32_t muted = rgb(theme_manager_get_color("text_muted"));
    CHECK(text(psu, "power_device_status") == "ON");
    CHECK(text(lamp, "power_device_status") == "OFF");
    CHECK(text(psu, "power_device_name") != text(lamp, "power_device_name"));
    CHECK((badge(psu) == danger && icon(psu) == danger));
    CHECK((badge(lamp) == muted && icon(lamp) == muted_variant_rgb(test_screen())));

    notify("printer_psu", "off");
    notify("bed_lamp", "on");
    CHECK(text(psu, "power_device_status") == "OFF");
    CHECK(text(lamp, "power_device_status") == "ON");
    CHECK((badge(psu) == muted && badge(lamp) == danger));
}

TEST_CASE_METHOD(PowerTileFixture, "Power tile: a user colour paints on, mutes off, spares locked",
                 "[power_device_widget][tile_color]") {
    auto& ps = get_printer_state();
    ps.init_subjects(false);
    PowerDeviceState::instance().deinit_subjects();
    PowerDeviceState::instance().set_devices({{"printer_psu", "gpio", "on", true}});
    make_tile({{"device", "printer_psu"}, {"color", kBlue}});

    // On: the chosen colour.
    CHECK(icon_rgb() == kBlue);
    CHECK(badge_rgb() == kBlue);
    CHECK(rgb(lv_obj_get_style_text_color(harness_->child("power_device_status"), LV_PART_MAIN)) ==
          kBlue);

    // Off: the same hue toned toward the theme's muted text.
    notify("printer_psu", "off");
    const uint32_t muted =
        rgb(tile_color::muted(lv_color_hex(kBlue), theme_manager_get_color("text_muted")));
    CHECK(muted != kBlue);
    CHECK(icon_rgb() == muted);
    CHECK(badge_rgb() == muted);

    // Locked while a job holds the machine: today's locked look, colour or not.
    notify("printer_psu", "on");
    ps.print_state().set_print_start_state(helix::PrintStartPhase::BED_MESH, "", 0);
    drain();
    REQUIRE(status_text() == "LOCKED");
    CHECK(icon_rgb() == muted_variant_rgb(test_screen()));
    CHECK(badge_rgb() == rgb(theme_manager_get_color("text_muted")));

    ps.print_state().reset_print_start_state();
    drain();
}

TEST_CASE_METHOD(PowerTileFixture, "Power tile: a picked colour is saved and applied live",
                 "[power_device_widget][tile_color][picker]") {
    PowerDeviceState::instance().set_devices({{"printer_psu", "gpio", "on", false}});

    // Seed the layout the widget saves into: PanelWidgetConfig keeps config
    // only for widget ids present on a loaded page.
    const nlohmann::json tile_config = {{"device", "printer_psu"}, {"icon", "power_plug"}};
    nlohmann::json layout;
    layout["pages"].push_back({{"id", "main"},
                               {"widgets", nlohmann::json::array({{{"id", "power_device:1"},
                                                                   {"enabled", true},
                                                                   {"config", tile_config}}})}});
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);
    cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/" + kPanel, layout);
    auto& wc = PanelWidgetManager::instance().get_widget_config(kPanel);
    wc.mark_dirty();
    wc.load();

    auto& widget = make_tile(tile_config);
    widget.set_panel_id(kPanel);
    Access::set_screen(widget, test_screen(), harness_->root());
    Access::show_picker(widget);
    lv_obj_t* backdrop = Access::backdrop(test_screen());
    REQUIRE(backdrop != nullptr);

    lv_subject_t* selected = lv_xml_get_subject(nullptr, "tile_color_selected");
    REQUIRE(selected != nullptr);
    CHECK(lv_subject_get_int(selected) == tile_color::palette_index(0));

    lv_obj_t* blue = lv_obj_find_by_name(backdrop, "tile_color_swatch_9");
    REQUIRE(blue != nullptr);
    REQUIRE(tile_color::palette_at(9) == kBlue);
    lv_obj_send_event(blue, LV_EVENT_CLICKED, nullptr);
    drain();

    // Live: the tile repaints and the card stays up for another pick.
    CHECK(icon_rgb() == kBlue);
    CHECK(Access::picker_visible(widget));
    CHECK(lv_subject_get_int(selected) == 9);

    // Saved beside the device and icon, and read back by a fresh tile.
    const nlohmann::json saved = wc.get_widget_config("power_device:1");
    CHECK(saved.value("color", 0u) == kBlue);
    CHECK(saved.value("device", "") == "printer_psu");
    CHECK(saved.value("icon", "") == "power_plug");

    Access::hide_picker(widget);
    process_lvgl(50);
    make_tile(saved);
    CHECK(icon_rgb() == kBlue);

    // "No colour" removes the key, so the tile is back on the state colours.
    auto& again = harness_->widget();
    again.set_panel_id(kPanel);
    Access::set_screen(again, test_screen(), harness_->root());
    Access::show_picker(again);
    lv_obj_t* none = lv_obj_find_by_name(Access::backdrop(test_screen()), "tile_color_swatch_15");
    REQUIRE(none != nullptr);
    lv_obj_send_event(none, LV_EVENT_CLICKED, nullptr);
    drain();
    CHECK_FALSE(wc.get_widget_config("power_device:1").contains("color"));
    CHECK(icon_rgb() == rgb(theme_manager_get_color("danger")));
    Access::hide_picker(again);
    process_lvgl(50);
}

TEST_CASE("tile_color: paint_for keeps the theme look unless a colour applies", "[tile_color]") {
    using tile_color::Paint;
    using tile_color::TileState;
    CHECK(tile_color::paint_for(kBlue, TileState::Active) == Paint::Custom);
    CHECK(tile_color::paint_for(kBlue, TileState::Inactive) == Paint::CustomMuted);
    CHECK(tile_color::paint_for(kBlue, TileState::Locked) == Paint::Theme);
    CHECK(tile_color::paint_for(kBlue, TileState::Unconfigured) == Paint::Theme);
    for (auto s :
         {TileState::Active, TileState::Inactive, TileState::Locked, TileState::Unconfigured}) {
        CHECK(tile_color::paint_for(0, s) == Paint::Theme);
    }
}

TEST_CASE("tile_color: the stored colour is read tolerantly", "[tile_color]") {
    CHECK(tile_color::color_from_config({{"color", kBlue}}) == kBlue);
    CHECK(tile_color::color_from_config({{"color", 0}}) == 0);
    CHECK(tile_color::color_from_config({{"color", "blue"}}) == 0);
    CHECK(tile_color::color_from_config({{"color", -1}}) == 0);
    CHECK(tile_color::color_from_config({{"color", 0x1000000}}) == 0);
    CHECK(tile_color::color_from_config(nlohmann::json::object()) == 0);
    CHECK(tile_color::color_from_config(nlohmann::json()) == 0);
}

// A device that left discovery while the picker was open has no state to show
// yet, so the tile names it and shows no ON/OFF rather than the last device's.
TEST_CASE_METHOD(PowerTileFixture, "Power tile: a picked device with no state reads unknown",
                 "[power_device_widget][select_device]") {
    PowerDeviceState::instance().set_devices(
        {{"bed_lamp", "gpio", "off", false}, {"printer_psu", "gpio", "on", false}});
    auto& widget = make_tile({{"device", "printer_psu"}});
    REQUIRE(status_text() == "ON");

    Access::set_screen(widget, test_screen(), harness_->root());
    Access::show_picker(widget);
    lv_obj_t* list = lv_obj_find_by_name(Access::backdrop(test_screen()), "device_list");
    REQUIRE(list != nullptr);
    lv_obj_t* lamp_row = lv_obj_get_child(list, 1); // after All Devices, sorted
    REQUIRE(lamp_row != nullptr);

    PowerDeviceState::instance().set_devices({{"printer_psu", "gpio", "on", false}});
    lv_obj_send_event(lamp_row, LV_EVENT_CLICKED, nullptr);
    drain();

    REQUIRE(Access::device_name(widget) == "bed_lamp");
    CHECK(status_text().empty());
    lv_obj_t* name = harness_->child("power_device_name");
    REQUIRE(name != nullptr);
    CHECK_FALSE(lv_obj_has_flag(name, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(name)) ==
          helix::get_display_name("bed_lamp", helix::DeviceType::POWER_DEVICE));
}

// An unknown status shows the Configure prompt; a known one brings the name back.
TEST_CASE_METHOD(PowerTileFixture, "Power tile: the name returns after an unknown status",
                 "[power_device_widget]") {
    PowerDeviceState::instance().set_devices({{"printer_psu", "gpio", "off", false}});
    make_tile({{"device", "printer_psu"}});
    lv_obj_t* name = harness_->child("power_device_name");
    REQUIRE(name != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(name, LV_OBJ_FLAG_HIDDEN));

    SubjectLifetime lifetime;
    lv_subject_t* status = PowerDeviceState::instance().get_status_subject("printer_psu", lifetime);
    REQUIRE(status != nullptr);
    lv_subject_set_int(status, -1);
    drain();
    CHECK(lv_obj_has_flag(name, LV_OBJ_FLAG_HIDDEN));

    lv_subject_set_int(status, 1);
    drain();
    CHECK_FALSE(lv_obj_has_flag(name, LV_OBJ_FLAG_HIDDEN));
    CHECK(status_text() == "ON");
}
