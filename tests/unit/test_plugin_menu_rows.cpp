// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_plugin_items_menu.h"
#include "ui_plugin_menu_rows.h"

#include "../test_fixtures.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "helix/ui/text_metrics.h"
#include "plugin_host.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;
namespace pm = helix::plugin_menu;

namespace {

void write_plugin(const std::filesystem::path& root, const std::string& id, const std::string& name,
                  const std::string& items, const std::string& lua) {
    std::filesystem::create_directories(root / id);
    std::ofstream(root / id / "manifest.json")
        << R"({"id":")" << id << R"(","name":")" << name << R"(","version":"1.0.0","menu_items":[)"
        << items << "]}";
    std::ofstream(root / id / "main.lua") << lua;
}

constexpr const char* kItems =
    R"({"id":"go","menu":"print_file","label":"Go now","icon":"tune","handler":"go"},
       {"id":"later","menu":"print_file","label":"Later","handler":"go","check":"grey"})";

constexpr const char* kLua = R"(
    runs = 0
    helix.ui.on("go", function(ctx) runs = runs + 1; seen_path = ctx.path end)
    helix.ui.on("grey", function() return {visible = true, enabled = false, reason = "Busy"} end)
    helix.ui.on("never", function() return false end)
)";

/// A menu that appends plugin rows under its own (empty) app rows, as the slot and
/// spool menus do.
class RowsTestMenu : public helix::ui::ContextMenu {
    HELIX_CONTEXT_MENU_KIND(RowsTestMenu)

  public:
    pm::json context;
    int added = -1;

  protected:
    const char* xml_component_name() const override {
        return "plugin_items_menu";
    }
    CardWidth card_width() const override {
        return {40, 200, 400};
    }
    void on_created(lv_obj_t* backdrop) override {
        added =
            helix::ui::append_plugin_rows(*this, lv_obj_find_by_name(backdrop, "plugin_rows"),
                                          pm::resolve(pm::MenuKind::PrintFile, context), context);
    }
};

std::vector<lv_obj_t*> children_named(lv_obj_t* container, const char* inner) {
    std::vector<lv_obj_t*> out;
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
        lv_obj_t* c = lv_obj_get_child(container, static_cast<int32_t>(i));
        if (lv_obj_find_by_name(c, inner))
            out.push_back(c);
    }
    return out;
}

std::string label_text(lv_obj_t* row, const char* name) {
    lv_obj_t* l = lv_obj_find_by_name(row, name);
    return l ? lv_label_get_text(l) : "";
}

long lua_global_int(PluginHost& host, const char* id, const char* name) {
    lua_State* L = host.runtime(id)->state();
    lua_getglobal(L, name);
    long v = static_cast<long>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return v;
}

struct Rig {
    TempDir tmp;
    HostRig host{enabled("a-plug", {})};
    Rig() {
        write_plugin(tmp.path, "a-plug", "Ay Plugin", kItems, kLua);
        host.host->load_from(tmp.path.string());
    }
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "plugin rows appear with heading and label",
                 "[ui][context_menu][plugin]") {
    REQUIRE(register_component("plugin_items_menu"));
    Rig rig;
    RowsTestMenu menu;
    menu.context = {{"path", "a.gcode"}};
    REQUIRE(menu.show_near_widget(test_screen(), 0, test_screen()));
    CHECK(menu.added == 2);
    lv_obj_t* rows = lv_obj_find_by_name(test_screen(), "plugin_rows");
    REQUIRE(rows);
    auto headings = children_named(rows, "plugin_name");
    REQUIRE(headings.size() == 1);
    CHECK(label_text(headings[0], "plugin_name") == "Ay Plugin");
    auto items = children_named(rows, "item_label");
    REQUIRE(items.size() == 2);
    CHECK(label_text(items[0], "item_label") == "Go now");
    CHECK_FALSE(lv_obj_has_state(items[0], LV_STATE_DISABLED));
    CHECK(lv_obj_is_hidden(lv_obj_find_by_name(items[0], "item_reason")));
    // The divider separates plugin rows from the app's own.
    CHECK(lv_obj_get_child_count(rows) == 4);

    CHECK(lv_obj_has_state(items[1], LV_STATE_DISABLED));
    lv_obj_t* reason = lv_obj_find_by_name(items[1], "item_reason");
    CHECK(std::string(lv_label_get_text(reason)) == "Busy");
    CHECK_FALSE(lv_obj_is_hidden(reason));
    menu.hide();
}

TEST_CASE_METHOD(XMLTestFixture, "tapping a plugin row runs its handler and hides the menu",
                 "[ui][context_menu][plugin]") {
    REQUIRE(register_component("plugin_items_menu"));
    Rig rig;
    RowsTestMenu menu;
    menu.context = {{"path", "boats/benchy.gcode"}};
    REQUIRE(menu.show_near_widget(test_screen(), 0, test_screen()));
    auto items = children_named(lv_obj_find_by_name(test_screen(), "plugin_rows"), "item_label");
    REQUIRE(items.size() == 2);

    lv_obj_send_event(items[1], LV_EVENT_CLICKED, nullptr); // disabled: nothing
    CHECK(menu.is_visible());
    CHECK(lua_global_int(*rig.host.host, "a-plug", "runs") == 0);

    lv_obj_send_event(items[0], LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(menu.is_visible());
    CHECK(lua_global_int(*rig.host.host, "a-plug", "runs") == 1);
    lua_State* L = rig.host.host->runtime("a-plug")->state();
    lua_getglobal(L, "seen_path");
    CHECK(std::string(lua_tostring(L, -1)) == "boats/benchy.gcode");
    lua_pop(L, 1);
    process_lvgl(50);
}

TEST_CASE_METHOD(XMLTestFixture, "a tap after the plugin went away runs nothing",
                 "[ui][context_menu][plugin]") {
    REQUIRE(register_component("plugin_items_menu"));
    Rig rig;
    RowsTestMenu menu;
    REQUIRE(menu.show_near_widget(test_screen(), 0, test_screen()));
    auto items = children_named(lv_obj_find_by_name(test_screen(), "plugin_rows"), "item_label");
    REQUIRE(items.size() == 2);
    rig.host.host->disable("a-plug");
    REQUIRE(rig.host.host->enable("a-plug"));
    lv_obj_send_event(items[0], LV_EVENT_CLICKED, nullptr);
    CHECK(lua_global_int(*rig.host.host, "a-plug", "runs") == 0);
    process_lvgl(50);
}

TEST_CASE_METHOD(XMLTestFixture, "plugin items menu says so when nothing is visible",
                 "[ui][context_menu][plugin]") {
    REQUIRE(register_component("plugin_items_menu"));
    TempDir tmp;
    write_plugin(tmp.path, "a-plug", "Ay Plugin",
                 R"({"id":"x","menu":"print_file","label":"X","handler":"go","check":"never"},
                    {"id":"t","menu":"tile","label":"T","handler":"go","check":"never"})",
                 kLua);
    // A tile item needs a widget declared; this plugin has none, so only the file item loads.
    write_plugin(tmp.path, "b-plug", "Bee",
                 R"({"id":"x","menu":"print_file","label":"X","handler":"go","check":"never"})",
                 kLua);
    HostRig host(enabled_all({{"b-plug", {}}}));
    host.host->load_from(tmp.path.string());

    helix::ui::PluginItemsMenu menu;
    REQUIRE(
        menu.show_for(test_screen(), test_screen(), pm::MenuKind::PrintFile, pm::json::object()));
    lv_obj_t* rows = lv_obj_find_by_name(test_screen(), "plugin_rows");
    REQUIRE(rows);
    CHECK(children_named(rows, "item_label").empty());
    lv_obj_t* empty = lv_obj_find_by_name(rows, "empty_text");
    REQUIRE(empty);
    CHECK(std::string(lv_label_get_text(empty)) == "No plugin actions for this file");
    menu.hide();
    process_lvgl(50);

    REQUIRE(menu.show_for(test_screen(), test_screen(), pm::MenuKind::Tile, pm::json::object(),
                          "b-plug"));
    empty = lv_obj_find_by_name(lv_obj_find_by_name(test_screen(), "plugin_rows"), "empty_text");
    REQUIRE(empty);
    CHECK(std::string(lv_label_get_text(empty)) == "No actions right now");
    menu.hide();
    process_lvgl(50);
}

TEST_CASE_METHOD(XMLTestFixture, "plugin items menu lists one plugin's items without a divider",
                 "[ui][context_menu][plugin]") {
    REQUIRE(register_component("plugin_items_menu"));
    Rig rig;
    helix::ui::PluginItemsMenu menu;
    REQUIRE(menu.show_for(test_screen(), test_screen(), pm::MenuKind::PrintFile, pm::json::object(),
                          "a-plug"));
    lv_obj_t* rows = lv_obj_find_by_name(test_screen(), "plugin_rows");
    CHECK(lv_obj_get_child_count(rows) == 3); // heading + two items
    CHECK(lv_obj_find_by_name(rows, "empty_text") == nullptr);
    menu.hide();
    process_lvgl(50);
}

TEST_CASE_METHOD(XMLTestFixture, "plugin items menu widens to show a long label and its reason",
                 "[ui][context_menu][plugin]") {
    REQUIRE(register_component("plugin_items_menu"));
    TempDir tmp;
    write_plugin(tmp.path, "c-plug", "Calibration Bridge",
                 R"({"id":"c","menu":"print_file","label":"Calibrate with this file","icon":"tune",
                     "handler":"go","check":"why"})",
                 R"(helix.ui.on("go", function() end)
                    helix.ui.on("why", function()
                        return {visible = true, enabled = false,
                                reason = "Start a calibration in the slicer first"}
                    end))");
    HostRig host(enabled_all({{"c-plug", {}}}));
    host.host->load_from(tmp.path.string());

    helix::ui::PluginItemsMenu menu;
    REQUIRE(
        menu.show_for(test_screen(), test_screen(), pm::MenuKind::PrintFile, pm::json::object()));
    lv_obj_t* rows = lv_obj_find_by_name(test_screen(), "plugin_rows");
    REQUIRE(rows);
    lv_obj_update_layout(lv_obj_get_screen(rows));
    // A dotted label's own text carries the dots, so each is measured from what was set.
    const std::pair<const char*, const char*> set[] = {
        {"item_label", "Calibrate with this file"},
        {"item_reason", "Start a calibration in the slicer first"}};
    for (const auto& [name, text] : set) {
        lv_obj_t* l = lv_obj_find_by_name(rows, name);
        REQUIRE(l);
        REQUIRE_FALSE(lv_obj_is_hidden(l));
        const int32_t need =
            helix::ui::text_width(text, lv_obj_get_style_text_font(l, LV_PART_MAIN),
                                  lv_obj_get_style_text_letter_space(l, LV_PART_MAIN));
        CAPTURE(name, lv_obj_get_width(l), need);
        // 800x480 has the room: the card fits the text on one line, not dotted or wrapped.
        CHECK(lv_obj_get_width(l) >= need);
    }
    menu.hide();
    process_lvgl(50);
}

#endif // HELIX_HAS_PLUGINS
