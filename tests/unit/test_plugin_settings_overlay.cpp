// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_nav_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "../test_helpers/plugin_test_support.h"
#include "config.h"
#include "display_settings_manager.h"
#include "lua_runtime.h"
#include "plugin_settings_overlay.h"

#include <filesystem>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {

/// The attribute value `name`, or empty. Local helper for the mapping checks.
std::string attr(const SettingRowSpec& spec, const std::string& name) {
    for (const auto& [k, v] : spec.attrs)
        if (k == name)
            return v;
    return "";
}

/// LVGLUITestFixture plus a deterministic navigation seed: animations off so the
/// go_back close path runs inline, and panel_stack_[0] holding a stand-in base panel.
class SettingsFx : public LVGLUITestFixture {
  public:
    std::array<lv_obj_t*, kUiPanelCount> panels{};

    SettingsFx() {
        DisplaySettingsManager::instance().set_animations_enabled(false);
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
    }
    ~SettingsFx() override {
        drain();
        process_lvgl(100); // deferred overlay deletes
        DisplaySettingsManager::instance().set_animations_enabled(true);
    }
};

/// Reads a Lua global back as an integer.
long long int_global(LuaRuntime* rt, const char* name) {
    lua_getglobal(rt->state(), name);
    long long v = lua_tointeger(rt->state(), -1);
    lua_pop(rt->state(), 1);
    return v;
}

/// The widget inside `row` that carries the row's value ("toggle", "slider",
/// "dropdown" or "value_input").
lv_obj_t* value_widget(lv_obj_t* row, const char* name) {
    return lv_obj_find_by_name(row, name);
}

} // namespace

TEST_CASE("each setting type maps to its row", "[plugin][plugin_settings]") {
    SettingDecl b;
    b.key = "on";
    b.label = "On";
    b.type = SettingType::Bool;
    CHECK(setting_row_spec("p", b, true).component == "setting_toggle_row");
    CHECK(attr(setting_row_spec("p", b, true), "callback") == "plugin_setting_changed");
    SettingDecl i;
    i.key = "n";
    i.label = "N";
    i.type = SettingType::Int;
    i.min = 1;
    i.max = 20;
    auto si = setting_row_spec("p", i, 5);
    CHECK(si.component == "setting_slider_row");
    CHECK(attr(si, "min") == "1");
    CHECK(attr(si, "max") == "20");
    CHECK(attr(si, "value") == "5");
    CHECK(attr(si, "trigger") == "released");
    SettingDecl f;
    f.key = "r";
    f.label = "R";
    f.type = SettingType::Float;
    f.min = 0;
    f.max = 1;
    CHECK(setting_row_spec("p", f, 0.5).component == "setting_slider_row");
    CHECK(attr(setting_row_spec("p", f, 0.5), "value") == "50");
    CHECK(attr(setting_row_spec("p", f, 0.5), "min") == "0");
    CHECK(attr(setting_row_spec("p", f, 0.5), "max") == "100");
    SettingDecl e;
    e.key = "m";
    e.label = "M";
    e.type = SettingType::Enum;
    e.options = {"a", "b"};
    CHECK(attr(setting_row_spec("p", e, "b"), "options") == "a\nb");
    SettingDecl s;
    s.key = "s";
    s.label = "S";
    s.type = SettingType::String;
    CHECK(setting_row_spec("p", s, "x").component == "setting_text_row");
    CHECK(attr(setting_row_spec("p", s, "x"), "value") == "x");
    SettingDecl a;
    a.key = "go";
    a.label = "Go";
    a.type = SettingType::Action;
    a.callback = "p_go";
    CHECK(setting_row_spec("p", a, nullptr).component == "setting_action_row");
    CHECK(attr(setting_row_spec("p", a, nullptr), "callback") == "plugin_setting_action");
    SettingDecl n;
    n.key = "st";
    n.label = "St";
    n.type = SettingType::Info;
    n.subject = "p__status";
    CHECK(setting_row_spec("p", n, nullptr).component == "setting_info_row");
    CHECK(attr(setting_row_spec("p", n, nullptr), "bind_value") == "p__status");
}

TEST_CASE_METHOD(SettingsFx, "the generated screen writes settings through the plugin's rule",
                 "[plugin][plugin_settings]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rt->run_string(R"(seen = nil
        helix.settings.on_change("step", function(v) seen = v end))",
                           "t"));
    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    CHECK(rig.host->set_setting("widget-demo", "step", 7));
    CHECK_FALSE(rig.host->set_setting("widget-demo", "step", 99)); // outside [1, 20]
    process_lvgl(600); // the settings write waits for its timer
    CHECK(rig.block["settings"]["widget-demo"]["step"] == json(7));
    CHECK(int_global(rt, "seen") == 7);
}

TEST_CASE_METHOD(SettingsFx, "a released slider row writes its value through the plugin's rule",
                 "[plugin][plugin_settings]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rt->run_string(R"(seen = nil
        helix.settings.on_change("step", function(v) seen = v end))",
                           "t"));
    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    PluginSettingsOverlay* screen = rig.host->settings_screen("widget-demo");
    REQUIRE(screen);
    lv_obj_t* row = screen->row_for("step");
    REQUIRE(row);
    lv_obj_t* slider = value_widget(row, "slider");
    REQUIRE(slider);
    lv_slider_set_value(slider, 7, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
    drain();
    process_lvgl(600); // the settings write waits for its timer
    CHECK(rig.block["settings"]["widget-demo"]["step"] == json(7));
    CHECK(int_global(rt, "seen") == 7);
}

TEST_CASE_METHOD(SettingsFx, "an open screen follows helix.settings.set",
                 "[plugin][plugin_settings]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    PluginSettingsOverlay* screen = rig.host->settings_screen("widget-demo");
    REQUIRE(screen);
    lv_obj_t* slider = value_widget(screen->row_for("step"), "slider");
    REQUIRE(slider);
    REQUIRE(lv_slider_get_value(slider) == 5);

    REQUIRE(rt->run_string(R"(helix.settings.set("step", 12))", "t"));
    CHECK(lv_slider_get_value(slider) == 12);
}

TEST_CASE_METHOD(SettingsFx, "a row shows the value its handler settled on",
                 "[plugin][plugin_settings]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rt->run_string(R"(helix.settings.on_change("step", function(v)
        if v > 10 then helix.settings.set("step", 10) end
    end))",
                           "t"));
    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    PluginSettingsOverlay* screen = rig.host->settings_screen("widget-demo");
    REQUIRE(screen);
    lv_obj_t* slider = value_widget(screen->row_for("step"), "slider");
    REQUIRE(slider);
    lv_slider_set_value(slider, 15, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
    drain();
    CHECK(lv_slider_get_value(slider) == 10);
    // The clamp is the stored state, so releasing at 10 again is not a write.
    lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
    drain();
    CHECK(lv_slider_get_value(slider) == 10);
}

TEST_CASE_METHOD(SettingsFx, "confirming a text row twice fires on_change once",
                 "[plugin][plugin_settings]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rt->run_string(R"(count = 0
        helix.settings.on_change("name", function(v) count = count + 1 end))",
                           "t"));
    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    PluginSettingsOverlay* screen = rig.host->settings_screen("widget-demo");
    REQUIRE(screen);
    lv_obj_t* input = value_widget(screen->row_for("name"), "value_input");
    REQUIRE(input);
    CHECK(std::string(lv_textarea_get_text(input)) == "x"); // default value shown

    // The keyboard confirm path reports ready and then defocused.
    lv_textarea_set_text(input, "y");
    lv_obj_send_event(input, LV_EVENT_READY, nullptr);
    lv_obj_send_event(input, LV_EVENT_DEFOCUSED, nullptr);
    drain();
    process_lvgl(600); // the settings write waits for its timer
    CHECK(rig.block["settings"]["widget-demo"]["name"] == json("y"));
    CHECK(int_global(rt, "count") == 1);
}

TEST_CASE_METHOD(SettingsFx, "an out-of-range text write is refused and the row restored",
                 "[plugin][plugin_settings]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rt->run_string(R"(count = 0
        helix.settings.on_change("name", function(v) count = count + 1 end))",
                           "t"));
    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    PluginSettingsOverlay* screen = rig.host->settings_screen("widget-demo");
    REQUIRE(screen);
    lv_obj_t* input = value_widget(screen->row_for("name"), "value_input");
    REQUIRE(input);

    // Land an accepted value first so the restore has a real stored value.
    lv_textarea_set_text(input, "ok");
    lv_obj_send_event(input, LV_EVENT_READY, nullptr);
    drain();
    process_lvgl(600); // the settings write waits for its timer
    REQUIRE(rig.block["settings"]["widget-demo"]["name"] == json("ok"));

    // kMaxSettingString is 1024; 1100 characters do not fit the declaration.
    lv_textarea_set_text(input, std::string(1100, 'z').c_str());
    lv_obj_send_event(input, LV_EVENT_READY, nullptr);
    drain();
    CHECK(std::string(lv_textarea_get_text(input)) == "ok");
    process_lvgl(600); // the settings write waits for its timer
    CHECK(rig.block["settings"]["widget-demo"]["name"] == json("ok"));
    CHECK(int_global(rt, "count") == 1);
}

TEST_CASE_METHOD(SettingsFx, "an action row runs the manifest's callback through the event path",
                 "[plugin][plugin_settings]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    PluginSettingsOverlay* screen = rig.host->settings_screen("widget-demo");
    REQUIRE(screen);
    lv_obj_t* row = screen->row_for("ping");
    REQUIRE(row);
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    drain();
    // The handler set the plugin's status subject to "pinged"; the info row is
    // bound to it, so the binding is the observable.
    lv_obj_t* info = screen->row_for("state");
    REQUIRE(info);
    lv_obj_t* value = lv_obj_find_by_name(info, "value");
    REQUIRE(value);
    CHECK(std::string(lv_label_get_text(value)) == "pinged");
}

TEST_CASE_METHOD(SettingsFx, "slider rows show the formatted value and follow the slider",
                 "[plugin][plugin_settings]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    PluginSettingsOverlay* screen = rig.host->settings_screen("widget-demo");
    REQUIRE(screen);

    // A Float row's slider carries the value x100; its label must not.
    lv_obj_t* ratio_label = lv_obj_find_by_name(screen->row_for("ratio"), "value_label");
    REQUIRE(ratio_label);
    CHECK(std::string(lv_label_get_text(ratio_label)) == "0.5");

    // An Int row's label follows the drag live, and the write lands on release.
    lv_obj_t* step_row = screen->row_for("step");
    lv_obj_t* slider = value_widget(step_row, "slider");
    lv_obj_t* step_label = lv_obj_find_by_name(step_row, "value_label");
    REQUIRE(slider);
    REQUIRE(step_label);
    CHECK(std::string(lv_label_get_text(step_label)) == "5");
    lv_slider_set_value(slider, 7, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(std::string(lv_label_get_text(step_label)) == "7");
    CHECK_FALSE(rig.block["settings"].contains("widget-demo")); // no write before release
    lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
    drain();
    CHECK(std::string(lv_label_get_text(step_label)) == "7");
    process_lvgl(600); // the settings write waits for its timer
    CHECK(rig.block["settings"]["widget-demo"]["step"] == json(7));
}

TEST_CASE_METHOD(SettingsFx, "toggle and dropdown rows write through the event path",
                 "[plugin][plugin_settings]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    PluginSettingsOverlay* screen = rig.host->settings_screen("widget-demo");
    REQUIRE(screen);

    lv_obj_t* toggle = value_widget(screen->row_for("enabled"), "toggle");
    REQUIRE(toggle);
    lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    drain();
    process_lvgl(600); // the settings write waits for its timer
    CHECK(rig.block["settings"]["widget-demo"]["enabled"] == json(false));

    lv_obj_t* dropdown = value_widget(screen->row_for("mode"), "dropdown");
    REQUIRE(dropdown);
    lv_dropdown_set_selected(dropdown, 1); // "b"
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    drain();
    process_lvgl(600); // the settings write waits for its timer
    CHECK(rig.block["settings"]["widget-demo"]["mode"] == json("b"));
}

TEST_CASE_METHOD(SettingsFx, "a reloaded plugin ignores its old screen's rows",
                 "[plugin][plugin_settings]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    PluginSettingsOverlay* old = rig.host->settings_screen("widget-demo");
    REQUIRE(old);

    // Unload queues the close but the old screen is still up when the plugin
    // comes back under the same id; its rows must not reach the new instance.
    rig.host->disable("widget-demo");
    REQUIRE(rig.host->enable("widget-demo"));
    lv_obj_t* slider = value_widget(old->row_for("step"), "slider");
    REQUIRE(slider);
    lv_slider_set_value(slider, 19, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);

    CHECK_FALSE(rig.block["settings"].contains("widget-demo"));
    drain();
    process_lvgl(500);
    CHECK(rig.host->settings_screen("widget-demo") == nullptr);
}

TEST_CASE_METHOD(SettingsFx, "unloading a plugin closes its open settings screen",
                 "[plugin][plugin_settings]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    REQUIRE(NavigationManager::instance().has_open_overlays());
    REQUIRE(rig.host->settings_screen("widget-demo"));

    rig.host->disable("widget-demo");
    drain(); // close_overlay queues go_back; its body runs on the next queue pass
    process_lvgl(500);
    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
    CHECK(rig.host->settings_screen("widget-demo") == nullptr);
    CHECK(lv_obj_find_by_name(lv_screen_active(), "settings_rows") == nullptr);
}

TEST_CASE_METHOD(SettingsFx, "a navbar tap during the close slide-out still tears the screen down",
                 "[plugin][plugin_settings]") {
    DisplaySettingsManager::instance().set_animations_enabled(true);
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    process_lvgl(500); // slide-in complete
    auto& nav = NavigationManager::instance();
    nav.go_back();
    drain(); // popped; the close callback waits for the slide-out
    NavigationManagerTestAccess::switch_to_panel(nav, helix::PanelId::Controls);
    drain();
    process_lvgl(500);
    drain();
    process_lvgl(100);

    // The host still lives, so only navigation can have torn the screen down.
    CHECK(rig.host->settings_screen("widget-demo") == nullptr);
    CHECK(lv_obj_find_by_name(lv_screen_active(), "settings_rows") == nullptr);
}

TEST_CASE_METHOD(SettingsFx, "a manifest settings_overlay replaces the generated screen",
                 "[plugin][plugin_settings]") {
    HostRig rig(enabled("custom-settings", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("custom-settings")->status == PluginStatus::Loaded);
    REQUIRE(rig.host->open_settings("custom-settings"));
    drain();
    CHECK(NavigationManager::instance().has_open_overlays());
    CHECK(rig.host->overlays().open_count("custom-settings") == 1);
    CHECK(rig.host->settings_screen("custom-settings") == nullptr);
    CHECK(lv_obj_find_by_name(lv_screen_active(), "custom-settings__marker"));
}

TEST_CASE_METHOD(SettingsFx, "open_settings refuses nothing-to-show and unloaded plugins",
                 "[plugin][plugin_settings]") {
    HostRig rig(enabled("shadow", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("shadow")->status == PluginStatus::Loaded);
    drain();
    CHECK_FALSE(rig.host->open_settings("shadow")); // no settings, no overlay
    CHECK_FALSE(rig.host->open_settings("nosuch")); // not loaded
    CHECK_FALSE(rig.host->set_setting("nosuch", "k", json(1)));
    drain();
    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
}

TEST_CASE_METHOD(SettingsFx,
                 "a loaded plugin's settings survive a rescan that shifts the plugin list",
                 "[plugin][plugin_settings][2103]") {
    namespace fs = std::filesystem;
    TempDir tmp;
    for (const char* name : {"hello", "looper", "widget-demo"})
        fs::copy(fs::path("tests/fixtures/plugins") / name, tmp.path / name,
                 fs::copy_options::recursive);
    HostRig rig(enabled_all({{"hello", {"gcode"}}, {"looper", {}}, {"widget-demo", {}}}));
    rig.host->load_from(tmp.path.string());
    REQUIRE(rig.host->runtime("widget-demo"));

    // A plugin that sorts first moves every row after it one slot along.
    fs::copy(fs::path("tests/fixtures/plugins") / "custom-settings", tmp.path / "custom-settings",
             fs::copy_options::recursive);
    rig.host->rescan({"custom-settings"});
    drain();
    REQUIRE(rig.host->plugins().front().dir_name == "custom-settings");

    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    PluginSettingsOverlay* screen = rig.host->settings_screen("widget-demo");
    REQUIRE(screen);
    CHECK(screen->row_for("step") != nullptr);
}

#endif // HELIX_HAS_PLUGINS
