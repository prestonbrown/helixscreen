// SPDX-License-Identifier: GPL-3.0-or-later
#include "helix-xml/src/xml/lv_xml.h"
#include "helix/xml/indexed_subject_pool.h"
#include "helix_test_fixture.h"

#include "../catch_amalgamated.hpp"

using helix::xml::IndexedSubjectPool;

TEST_CASE_METHOD(HelixTestFixture, "IndexedSubjectPool registers resolvable indexed names",
                 "[xml][pool]") {
    IndexedSubjectPool names("pooltest_name", IndexedSubjectPool::Type::String);
    names.ensure_size(3);
    names.set_string(0, "alpha");
    names.set_string(2, "gamma");

    REQUIRE(names.size() == 3);
    REQUIRE(lv_xml_get_subject(nullptr, "pooltest_name_0") == names.at(0));
    REQUIRE(lv_xml_get_subject(nullptr, "pooltest_name_2") == names.at(2));
    REQUIRE(std::string(lv_subject_get_string(names.at(0))) == "alpha");
}

TEST_CASE_METHOD(HelixTestFixture, "IndexedSubjectPool grow preserves earlier slots",
                 "[xml][pool]") {
    IndexedSubjectPool vis("pooltest_vis", IndexedSubjectPool::Type::Int);
    vis.ensure_size(2);
    vis.set_int(1, 42);
    lv_subject_t* slot1 = vis.at(1);
    vis.ensure_size(5);          // grow
    REQUIRE(vis.at(1) == slot1); // stable address (unique_ptr)
    REQUIRE(lv_subject_get_int(vis.at(1)) == 42);
    REQUIRE(lv_xml_get_subject(nullptr, "pooltest_vis_4") == vis.at(4));
}

TEST_CASE_METHOD(HelixTestFixture,
                 "IndexedSubjectPool reclaim unregisters all names and is idempotent",
                 "[xml][pool]") {
    auto pool = std::make_unique<IndexedSubjectPool>("pooltest_rc", IndexedSubjectPool::Type::Int);
    pool->ensure_size(3);
    REQUIRE(lv_xml_get_subject(nullptr, "pooltest_rc_1") != nullptr);
    pool->reclaim();
    REQUIRE(lv_xml_get_subject(nullptr, "pooltest_rc_0") == nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "pooltest_rc_2") == nullptr);
    pool->reclaim(); // idempotent — no crash / double free
    pool.reset();    // dtor reclaim on empty — safe
}
