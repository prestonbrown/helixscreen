// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// TEST_MIRROR_OK: exercises the shipped lib/helix-xml parser, which has no
//                 header under include/ or src/ to include

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml_component.h"

#include "../catch_amalgamated.hpp"

// LVGL keeps radio mode in a setter, not an object flag, so the attribute must call it.
TEST_CASE_METHOD(XMLTestFixture, "xml: radio_button attribute sets the object's radio mode",
                 "[xml][radio_button]") {
    for (const bool on : {true, false}) {
        const std::string xml = std::string("<component><view extends=\"lv_obj\" radio_button=\"") +
                                (on ? "true" : "false") + "\"/></component>";
        lv_xml_component_unregister("test_radio_button");
        REQUIRE(lv_xml_register_component_from_data("test_radio_button", xml.c_str()) ==
                LV_RESULT_OK);
        lv_obj_t* obj = create_component("test_radio_button");
        REQUIRE(obj != nullptr);
        CHECK(lv_obj_is_radio_button(obj) == on);
    }
}
