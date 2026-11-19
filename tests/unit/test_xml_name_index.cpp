// SPDX-License-Identifier: GPL-3.0-or-later

// TEST_MIRROR_OK: the code under test is the helix-xml engine, which ships from lib/helix-xml, not
// include/ or src/.

#include "../lvgl_test_fixture.h"
#include "core/subject.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"

#include <cstring>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

// The globals scope answers subject and const lookups through a hash index that
// every register and unregister maintains. Removals in the middle of probe runs
// are the case that breaks an open-addressed index, so churn enough names to
// force collisions and growth, then check every name either way.

TEST_CASE_METHOD(LVGLTestFixture,
                 "globals subjects stay findable across register and unregister churn",
                 "[xml][name_index]") {
    constexpr int N = 300;
    std::vector<helix::Subject> subjects(N);
    std::vector<std::string> names;
    for (int i = 0; i < N; ++i) {
        names.push_back("name_index_subj_" + std::to_string(i));
        subjects[i].init_int(i);
        REQUIRE(lv_xml_register_subject(nullptr, names[i].c_str(), &subjects[i]) == LV_RESULT_OK);
    }

    for (int i = 0; i < N; i += 3) {
        REQUIRE(lv_xml_unregister_subject(nullptr, names[i].c_str()) == LV_RESULT_OK);
    }
    for (int i = 0; i < N; ++i) {
        INFO(names[i]);
        CHECK(lv_xml_get_subject(nullptr, names[i].c_str()) ==
              (i % 3 == 0 ? nullptr : &subjects[i]));
    }

    // Re-registering a live name repoints it rather than adding a second record.
    helix::Subject replacement;
    replacement.init_int(-1);
    REQUIRE(lv_xml_register_subject(nullptr, names[1].c_str(), &replacement) == LV_RESULT_OK);
    CHECK(lv_xml_get_subject(nullptr, names[1].c_str()) == &replacement);
    REQUIRE(lv_xml_unregister_subject(nullptr, names[1].c_str()) == LV_RESULT_OK);
    CHECK(lv_xml_get_subject(nullptr, names[1].c_str()) == nullptr);

    for (int i = 0; i < N; i += 3) {
        REQUIRE(lv_xml_register_subject(nullptr, names[i].c_str(), &subjects[i]) == LV_RESULT_OK);
    }
    for (int i = 0; i < N; ++i) {
        INFO(names[i]);
        CHECK(lv_xml_get_subject(nullptr, names[i].c_str()) == (i == 1 ? nullptr : &subjects[i]));
    }

    for (int i = 0; i < N; ++i) {
        lv_xml_unregister_subject(nullptr, names[i].c_str());
        subjects[i].reset();
    }
}

TEST_CASE("globals consts resolve through the index after registration and update",
          "[xml][name_index]") {
    constexpr int N = 200;
    for (int i = 0; i < N; ++i) {
        const std::string name = "name_index_const_" + std::to_string(i);
        const std::string value = std::to_string(i * 7);
        REQUIRE(lv_xml_register_const(nullptr, name.c_str(), value.c_str()) == LV_RESULT_OK);
    }
    REQUIRE(lv_xml_update_const(nullptr, "name_index_const_42", "updated") == LV_RESULT_OK);

    for (int i = 0; i < N; ++i) {
        const std::string name = "name_index_const_" + std::to_string(i);
        const char* got = lv_xml_get_const(nullptr, name.c_str());
        INFO(name);
        REQUIRE(got != nullptr);
        CHECK(std::string(got) == (i == 42 ? std::string("updated") : std::to_string(i * 7)));
    }
    CHECK(lv_xml_get_const_silent(nullptr, "name_index_const_missing") == nullptr);
}
