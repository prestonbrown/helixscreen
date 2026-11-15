// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// LVGL keeps radio-button behaviour in a setter rather than an object flag, so
// the XML `radio_button` attribute must reach lv_obj_set_radio_button().
//
// TEST_MIRROR_OK: exercises the shipped lib/helix-xml parser, which has no
//                 header under include/ or src/ to include

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "lvgl/lvgl.h"

#include "../catch_amalgamated.hpp"

namespace {

lv_obj_t* build_child(XMLTestFixture& fx, const char* comp_name, const char* child_attrs) {
    const std::string xml =
        std::string("<component><view extends=\"lv_obj\"><lv_obj name=\"rb\" ") + child_attrs +
        "/></view></component>";
    lv_xml_component_unregister(comp_name);
    REQUIRE(lv_xml_register_component_from_data(comp_name, xml.c_str()) == LV_RESULT_OK);
    lv_obj_t* root = fx.create_component(comp_name);
    REQUIRE(root != nullptr);
    lv_obj_t* child = lv_obj_find_by_name(root, "rb");
    REQUIRE(child != nullptr);
    return child;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "xml: radio_button attribute sets the object's radio mode",
                 "[xml][radio_button]") {
    CHECK(lv_obj_is_radio_button(build_child(*this, "test_rb_on", "radio_button=\"true\"")));
    CHECK_FALSE(
        lv_obj_is_radio_button(build_child(*this, "test_rb_off", "radio_button=\"false\"")));
    CHECK_FALSE(lv_obj_is_radio_button(build_child(*this, "test_rb_unset", "")));
}
