// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_xml_policy.h"

#include "../catch_amalgamated.hpp"

using helix::plugin::check_plugin_attr;
using helix::plugin::check_plugin_xml;

namespace {
std::string view_with(const std::string& body) {
    return "<component><view extends=\"lv_obj\">" + body + "</view></component>";
}
} // namespace

TEST_CASE("plugin XML may use plugin_event and its own subjects", "[plugin][xml_policy]") {
    CHECK(check_plugin_xml("ab", {}, view_with(R"(<lv_label bind_text="ab__status"/>
        <lv_button><event_cb trigger="clicked" callback="plugin_event" user_data="ab__go:3"/></lv_button>
        <lv_obj><bind_flag_if_eq subject="ab__busy" flag="hidden" ref_value="0"/></lv_obj>)"))
              .empty());
}

TEST_CASE("plugin XML may not name app callbacks or subjects", "[plugin][xml_policy]") {
    CHECK_FALSE(
        check_plugin_xml(
            "ab", {},
            view_with(
                R"(<lv_button><event_cb trigger="clicked" callback="on_estop_clicked"/></lv_button>)"))
            .empty());
    CHECK_FALSE(
        check_plugin_xml("ab", {}, view_with(R"(<text_input clear_callback="on_wifi_clear"/>)"))
            .empty());
    CHECK_FALSE(
        check_plugin_xml("ab", {}, view_with(R"(<lv_slider bind_value="extruder_target"/>)"))
            .empty());
    CHECK_FALSE(
        check_plugin_xml(
            "ab", {},
            view_with(
                R"(<lv_obj><bind_flag_if_eq subject="printer_connected" flag="hidden" ref_value="0"/></lv_obj>)"))
            .empty());
    CHECK_FALSE(
        check_plugin_xml(
            "ab", {},
            view_with(
                R"(<lv_button><event_cb trigger="clicked" callback="plugin_event" user_data="other__go"/></lv_button>)"))
            .empty());
    CHECK_FALSE(
        check_plugin_xml("ab", {}, view_with(R"(<lv_label bind_text="$status"/>)")).empty());
    // Ownership would reject "$status" too; the message is what pins the prop rule itself.
    CHECK(check_plugin_xml("ab", {}, view_with(R"(<lv_label bind_text="$status"/>)"))
              .find("through a prop") != std::string::npos);
    CHECK_FALSE(check_plugin_xml("ab", {},
                                 "<component><subjects><int name=\"x\" value=\"0\"/></subjects>"
                                 "<view extends=\"lv_obj\"/></component>")
                    .empty());
    CHECK_FALSE(check_plugin_xml("ab", {}, "not xml <").empty());
}

TEST_CASE("both spellings of an lv_obj child are checked the same way", "[plugin][xml_policy]") {
    CHECK(
        check_plugin_xml(
            "ab", {},
            view_with(
                R"(<lv_obj-event_cb trigger="clicked" callback="plugin_event" user_data="ab__go"/>)"))
            .empty());
    CHECK_FALSE(
        check_plugin_xml(
            "ab", {},
            view_with(
                R"(<lv_obj-event_cb trigger="clicked" callback="plugin_event" user_data="other__go"/>)"))
            .empty());
    CHECK_FALSE(check_plugin_xml("ab", {}, view_with(R"(<screen_load_event/>)")).empty());
    CHECK_FALSE(check_plugin_xml("ab", {}, view_with(R"(<lv_obj-screen_load_event/>)")).empty());
    CHECK_FALSE(check_plugin_xml("ab", {}, view_with(R"(<lv_obj-screen_create_event/>)")).empty());
}

TEST_CASE("plugin XML may use only its own components and the app allowlist",
          "[plugin][xml_policy]") {
    // App components declare subject and callback props under free names, so any
    // component off the allowlist could smuggle one past an attribute rule.
    CHECK_FALSE(check_plugin_xml("ab", {},
                                 view_with(R"(<setting_toggle_row subject="printer_connected"/>)"))
                    .empty());
    CHECK_FALSE(
        check_plugin_xml("ab", {}, "<component><view extends=\"temp_display\"/></component>")
            .empty());
    CHECK(check_plugin_xml("ab", {}, "<component><view extends=\"overlay_panel\"/></component>")
              .empty());
    // A component this plugin is registering is usable, and nothing else that merely
    // looks owned is.
    CHECK(
        check_plugin_xml("ab", {"ab__tile"}, "<component><view extends=\"ab__tile\"/></component>")
            .empty());
    CHECK(check_plugin_xml("ab", {"ab__tile"}, view_with(R"(<ab__tile/>)")).empty());
    CHECK_FALSE(
        check_plugin_xml("ab", {"ab__tile"}, "<component><view extends=\"ab__panel\"/></component>")
            .empty());
    CHECK(check_plugin_xml("ab", {}, view_with(R"(<overlay_panel title="Demo"/>)")).empty());
}

TEST_CASE("plugin XML may use the app's icon, text and card widgets", "[plugin][xml_policy]") {
    for (const char* el : {"icon", "ui_card", "text_heading", "text_body", "text_muted",
                           "text_small", "text_xs", "text_tiny"}) {
        std::string xml = view_with(std::string("<") + el + " name=\"demo__x\"/>");
        CHECK(check_plugin_xml("demo", {"demo__w"}, xml) == std::string());
    }
    CHECK(check_plugin_xml("demo", {"demo__w"}, view_with("<text_button/>")) != std::string());
}

TEST_CASE("app widgets keep the name rules", "[plugin][xml_policy]") {
    // An unowned object name, an unowned bind_text subject, and a foreign callback are
    // each rejected on icon and on text_body exactly as on lv_label.
    const char* bad[] = {
        "<icon name=\"other__x\"/>",
        "<text_body bind_text=\"extruder_temp\"/>",
        "<icon><event_cb trigger=\"clicked\" callback=\"settings_open\"/></icon>",
    };
    for (const char* inner : bad) {
        CHECK(check_plugin_xml("demo", {"demo__w"}, view_with(inner)) != std::string());
    }
}

TEST_CASE("plugin_canvas is available and keeps the name rule", "[plugin][xml_policy]") {
    CHECK(check_plugin_xml("p", {}, view_with(R"(<plugin_canvas name="p__c"/>)")).empty());
    std::string why = check_plugin_xml("p", {}, view_with(R"(<plugin_canvas name="c"/>)"));
    CHECK_FALSE(why.empty());
    CHECK(why.find("object names must be p__<name>") != std::string::npos);
}

TEST_CASE("plugin XML fonts are responsive base tokens", "[plugin][xml_policy]") {
    CHECK(
        check_plugin_xml("p", {}, view_with(R"(<lv_label text="v" style_text_font="#font_body"/>)"))
            .empty());
    // font_small is the base token named "small", not a suffixed variant.
    CHECK(check_plugin_xml("p", {},
                           view_with(R"(<lv_label text="v" style_text_font="#font_small"/>)"))
              .empty());
    std::string why = check_plugin_xml(
        "p", {}, view_with(R"(<lv_label text="v" style_text_font="#font_heading_large"/>)"));
    CHECK_FALSE(why.empty());
    CHECK(why.find("base font tokens") != std::string::npos);
    // Values outside the #font_ vocabulary are the XML engine's business.
    CHECK(check_plugin_xml("p", {}, view_with(R"(<lv_label text="v" style_text_font="inches"/>)"))
              .empty());
}

TEST_CASE("a plugin id that prefixes an app name owns nothing of it", "[plugin][xml_policy]") {
    CHECK_FALSE(check_plugin_xml("ams", {}, view_with(R"(<ams_device_operations/>)")).empty());
    CHECK_FALSE(check_plugin_xml("ams", {},
                                 "<component><view extends=\"ams_device_operations\"/></component>")
                    .empty());
    CHECK_FALSE(
        check_plugin_xml("extruder", {}, view_with(R"(<lv_slider bind_value="extruder_target"/>)"))
            .empty());
    CHECK_FALSE(
        check_plugin_xml("settings", {},
                         view_with(R"(<lv_switch bind_checked="settings_animations_enabled"/>)"))
            .empty());
}

TEST_CASE("cond expressions may name only plugin-owned subjects", "[plugin][xml_policy]") {
    CHECK_FALSE(
        check_plugin_xml(
            "ab", {}, view_with(R"(<bind_flag_if cond="printer_connected == 1" flag="hidden"/>)"))
            .empty());
    CHECK_FALSE(
        check_plugin_xml(
            "ab", {},
            view_with(R"(<overlay_panel action_button_2_hidden_cond="not printer_connected"/>)"))
            .empty());
    CHECK(
        check_plugin_xml("ab", {}, view_with(R"(<bind_flag_if cond="ab__x == 1" flag="hidden"/>)"))
            .empty());
    CHECK(check_plugin_xml("ab", {},
                           view_with(R"x(<bind_flag_if cond="ab__x or ab__y and not (1 + 2 >= 3)"
                                     flag="hidden"/>)x"))
              .empty());
    // '.' is not in the expression grammar, so an app member access fails closed.
    CHECK_FALSE(
        check_plugin_xml("ab", {}, view_with(R"(<bind_flag_if cond="ab.x == 1" flag="hidden"/>)"))
            .empty());
    CHECK(
        check_plugin_xml("ab", {}, view_with(R"(<bind_flag_if cond="3" flag="hidden"/>)")).empty());
}

TEST_CASE("check_plugin_attr is the one attribute rule", "[plugin][xml_policy]") {
    CHECK(check_plugin_attr("ab", "callback", "on_estop_clicked").has_value());
    CHECK(check_plugin_attr("ab", "callback", "plugin_event") == std::nullopt);
    CHECK(check_plugin_attr("ab", "bind_value", "extruder_target").has_value());
    CHECK(check_plugin_attr("ab", "bind_value", "ab__target") == std::nullopt);
    CHECK(check_plugin_attr("ab", "user_data", "hello__go:1").has_value());
    CHECK(check_plugin_attr("ab", "user_data", "ab__go:1") == std::nullopt);
    CHECK(check_plugin_attr("ab", "action_button_2_hidden_cond", "printer_connected").has_value());
    CHECK(check_plugin_attr("ab", "hidden_cond", "ab__ready or 1") == std::nullopt);
    CHECK(check_plugin_attr("ab", "bind_text", "$title").has_value());
    // Attributes that carry none of these roles are not the policy's business.
    CHECK(check_plugin_attr("ab", "width", "200") == std::nullopt);
    CHECK(check_plugin_attr("ab", "title", "anything") == std::nullopt);
}

TEST_CASE("plugin XML may not write subjects through subject_*_event elements",
          "[plugin][xml_policy]") {
    // A subject_*_event stores a raw subject pointer as event user_data and adds no
    // observer, so the writing object can outlive a subject freed at unload. A plugin
    // reaches the same effect safely through plugin_event plus s:set in Lua.
    const char* forms[] = {
        R"(<lv_obj-subject_set_int_event subject="ab__n" value="1"/>)",
        R"(<lv_obj-subject_set_float_event subject="ab__n" value="1"/>)",
        R"(<lv_obj-subject_set_string_event subject="ab__s" value="x"/>)",
        R"(<lv_obj-subject_increment_event subject="ab__n"/>)",
        R"(<lv_obj-subject_toggle_event subject="ab__flag"/>)",
        R"(<subject_toggle_event subject="ab__flag"/>)",
    };
    for (const char* f : forms)
        CHECK_FALSE(check_plugin_xml("ab", {}, view_with(f)).empty());
}

TEST_CASE("plugin object names are plugin-owned", "[plugin][xml_policy]") {
    // Screen-wide lookups by name (modal_dialog, rename inputs) must never resolve
    // to a plugin object.
    CHECK_FALSE(check_plugin_xml("ab", {}, view_with(R"(<lv_obj name="modal_dialog"/>)")).empty());
    CHECK_FALSE(
        check_plugin_xml("ab", {}, view_with(R"(<lv_textarea name="rename_input"/>)")).empty());
    CHECK(check_plugin_xml("ab", {}, view_with(R"(<lv_label name="ab__status"/>)")).empty());
    CHECK(check_plugin_xml("ab", {}, view_with(R"(<lv_label/>)")).empty());
    // A prop or a style reference names no object.
    CHECK(check_plugin_xml("ab", {},
                           "<component><api><prop name=\"title\" type=\"string\"/></api>"
                           "<view extends=\"lv_obj\"/></component>")
              .empty());
    CHECK(check_plugin_xml("ab", {}, view_with(R"(<lv_obj-style name="card"/>)")).empty());
    // bind_style and bind_style_if_* name the style they toggle.
    CHECK(check_plugin_xml("ab", {},
                           view_with(R"(<bind_style name="card" subject="ab__on" ref_value="1"/>)"))
              .empty());
    CHECK(check_plugin_xml(
              "ab", {},
              view_with(R"(<lv_obj-bind_style_if_eq name="card" subject="ab__on" ref_value="1"/>)"))
              .empty());
    CHECK(check_plugin_attr("ab", "name", "modal_dialog").has_value());
    CHECK_FALSE(check_plugin_attr("ab", "name", "ab__root").has_value());
}

#endif // HELIX_HAS_PLUGINS
