// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "printer_excluded_objects_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using Catch::Approx;
using namespace helix;

TEST_CASE("Object geometry storage and retrieval", "[exclude_object][geometry]") {
    PrinterExcludedObjectsState state;
    state.init_subjects();

    SECTION("set and get object geometry") {
        std::vector<PrinterExcludedObjectsState::ObjectInfo> objects = {
            {"part_1", {100.0f, 100.0f}, {80.0f, 80.0f}, {120.0f, 120.0f}, {}, true, true},
            {"part_2", {200.0f, 150.0f}, {180.0f, 130.0f}, {220.0f, 170.0f}, {}, true, true},
        };
        state.set_defined_objects_with_geometry(objects);

        auto defined = state.get_defined_objects();
        REQUIRE(defined.size() == 2);

        auto geom = state.get_object_geometry("part_1");
        REQUIRE(geom.has_value());
        REQUIRE(geom->center.x == Approx(100.0f));
        REQUIRE(geom->bbox_min.x == Approx(80.0f));
        REQUIRE(geom->bbox_max.x == Approx(120.0f));
    }

    SECTION("unknown object returns nullopt") {
        auto geom = state.get_object_geometry("nonexistent");
        REQUIRE_FALSE(geom.has_value());
    }

    SECTION("object without geometry flags") {
        std::vector<PrinterExcludedObjectsState::ObjectInfo> objects = {
            {"no_geom", {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {}, false, false},
        };
        state.set_defined_objects_with_geometry(objects);
        auto geom = state.get_object_geometry("no_geom");
        REQUIRE(geom.has_value());
        REQUIRE_FALSE(geom->has_center);
        REQUIRE_FALSE(geom->has_bbox);
    }

    SECTION("version bumps on geometry update") {
        int v1 = lv_subject_get_int(state.get_defined_objects_version_subject());
        std::vector<PrinterExcludedObjectsState::ObjectInfo> objects = {
            {"obj_a", {50.0f, 50.0f}, {10.0f, 10.0f}, {90.0f, 90.0f}, {}, true, true},
        };
        state.set_defined_objects_with_geometry(objects);
        int v2 = lv_subject_get_int(state.get_defined_objects_version_subject());
        REQUIRE(v2 > v1);
    }

    state.deinit_subjects();
}

TEST_CASE("exclude_object status parse", "[exclude_object][geometry]") {
    PrinterExcludedObjectsState state;
    state.init_subjects(false);

    state.update_from_status(nlohmann::json::parse(R"({"exclude_object": {
        "objects": [
            {"name": "part_a", "center": [10, 20], "polygon": [[0, 0], ["x", 5], [30, 40]]},
            {"name": "part_b"},
            {"center": [1, 1]},
            7
        ],
        "excluded_objects": ["part_a", 3],
        "current_object": "part_a"
    }})"));

    REQUIRE(state.get_defined_objects().size() == 2);
    CHECK(state.get_excluded_objects() == std::unordered_set<std::string>{"part_a"});
    CHECK(state.get_current_object() == "part_a");

    auto a = state.get_object_geometry("part_a");
    REQUIRE(a.has_value());
    CHECK(a->has_center);
    CHECK(a->center.x == Approx(10.0f));
    CHECK(a->center.y == Approx(20.0f));
    // The non-numeric point is skipped: it neither throws nor moves the bbox.
    CHECK(a->has_bbox);
    CHECK(a->polygon.size() == 2);
    CHECK(a->bbox_min.x == Approx(0.0f));
    CHECK(a->bbox_min.y == Approx(0.0f));
    CHECK(a->bbox_max.x == Approx(30.0f));
    CHECK(a->bbox_max.y == Approx(40.0f));

    auto b = state.get_object_geometry("part_b");
    REQUIRE(b.has_value());
    CHECK_FALSE(b->has_center);
    CHECK_FALSE(b->has_bbox);

    SECTION("a null current_object clears it") {
        state.update_from_status(
            nlohmann::json::parse(R"({"exclude_object": {"current_object": null}})"));
        CHECK(state.get_current_object().empty());
        CHECK(state.get_defined_objects().size() == 2);
    }

    SECTION("a frame without exclude_object changes nothing") {
        state.update_from_status(nlohmann::json::parse(R"({"toolhead": {}})"));
        CHECK(state.get_current_object() == "part_a");
        CHECK(state.get_defined_objects().size() == 2);
    }

    state.deinit_subjects();
}

TEST_CASE("make_object_info derives the bbox from the polygon",
          "[exclude_object][geometry][pre_start_exclude]") {
    using S = PrinterExcludedObjectsState;
    const auto info = S::make_object_info("A", glm::vec2(5.0f, 6.0f), {{1, 2}, {9, 3}, {4, 8}});
    CHECK(info.name == "A");
    CHECK(info.has_center);
    CHECK(info.center == glm::vec2(5.0f, 6.0f));
    CHECK(info.has_bbox);
    CHECK(info.bbox_min == glm::vec2(1.0f, 2.0f));
    CHECK(info.bbox_max == glm::vec2(9.0f, 8.0f));
    CHECK(info.polygon.size() == 3);

    const auto bare = S::make_object_info("B", std::nullopt, {});
    CHECK_FALSE(bare.has_center);
    CHECK_FALSE(bare.has_bbox);
}

TEST_CASE("A status polygon with no usable point gives no bbox",
          "[exclude_object][geometry][pre_start_exclude]") {
    PrinterExcludedObjectsState state;
    state.init_subjects(false);
    state.update_from_status(nlohmann::json::parse(R"({"exclude_object": {
        "objects": [{"name": "A", "center": [1, 1], "polygon": [["x", "y"]]}]}})"));
    const auto geom = state.get_object_geometry("A");
    REQUIRE(geom.has_value());
    CHECK(geom->has_center);
    CHECK_FALSE(geom->has_bbox);
    state.deinit_subjects();
}
