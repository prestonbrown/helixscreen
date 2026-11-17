// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_spoolman_context_menu.h"

#include "../test_fixtures.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "plugin_host.h"
#include "spoolman_types.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::plugin::test;

namespace {

struct SpoolPlugin {
    TempDir tmp;
    std::unique_ptr<HostRig> rig;

    SpoolPlugin() {
        std::filesystem::create_directories(tmp.path / "spool-plug");
        std::ofstream(tmp.path / "spool-plug" / "manifest.json")
            << R"({"id":"spool-plug","name":"Spool Plug","version":"1.0.0","menu_items":[
                 {"id":"find","menu":"spool","label":"Find in inventory","handler":"find"}]})";
        std::ofstream(tmp.path / "spool-plug" / "main.lua") << R"(
            helix.ui.on("find", function(ctx)
                seen = string.format("%s|%s|%s|%s", tostring(ctx.spool_id), tostring(ctx.material),
                                     tostring(ctx.color), tostring(ctx.remaining_g))
            end)
        )";
        rig = std::make_unique<HostRig>(enabled("spool-plug", {}));
        rig->host->load_from(tmp.path.string());
    }

    std::string seen() {
        lua_State* L = rig->host->runtime("spool-plug")->state();
        lua_getglobal(L, "seen");
        std::string s = lua_isnil(L, -1) ? "nil" : lua_tostring(L, -1);
        lua_pop(L, 1);
        return s;
    }
};

SpoolInfo test_spool() {
    SpoolInfo s;
    s.id = 31;
    s.vendor = "Hatchbox";
    s.material = "PETG";
    s.color_hex = "#1A1A2E";
    s.remaining_weight_g = 0;
    return s;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "spool menu appends plugin rows with a heading",
                 "[spoolman][context_menu][plugin]") {
    REQUIRE(register_component("spoolman_context_menu"));
    SpoolPlugin plugin;
    helix::ui::SpoolmanContextMenu menu;
    REQUIRE(menu.show_for_spool(test_screen(), test_spool(), test_screen(), true));
    lv_obj_t* rows = lv_obj_find_by_name(test_screen(), "plugin_rows");
    REQUIRE(rows);
    lv_obj_t* heading = lv_obj_find_by_name(rows, "plugin_name");
    REQUIRE(heading);
    CHECK(std::string(lv_label_get_text(heading)) == "Spool Plug");
    lv_obj_t* label = lv_obj_find_by_name(rows, "item_label");
    REQUIRE(label);
    CHECK(std::string(lv_label_get_text(label)) == "Find in inventory");
    // After the app's own last item.
    lv_obj_t* card = lv_obj_get_parent(rows);
    CHECK(lv_obj_get_index(rows) == static_cast<int32_t>(lv_obj_get_child_count(card)) - 1);

    lv_obj_send_event(lv_obj_get_parent(lv_obj_get_parent(label)), LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(menu.is_visible());
    CHECK(plugin.seen() == "31|PETG|#1A1A2E|nil");
    process_lvgl(20);
}

TEST_CASE_METHOD(XMLTestFixture, "spool menu: a disabled plugin adds nothing",
                 "[spoolman][context_menu][plugin]") {
    REQUIRE(register_component("spoolman_context_menu"));
    SpoolPlugin plugin;
    plugin.rig->host->disable("spool-plug");
    helix::ui::SpoolmanContextMenu menu;
    REQUIRE(menu.show_for_spool(test_screen(), test_spool(), test_screen(), true));
    lv_obj_t* rows = lv_obj_find_by_name(test_screen(), "plugin_rows");
    REQUIRE(rows);
    CHECK(lv_obj_get_child_count(rows) == 0);
    menu.hide();
    process_lvgl(20);
}

TEST_CASE_METHOD(XMLTestFixture, "spool menu shows the price in Spoolman's currency",
                 "[spoolman][context_menu][price]") {
    REQUIRE(register_component("spoolman_context_menu"));
    helix::ui::SpoolmanContextMenu menu;
    SpoolInfo spool = test_spool();
    spool.price = 24.5;
    REQUIRE(menu.show_for_spool(test_screen(), spool, test_screen(), true, "EUR"));
    lv_obj_t* price = lv_obj_find_by_name(test_screen(), "spool_price_label");
    REQUIRE(price);
    CHECK_FALSE(lv_obj_is_hidden(price));
    CHECK(std::string(lv_label_get_text(price)) == "Price: \xE2\x82\xAC"
                                                   "24.50");
    menu.hide();
    process_lvgl(20);
}

TEST_CASE_METHOD(XMLTestFixture, "spool menu hides the price line when there is no price",
                 "[spoolman][context_menu][price]") {
    REQUIRE(register_component("spoolman_context_menu"));
    helix::ui::SpoolmanContextMenu menu;
    SpoolInfo spool = test_spool();
    spool.price = 0;
    spool.filament_price = 0;
    REQUIRE(menu.show_for_spool(test_screen(), spool, test_screen(), true, "EUR"));
    lv_obj_t* price = lv_obj_find_by_name(test_screen(), "spool_price_label");
    REQUIRE(price);
    CHECK(lv_obj_is_hidden(price));
    menu.hide();
    process_lvgl(20);
}

#endif // HELIX_HAS_PLUGINS
