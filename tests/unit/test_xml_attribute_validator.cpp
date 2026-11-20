// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xml_attribute_validator.h"

#include "../catch_amalgamated.hpp"

using namespace xml_validator;

// =============================================================================
// Real parser content from LVGL for realistic testing
// =============================================================================

// Excerpt from lib/lvgl/src/xml/parsers/lv_xml_label_parser.c
static const char* kLabelParserContent = R"(
void lv_xml_label_apply(lv_xml_parser_state_t * state, const char ** attrs)
{
    void * item = lv_xml_state_get_item(state);

    lv_xml_obj_apply(state, attrs); /*Apply the common properties, e.g. width, height, styles flags etc*/

    for(int i = 0; attrs[i]; i += 2) {
        const char * name = attrs[i];
        const char * value = attrs[i + 1];

        if(lv_streq("text", name)) lv_label_set_text(item, value);
        else if(lv_streq("long_mode", name)) lv_label_set_long_mode(item, long_mode_text_to_enum_value(value));
#if LV_USE_TRANSLATION
        else if(lv_streq("translation_tag", name)) lv_label_set_translation_tag(item, value);
#endif
        else if(lv_streq("bind_text", name)) {
            lv_subject_t * subject = lv_xml_get_subject(&state->scope, value);
            if(subject == NULL) {
                LV_LOG_WARN("Subject \"%s\" doesn't exist in label bind_text", value);
                continue;
            }
            const char * fmt = lv_xml_get_value_of(attrs, "bind_text-fmt");
            if(fmt) {
                fmt = lv_strdup(fmt);
                lv_obj_add_event_cb(item, lv_event_free_user_data_cb, LV_EVENT_DELETE, (void *) fmt);
            }
            lv_label_bind_text(item, subject, fmt);
        }
    }
}
)";

// Excerpt from lib/lvgl/src/xml/parsers/lv_xml_obj_parser.c - lv_xml_obj_apply function
static const char* kObjParserContent = R"(
#define SET_STYLE_IF(prop, value) if(lv_streq(prop_name, "style_" #prop)) lv_obj_set_style_##prop(obj, value, selector)

void lv_xml_obj_apply(lv_xml_parser_state_t * state, const char ** attrs)
{
    void * item = lv_xml_state_get_item(state);

    for(int i = 0; attrs[i]; i += 2) {
        const char * name = attrs[i];
        const char * value = attrs[i + 1];
        size_t name_len = lv_strlen(name);

#if LV_USE_OBJ_NAME
        if(lv_streq("name", name)) {
            lv_obj_set_name(item, value);
        }
#endif
        if(lv_streq("x", name)) lv_obj_set_x(item, lv_xml_to_size(value));
        else if(lv_streq("y", name)) lv_obj_set_y(item, lv_xml_to_size(value));
        else if(lv_streq("width", name)) lv_obj_set_width(item, lv_xml_to_size(value));
        else if(lv_streq("height", name)) lv_obj_set_height(item, lv_xml_to_size(value));
        else if(lv_streq("align", name)) lv_obj_set_align(item, lv_xml_align_to_enum(value));
        else if(lv_streq("flex_flow", name)) lv_obj_set_flex_flow(item, lv_xml_flex_flow_to_enum(value));
        else if(lv_streq("flex_grow", name)) lv_obj_set_flex_grow(item, lv_xml_atoi(value));
        else if(lv_streq("hidden", name)) lv_obj_set_flag(item, LV_OBJ_FLAG_HIDDEN, lv_xml_to_bool(value));
        else if(lv_streq("clickable", name)) lv_obj_set_flag(item, LV_OBJ_FLAG_CLICKABLE, lv_xml_to_bool(value));
        else if(lv_streq("scrollable", name)) lv_obj_set_flag(item, LV_OBJ_FLAG_SCROLLABLE, lv_xml_to_bool(value));
        else if(lv_streq("bind_checked", name)) {
            lv_subject_t * subject = lv_xml_get_subject(&state->scope, value);
            if(subject) {
                lv_obj_bind_checked(item, subject);
            }
        }

        else if(name_len > 6 && lv_memcmp("style_", name, 6) == 0) {
            apply_styles(state, item, name, value);
        }
    }
}

static void apply_styles(lv_xml_parser_state_t * state, lv_obj_t * obj, const char * name, const char * value)
{
    char name_local[512];
    lv_strlcpy(name_local, name, sizeof(name_local));

    lv_style_selector_t selector;
    const char * prop_name = lv_xml_style_string_process(name_local, &selector);

    SET_STYLE_IF(width, lv_xml_to_size(value));
    else SET_STYLE_IF(min_width, lv_xml_to_size(value));
    else SET_STYLE_IF(max_width, lv_xml_to_size(value));
    else SET_STYLE_IF(height, lv_xml_to_size(value));
    else SET_STYLE_IF(bg_color, lv_xml_to_color(value));
    else SET_STYLE_IF(bg_opa, lv_xml_to_opa(value));
    else SET_STYLE_IF(pad_all, lv_xml_atoi(value));
    else SET_STYLE_IF(pad_left, lv_xml_atoi(value));
    else SET_STYLE_IF(pad_right, lv_xml_atoi(value));
    else SET_STYLE_IF(pad_top, lv_xml_atoi(value));
    else SET_STYLE_IF(pad_bottom, lv_xml_atoi(value));
    else SET_STYLE_IF(text_color, lv_xml_to_color(value));
    else SET_STYLE_IF(text_font, lv_xml_get_font(&state->scope, value));
    else SET_STYLE_IF(radius, lv_xml_to_size(value));
    else SET_STYLE_IF(flex_flow, lv_xml_flex_flow_to_enum(value));
    else SET_STYLE_IF(flex_grow, lv_xml_atoi(value));
}
)";

// Widget registration from lib/lvgl/src/xml/lv_xml.c
static const char* kWidgetRegistrationContent = R"(
void lv_xml_init(void)
{
    lv_xml_register_widget("lv_obj", lv_xml_obj_create, lv_xml_obj_apply);

#if LV_USE_BUTTON
    lv_xml_register_widget("lv_button", lv_xml_button_create, lv_xml_button_apply);
#endif

#if LV_USE_LABEL
    lv_xml_register_widget("lv_label", lv_xml_label_create, lv_xml_label_apply);
#endif

#if LV_USE_IMAGE
    lv_xml_register_widget("lv_image", lv_xml_image_create, lv_xml_image_apply);
#endif

    lv_xml_register_widget("lv_obj-event_cb", lv_obj_xml_event_cb_create, lv_obj_xml_event_cb_apply);
    lv_xml_register_widget("lv_obj-bind_flag_if_eq", lv_obj_xml_bind_flag_create, lv_obj_xml_bind_flag_apply);
}
)";

// Custom widget registration from project source
static const char* kCustomWidgetRegistrationContent = R"(
void ui_button_init() {
    lv_xml_register_widget("ui_button", ui_button_create, ui_button_apply);
    spdlog::debug("[ui_button] Registered semantic button widget");
}

void helix::ui::icon::register_widget() {
    lv_xml_register_widget("icon", ui_icon_xml_create, ui_icon_xml_apply);
    spdlog::trace("[Icon] Font-based icon widget registered with XML system");
}
)";

// Real icon.xml component from ui_xml/icon.xml
static const char* kIconComponentXml = R"(<?xml version="1.0"?>
<component>
  <api>
    <prop name="src" type="string" default="home"/>
    <prop name="size" type="string" default="xl"/>
    <prop name="variant" type="string" default=""/>
    <prop name="color" type="string" default=""/>
  </api>
  <consts>
    <px name="size_xs" value="16"/>
    <px name="size_sm" value="24"/>
  </consts>
  <view extends="lv_label"/>
</component>
)";

// Component without explicit extends
static const char* kSimpleComponentXml = R"(<?xml version="1.0"?>
<component>
  <api>
    <prop name="title" type="string" default=""/>
  </api>
  <view>
    <lv_obj width="100%" height="auto"/>
  </view>
</component>
)";

// Non-component XML (just a view)
static const char* kNonComponentXml = R"(<?xml version="1.0"?>
<view>
  <lv_obj name="root" width="100%" height="100%">
    <lv_label text="Hello"/>
  </lv_obj>
</view>
)";

// Event callback pseudo-widget parser content
static const char* kEventCbParserContent = R"(
void lv_obj_xml_event_cb_apply(lv_xml_parser_state_t * state, const char ** attrs)
{
    const char * trigger_str = lv_xml_get_value_of(attrs, "trigger");
    lv_event_code_t code = LV_EVENT_CLICKED;
    if(trigger_str) code = lv_xml_trigger_text_to_enum_value(trigger_str);

    const char * cb_str = lv_xml_get_value_of(attrs, "callback");
    if(cb_str == NULL) {
        LV_LOG_WARN("callback is mandatory for event-call_function");
        return;
    }

    lv_obj_t * obj = lv_xml_state_get_parent(state);
    lv_event_cb_t cb = lv_xml_get_event_cb(&state->scope, cb_str);

    const char * user_data_str = lv_xml_get_value_of(attrs, "user_data");
    char * user_data = NULL;
    if(user_data_str) user_data = lv_strdup(user_data_str);

    lv_obj_add_event_cb(obj, cb, code, user_data);
}
)";

// =============================================================================
// Tests for extract_attributes_from_parser()
// =============================================================================

TEST_CASE("extract_attributes_from_parser extracts label attributes", "[xml-validator]") {
    auto attrs = extract_attributes_from_parser(kLabelParserContent, "lv_label");

    SECTION("Finds direct text attribute") {
        REQUIRE(attrs.count("text") == 1);
    }

    SECTION("Finds long_mode attribute") {
        REQUIRE(attrs.count("long_mode") == 1);
    }

    SECTION("Finds bind_text attribute") {
        REQUIRE(attrs.count("bind_text") == 1);
    }

    SECTION("Finds bind_text-fmt companion attribute") {
        // bind_text-fmt is used with bind_text via lv_xml_get_value_of
        REQUIRE(attrs.count("bind_text-fmt") == 1);
    }

    SECTION("Finds translation_tag attribute") {
        REQUIRE(attrs.count("translation_tag") == 1);
    }

    SECTION("Does not include non-attribute strings") {
        REQUIRE(attrs.count("subject") == 0);
        REQUIRE(attrs.count("item") == 0);
        REQUIRE(attrs.count("value") == 0);
    }
}

TEST_CASE("extract_attributes_from_parser extracts obj attributes", "[xml-validator]") {
    auto attrs = extract_attributes_from_parser(kObjParserContent, "lv_obj");

    SECTION("Finds basic positioning attributes") {
        REQUIRE(attrs.count("x") == 1);
        REQUIRE(attrs.count("y") == 1);
        REQUIRE(attrs.count("width") == 1);
        REQUIRE(attrs.count("height") == 1);
        REQUIRE(attrs.count("align") == 1);
    }

    SECTION("Finds flex layout attributes") {
        REQUIRE(attrs.count("flex_flow") == 1);
        REQUIRE(attrs.count("flex_grow") == 1);
    }

    SECTION("Finds flag attributes") {
        REQUIRE(attrs.count("hidden") == 1);
        REQUIRE(attrs.count("clickable") == 1);
        REQUIRE(attrs.count("scrollable") == 1);
    }

    SECTION("Finds bind_checked attribute") {
        REQUIRE(attrs.count("bind_checked") == 1);
    }

    SECTION("Finds name attribute") {
        REQUIRE(attrs.count("name") == 1);
    }
}

TEST_CASE("extract_attributes_from_parser handles SET_STYLE_IF macro", "[xml-validator]") {
    auto attrs = extract_attributes_from_parser(kObjParserContent, "lv_obj");

    SECTION("Extracts style_width from SET_STYLE_IF(width, ...)") {
        REQUIRE(attrs.count("style_width") == 1);
    }

    SECTION("Extracts style_height from SET_STYLE_IF(height, ...)") {
        REQUIRE(attrs.count("style_height") == 1);
    }

    SECTION("Extracts style_bg_color from SET_STYLE_IF(bg_color, ...)") {
        REQUIRE(attrs.count("style_bg_color") == 1);
    }

    SECTION("Extracts style_pad_* attributes") {
        REQUIRE(attrs.count("style_pad_all") == 1);
        REQUIRE(attrs.count("style_pad_left") == 1);
        REQUIRE(attrs.count("style_pad_right") == 1);
        REQUIRE(attrs.count("style_pad_top") == 1);
        REQUIRE(attrs.count("style_pad_bottom") == 1);
    }

    SECTION("Extracts style_text_* attributes") {
        REQUIRE(attrs.count("style_text_color") == 1);
        REQUIRE(attrs.count("style_text_font") == 1);
    }

    SECTION("Extracts style_radius") {
        REQUIRE(attrs.count("style_radius") == 1);
    }

    SECTION("Extracts style_flex_* attributes") {
        REQUIRE(attrs.count("style_flex_flow") == 1);
        REQUIRE(attrs.count("style_flex_grow") == 1);
    }
}

TEST_CASE("extract_attributes_from_parser handles empty/malformed content", "[xml-validator]") {
    SECTION("Returns empty set for empty string") {
        auto attrs = extract_attributes_from_parser("", "test");
        REQUIRE(attrs.empty());
    }

    SECTION("Returns empty set for content without lv_streq") {
        auto attrs = extract_attributes_from_parser("int main() { return 0; }", "test");
        REQUIRE(attrs.empty());
    }

    SECTION("Returns empty set for malformed lv_streq calls") {
        // Missing closing quote
        auto attrs = extract_attributes_from_parser(R"(lv_streq("broken, name))", "test");
        REQUIRE(attrs.empty());
    }
}

TEST_CASE("extract_attributes_from_parser extracts event_cb attributes", "[xml-validator]") {
    auto attrs = extract_attributes_from_parser(kEventCbParserContent, "lv_obj-event_cb");

    SECTION("Finds trigger attribute") {
        REQUIRE(attrs.count("trigger") == 1);
    }

    SECTION("Finds callback attribute") {
        REQUIRE(attrs.count("callback") == 1);
    }

    SECTION("Finds user_data attribute") {
        REQUIRE(attrs.count("user_data") == 1);
    }
}

// =============================================================================
// Tests for extract_widget_registration()
// =============================================================================

TEST_CASE("extract_widget_registration extracts LVGL widgets", "[xml-validator]") {
    auto registrations = extract_widget_registration(kWidgetRegistrationContent);

    SECTION("Extracts lv_obj registration") {
        bool found = false;
        for (const auto& [name, apply_fn] : registrations) {
            if (name == "lv_obj" && apply_fn == "lv_xml_obj_apply") {
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }

    SECTION("Extracts lv_button registration") {
        bool found = false;
        for (const auto& [name, apply_fn] : registrations) {
            if (name == "lv_button" && apply_fn == "lv_xml_button_apply") {
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }

    SECTION("Extracts lv_label registration") {
        bool found = false;
        for (const auto& [name, apply_fn] : registrations) {
            if (name == "lv_label" && apply_fn == "lv_xml_label_apply") {
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }

    SECTION("Extracts pseudo-widget lv_obj-event_cb") {
        bool found = false;
        for (const auto& [name, apply_fn] : registrations) {
            if (name == "lv_obj-event_cb" && apply_fn == "lv_obj_xml_event_cb_apply") {
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }

    SECTION("Finds correct number of registrations") {
        // lv_obj, lv_button, lv_label, lv_image, lv_obj-event_cb, lv_obj-bind_flag_if_eq
        REQUIRE(registrations.size() == 6);
    }
}

TEST_CASE("extract_widget_registration extracts custom widgets", "[xml-validator]") {
    auto registrations = extract_widget_registration(kCustomWidgetRegistrationContent);

    SECTION("Extracts ui_button registration") {
        bool found = false;
        for (const auto& [name, apply_fn] : registrations) {
            if (name == "ui_button" && apply_fn == "ui_button_apply") {
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }

    SECTION("Extracts icon registration") {
        bool found = false;
        for (const auto& [name, apply_fn] : registrations) {
            if (name == "icon" && apply_fn == "ui_icon_xml_apply") {
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }
}

TEST_CASE("extract_widget_registration handles edge cases", "[xml-validator]") {
    SECTION("Returns empty vector for empty content") {
        auto registrations = extract_widget_registration("");
        REQUIRE(registrations.empty());
    }

    SECTION("Returns empty vector for content without registrations") {
        auto registrations = extract_widget_registration("int main() { return 0; }");
        REQUIRE(registrations.empty());
    }
}

// =============================================================================
// Tests for extract_component_props()
// =============================================================================

TEST_CASE("extract_component_props parses icon component", "[xml-validator]") {
    auto info = extract_component_props(kIconComponentXml);

    SECTION("Extracts extends attribute") {
        REQUIRE(info.extends == "lv_label");
    }

    SECTION("Extracts src prop") {
        REQUIRE(info.props.count("src") == 1);
    }

    SECTION("Extracts size prop") {
        REQUIRE(info.props.count("size") == 1);
    }

    SECTION("Extracts variant prop") {
        REQUIRE(info.props.count("variant") == 1);
    }

    SECTION("Extracts color prop") {
        REQUIRE(info.props.count("color") == 1);
    }

    SECTION("Has exactly 4 props") {
        REQUIRE(info.props.size() == 4);
    }
}

TEST_CASE("extract_component_props handles missing extends", "[xml-validator]") {
    auto info = extract_component_props(kSimpleComponentXml);

    SECTION("Defaults to lv_obj when no extends") {
        REQUIRE(info.extends == "lv_obj");
    }

    SECTION("Still extracts props") {
        REQUIRE(info.props.count("title") == 1);
    }
}

TEST_CASE("extract_component_props handles non-component XML", "[xml-validator]") {
    auto info = extract_component_props(kNonComponentXml);

    SECTION("Returns empty extends for non-component") {
        REQUIRE(info.extends.empty());
    }

    SECTION("Returns empty props for non-component") {
        REQUIRE(info.props.empty());
    }
}

TEST_CASE("extract_component_props handles edge cases", "[xml-validator]") {
    SECTION("Returns empty for empty string") {
        auto info = extract_component_props("");
        REQUIRE(info.extends.empty());
        REQUIRE(info.props.empty());
    }

    SECTION("Returns empty for malformed XML") {
        auto info = extract_component_props("<component><api><prop name=");
        REQUIRE(info.props.empty());
    }
}

// =============================================================================
// Tests for build_inheritance_tree()
// =============================================================================

TEST_CASE("build_inheritance_tree builds correct attribute sets", "[xml-validator]") {
    WidgetDatabase db;

    // Set up lv_obj as base
    db.widget_attrs["lv_obj"] = {"x", "y", "width", "height", "align", "name"};
    db.inheritance["lv_obj"] = ""; // No parent

    // Set up lv_label inheriting from lv_obj
    db.widget_attrs["lv_label"] = {"text", "long_mode", "bind_text"};
    db.inheritance["lv_label"] = "lv_obj";

    // Set up icon component inheriting from lv_label
    db.widget_attrs["icon"] = {"src", "size", "variant", "color"};
    db.inheritance["icon"] = "lv_label";

    auto full_attrs = build_inheritance_tree(db);

    SECTION("lv_obj has only its own attributes") {
        const auto& obj_attrs = full_attrs["lv_obj"];
        REQUIRE(obj_attrs.count("x") == 1);
        REQUIRE(obj_attrs.count("y") == 1);
        REQUIRE(obj_attrs.count("width") == 1);
        REQUIRE(obj_attrs.count("height") == 1);
        REQUIRE(obj_attrs.count("text") == 0); // Not inherited up
    }

    SECTION("lv_label inherits from lv_obj") {
        const auto& label_attrs = full_attrs["lv_label"];
        // Own attributes
        REQUIRE(label_attrs.count("text") == 1);
        REQUIRE(label_attrs.count("long_mode") == 1);
        REQUIRE(label_attrs.count("bind_text") == 1);
        // Inherited from lv_obj
        REQUIRE(label_attrs.count("x") == 1);
        REQUIRE(label_attrs.count("y") == 1);
        REQUIRE(label_attrs.count("width") == 1);
        REQUIRE(label_attrs.count("height") == 1);
    }

    SECTION("icon inherits from lv_label and lv_obj") {
        const auto& icon_attrs = full_attrs["icon"];
        // Own attributes
        REQUIRE(icon_attrs.count("src") == 1);
        REQUIRE(icon_attrs.count("size") == 1);
        REQUIRE(icon_attrs.count("variant") == 1);
        REQUIRE(icon_attrs.count("color") == 1);
        // Inherited from lv_label
        REQUIRE(icon_attrs.count("text") == 1);
        REQUIRE(icon_attrs.count("long_mode") == 1);
        // Inherited from lv_obj (through lv_label)
        REQUIRE(icon_attrs.count("x") == 1);
        REQUIRE(icon_attrs.count("width") == 1);
    }
}

TEST_CASE("build_inheritance_tree handles pseudo-widgets", "[xml-validator]") {
    WidgetDatabase db;

    // Pseudo-widget like lv_obj-event_cb has its own attributes
    db.widget_attrs["lv_obj-event_cb"] = {"trigger", "callback", "user_data"};
    db.inheritance["lv_obj-event_cb"] = ""; // No inheritance for pseudo-widgets

    auto full_attrs = build_inheritance_tree(db);

    SECTION("Pseudo-widget has only its own attributes") {
        const auto& attrs = full_attrs["lv_obj-event_cb"];
        REQUIRE(attrs.count("trigger") == 1);
        REQUIRE(attrs.count("callback") == 1);
        REQUIRE(attrs.count("user_data") == 1);
        REQUIRE(attrs.size() == 3);
    }
}

TEST_CASE("build_inheritance_tree handles missing parent", "[xml-validator]") {
    WidgetDatabase db;

    // Widget claims to inherit from non-existent parent
    db.widget_attrs["orphan_widget"] = {"custom_attr"};
    db.inheritance["orphan_widget"] = "non_existent_parent";

    auto full_attrs = build_inheritance_tree(db);

    SECTION("Widget with missing parent still has its own attributes") {
        const auto& attrs = full_attrs["orphan_widget"];
        REQUIRE(attrs.count("custom_attr") == 1);
    }

    SECTION("Missing parent is handled gracefully (no crash)") {
        // Just verifying no exception was thrown
        REQUIRE(full_attrs.count("orphan_widget") == 1);
    }
}

TEST_CASE("build_inheritance_tree handles empty database", "[xml-validator]") {
    WidgetDatabase db;
    auto full_attrs = build_inheritance_tree(db);

    REQUIRE(full_attrs.empty());
}

TEST_CASE("build_inheritance_tree handles diamond inheritance", "[xml-validator]") {
    // Though unlikely in LVGL, test that we handle it gracefully
    WidgetDatabase db;

    db.widget_attrs["base"] = {"base_attr"};
    db.inheritance["base"] = "";

    // Two intermediate widgets
    db.widget_attrs["left"] = {"left_attr"};
    db.inheritance["left"] = "base";

    db.widget_attrs["right"] = {"right_attr"};
    db.inheritance["right"] = "base";

    // Diamond converges (note: in practice only single inheritance)
    // For this test, just verify left and right both get base_attr
    auto full_attrs = build_inheritance_tree(db);

    SECTION("Left branch gets base attributes") {
        REQUIRE(full_attrs["left"].count("base_attr") == 1);
        REQUIRE(full_attrs["left"].count("left_attr") == 1);
    }

    SECTION("Right branch gets base attributes") {
        REQUIRE(full_attrs["right"].count("base_attr") == 1);
        REQUIRE(full_attrs["right"].count("right_attr") == 1);
    }
}

TEST_CASE("extract_component_props ignores a view written inside a comment", "[xml-validator]") {
    const std::string xml = R"(<component>
  <!-- Usage: <view extends="my_panel" title="X"/> -->
  <view extends="lv_obj"/>
</component>)";
    CHECK(extract_component_props(xml).extends == "lv_obj");
}

TEST_CASE("build_inheritance_tree stops at an inheritance cycle", "[xml-validator]") {
    WidgetDatabase db;
    db.widget_attrs["a"] = {"x"};
    db.widget_attrs["b"] = {"y"};
    db.widget_attrs["self"] = {"z"};
    db.inheritance["a"] = "b";
    db.inheritance["b"] = "a";
    db.inheritance["self"] = "self";

    auto full = build_inheritance_tree(db);
    CHECK(full["a"] == std::unordered_set<std::string>{"x", "y"});
    CHECK(full["self"] == std::unordered_set<std::string>{"z"});
}

TEST_CASE("extract_component_props ignores a prop written inside a comment", "[xml-validator]") {
    const std::string xml = R"(<component>
  <api>
    <!-- <prop name="retired" type="string"/> -->
    <prop name="live" type="string"/>
  </api>
  <view extends="lv_obj"/>
</component>)";
    CHECK(extract_component_props(xml).props == std::unordered_set<std::string>{"live"});
}
