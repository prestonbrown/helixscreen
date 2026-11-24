// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_unit_tile_widget.cpp
 * @brief Per-tile unit text on the humidity and width-sensor home tiles (#939).
 *
 * An override replaces the unit after the reading and never converts it; an
 * empty override draws no unit; a tile with none keeps its default.
 */

#include "ui_context_menu.h"
#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_home_layout.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "humidity_sensor_manager.h"
#include "panel_widget_manager.h"
#include "src/ui/panel_widgets/unit_tile_widget.h"
#include "width_sensor_manager.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

lv_subject_t* humidity_subject() {
    auto& sensors = helix::sensors::HumiditySensorManager::instance();
    sensors.init_subjects();
    return sensors.get_chamber_humidity_subject();
}

lv_subject_t* width_subject() {
    auto& sensors = helix::sensors::WidthSensorManager::instance();
    sensors.init_subjects();
    return sensors.get_diameter_subject();
}

using HumidityTile = PanelWidgetHarness<UnitTileWidget>;

std::string text_of(PanelWidgetHarnessBase& tile, const char* name) {
    lv_obj_t* label = tile.child(name);
    REQUIRE(label != nullptr);
    return lv_label_get_text(label);
}

HarnessConfig config_with(const nlohmann::json& unit) {
    nlohmann::json value = nlohmann::json::object();
    if (!unit.is_null()) {
        value["unit_override"] = unit;
    }
    return HarnessConfig{value, true};
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "unit tile: humidity unit override relabels its own tile only",
                 "[unit_tile][unit_override]") {
    lv_subject_set_int(humidity_subject(), 612);
    const auto& spec = UnitTileWidget::humidity_spec();
    HumidityTile custom(test_screen(), config_with("RH"), "humidity:1", spec);
    HumidityTile plain(test_screen(), config_with(nullptr), "humidity:2", spec);
    HumidityTile hidden(test_screen(), config_with(""), "humidity:3", spec);
    custom.create_tile(test_screen());
    plain.create_tile(test_screen());
    hidden.create_tile(test_screen());

    CHECK(text_of(custom, "humidity_value") == "61RH");
    CHECK(text_of(plain, "humidity_value") == "61%");
    CHECK(text_of(hidden, "humidity_value") == "61");

    // The reading follows the sensor, and a runtime edit reaches only its tile.
    lv_subject_set_int(humidity_subject(), 455);
    process_lvgl(50);
    CHECK(text_of(custom, "humidity_value") == "45RH");
    custom.widget().set_unit_override(std::string("pct"));
    CHECK(text_of(custom, "humidity_value") == "45pct");
    CHECK(text_of(plain, "humidity_value") == "45%");
    custom.widget().set_unit_override(std::nullopt);
    CHECK(text_of(custom, "humidity_value") == "45%");

    // No sensor: the tile shows no reading, whatever the unit.
    lv_subject_set_int(humidity_subject(), -1);
    process_lvgl(50);
    CHECK(text_of(plain, "humidity_value") == "--");
    CHECK(text_of(hidden, "humidity_value") == "--");
}

TEST_CASE_METHOD(LVGLUITestFixture, "unit tile: a detached tile stops following its sensor",
                 "[unit_tile]") {
    lv_subject_set_int(humidity_subject(), 612);
    HumidityTile tile(test_screen(), config_with(nullptr), "humidity:1",
                      UnitTileWidget::humidity_spec());
    tile.create_tile(test_screen());
    REQUIRE(text_of(tile, "humidity_value") == "61%");
    tile.widget().detach_tile();
    lv_subject_set_int(humidity_subject(), 455);
    process_lvgl(50);
    CHECK(text_of(tile, "humidity_value") == "61%");
}

TEST_CASE_METHOD(LVGLUITestFixture, "unit tile: width unit override replaces the default unit",
                 "[unit_tile][unit_override]") {
    lv_subject_set_int(width_subject(), 1750);
    const auto& spec = UnitTileWidget::width_spec();
    HumidityTile custom(test_screen(), config_with("in"), "width_sensor:1", spec);
    HumidityTile plain(test_screen(), config_with(nullptr), "width_sensor:2", spec);
    HumidityTile hidden(test_screen(), config_with(""), "width_sensor:3", spec);
    custom.create_tile(test_screen());
    plain.create_tile(test_screen());
    hidden.create_tile(test_screen());

    CHECK(text_of(custom, "width_value") == "1.75 in");
    CHECK(text_of(plain, "width_value") == "1.75 mm");
    CHECK(text_of(hidden, "width_value") == "1.75");
}

// The override lives in the instance's stored config, and only on the tile it
// was set on.
TEST_CASE_METHOD(LVGLUITestFixture, "unit tile: unit override persists per instance",
                 "[unit_tile][unit_override]") {
    using helix::test::ScopedHomeLayout;
    const auto placed = [](const char* id, int col) {
        return json{{"id", id}, {"enabled", true}, {"col", col}, {"row", 0}};
    };
    ScopedHomeLayout layout(json::array({placed("humidity", 0), placed("width_sensor", 2)}));
    auto& mgr = PanelWidgetManager::instance();
    const auto reload_stored = [&](const char* id) {
        mgr.clear_panel_config("home");
        mgr.get_widget_config("home").mark_dirty();
        return mgr.get_widget_config("home").get_widget_config(id);
    };

    {
        UnitTileWidget humidity("humidity", UnitTileWidget::humidity_spec());
        humidity.set_panel_id("home");
        humidity.set_config(json::object());
        humidity.set_unit_override(std::string("RH"));
    }
    CHECK(reload_stored("humidity").value("unit_override", std::string("<none>")) == "RH");
    CHECK_FALSE(reload_stored("width_sensor").contains("unit_override"));

    UnitTileWidget restarted("humidity", UnitTileWidget::humidity_spec());
    restarted.set_panel_id("home");
    restarted.set_config(reload_stored("humidity"));
    REQUIRE(restarted.unit_override().has_value());
    CHECK(*restarted.unit_override() == "RH");

    restarted.set_unit_override(std::nullopt);
    CHECK_FALSE(reload_stored("humidity").contains("unit_override"));
}

// The configure card shows the unit; its row opens a text dialog whose answer
// becomes the override, and Default drops it.
TEST_CASE_METHOD(LVGLUITestFixture, "unit tile: configure card edits the unit",
                 "[unit_tile][unit_override]") {
    helix::ui::ContextMenu::register_shared_callbacks();
    UnitTileWidget::register_xml();
    lv_subject_set_int(humidity_subject(), 612);
    UnitTileWidget widget("humidity", UnitTileWidget::humidity_spec());
    widget.set_config(json::object());
    lv_obj_t* tile = lv_obj_create(test_screen());
    widget.attach_tile(tile, test_screen());

    REQUIRE(widget.has_edit_configure());
    widget.on_edit_configure();
    lv_obj_t* card = lv_obj_find_by_name(test_screen(), "context_menu");
    REQUIRE(card != nullptr);
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(card, "unit_value"))) == "%");
    CHECK(lv_obj_is_hidden(lv_obj_find_by_name(card, "unit_reset")));

    lv_obj_send_event(lv_obj_find_by_name(card, "unit_row"), LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);
    CHECK(lv_obj_find_by_name(test_screen(), "context_menu") == nullptr);
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    lv_obj_t* input = lv_obj_find_by_name(dialog, "text_entry_input");
    REQUIRE(input != nullptr);
    lv_textarea_set_text(input, "RH");
    lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);
    REQUIRE(widget.unit_override().has_value());
    CHECK(*widget.unit_override() == "RH");

    widget.on_edit_configure();
    card = lv_obj_find_by_name(test_screen(), "context_menu");
    REQUIRE(card != nullptr);
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(card, "unit_value"))) == "RH");
    lv_obj_t* reset = lv_obj_find_by_name(card, "unit_reset");
    REQUIRE_FALSE(lv_obj_is_hidden(reset));
    lv_obj_send_event(reset, LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(widget.unit_override().has_value());
    CHECK(lv_obj_is_hidden(reset));
    widget.detach_tile();
}
