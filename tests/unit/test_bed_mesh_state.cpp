// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_bed_mesh_state.cpp
 * @brief compute_mesh_stats on real and empty matrices, and BedMeshState publishing the
 * active mesh with no panel in existence.
 *
 * Run with: ./build/bin/helix-tests "[bed_mesh][mesh_stats]"
 */

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "bed_mesh_state.h"
#include "bed_mesh_stats.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using nlohmann::json;

namespace {

BedMeshProfile make_profile(std::vector<std::vector<float>> matrix, const char* name = "default") {
    BedMeshProfile p;
    p.name = name;
    p.probed_matrix = std::move(matrix);
    p.y_count = static_cast<int>(p.probed_matrix.size());
    p.x_count = p.probed_matrix.empty() ? 0 : static_cast<int>(p.probed_matrix[0].size());
    p.mesh_min[0] = 10.0f;
    p.mesh_min[1] = 20.0f;
    p.mesh_max[0] = 110.0f;
    p.mesh_max[1] = 220.0f;
    return p;
}

} // namespace

TEST_CASE("compute_mesh_stats: a real matrix", "[bed_mesh][mesh_stats]") {
    // 3 columns, 2 rows. Low point at col 0 row 1, high point at col 2 row 0.
    const auto p = make_profile({{0.00f, 0.10f, 0.30f}, {-0.10f, 0.05f, 0.15f}});
    const MeshStats s = compute_mesh_stats(p);

    CHECK(s.loaded);
    CHECK(s.cols == 3);
    CHECK(s.rows == 2);
    CHECK(s.min_z == Catch::Approx(-0.10f));
    CHECK(s.max_z == Catch::Approx(0.30f));
    CHECK(s.range == Catch::Approx(0.40f));
    CHECK(s.mean_z == Catch::Approx(0.5f / 6.0f));
    // x steps 50, y steps 200: the low point is col 0 row 1, the high col 2 row 0.
    CHECK(s.min_x == Catch::Approx(10.0f));
    CHECK(s.min_y == Catch::Approx(220.0f));
    CHECK(s.max_x == Catch::Approx(110.0f));
    CHECK(s.max_y == Catch::Approx(20.0f));
}

TEST_CASE("compute_mesh_stats: a flat mesh has no range", "[bed_mesh][mesh_stats]") {
    const MeshStats s = compute_mesh_stats(make_profile({{0.2f, 0.2f}, {0.2f, 0.2f}}));
    CHECK(s.loaded);
    CHECK(s.range == 0.0f);
}

TEST_CASE("compute_mesh_stats: equal extremes report the first point probed",
          "[bed_mesh][mesh_stats]") {
    const MeshStats s = compute_mesh_stats(make_profile({{0.3f, 0.0f, 0.3f}, {0.0f, 0.3f, 0.0f}}));
    CHECK(s.max_x == Catch::Approx(10.0f));
    CHECK(s.max_y == Catch::Approx(20.0f));
    CHECK(s.min_x == Catch::Approx(60.0f));
    CHECK(s.min_y == Catch::Approx(20.0f));
}

TEST_CASE("compute_mesh_stats: Klipper's empty mesh reads as not loaded",
          "[bed_mesh][mesh_stats]") {
    CHECK_FALSE(compute_mesh_stats(BedMeshProfile{}).loaded);
    CHECK_FALSE(compute_mesh_stats(make_profile({{}})).loaded); // [[]]
    CHECK_FALSE(compute_mesh_stats(make_profile({{}, {}})).loaded);
    const MeshStats none = compute_mesh_stats(make_profile({}));
    CHECK_FALSE(none.loaded);
    CHECK(none.range == 0.0f);
    CHECK(none.cols == 0);
}

TEST_CASE("mean_normalized_matrix centres the matrix on zero", "[bed_mesh][mesh_stats]") {
    const auto p = make_profile({{0.0f, 0.2f}, {0.4f, 0.6f}});
    const auto n = mean_normalized_matrix(p, compute_mesh_stats(p));
    REQUIRE(n.size() == 2);
    CHECK(n[0][0] == Catch::Approx(-0.3f));
    CHECK(n[1][1] == Catch::Approx(0.3f));
}

namespace {

struct BedMeshStateFixture : public LVGLUITestFixture {
    BedMeshStateFixture()
        : mock(MoonrakerClientMock::PrinterType::VORON_24), mock_api(mock, state()) {
        BedMeshState::instance().deinit_subjects();
        BedMeshState::instance().init_subjects();
    }
    ~BedMeshStateFixture() override {
        BedMeshState::instance().deinit_subjects();
        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    static std::string text(const char* name) {
        return lv_subject_get_string(lv_xml_get_subject(nullptr, name));
    }
    static int number(const char* name) {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, name));
    }

    MoonrakerClientMock mock;
    MoonrakerAPI mock_api;
};

json mesh_frame(const json& matrix, const char* profile) {
    return {{"bed_mesh",
             {{"profile_name", profile},
              {"probed_matrix", matrix},
              {"mesh_min", {10.0, 20.0}},
              {"mesh_max", {110.0, 220.0}},
              {"profiles", {{"default", json::object()}, {"cold", json::object()}}}}}};
}

} // namespace

TEST_CASE_METHOD(BedMeshStateFixture, "BedMeshState publishes a mesh frame with no panel",
                 "[bed_mesh][mesh_stats]") {
    BedMeshState::instance().attach(&mock_api);
    CHECK(number("bed_mesh_available") == 0);
    const int version = number("bed_mesh_version");

    mock.dispatch_status_update(mesh_frame(
        json::array({json::array({0.0, 0.1, 0.3}), json::array({-0.1, 0.05, 0.15})}), "adaptive"));
    drain();

    CHECK(number("bed_mesh_available") == 1);
    CHECK(text("bed_mesh_profile_name") == "adaptive");
    CHECK(text("bed_mesh_dimensions") == "3x2 points");
    CHECK(text("bed_mesh_variance") == "0.400 mm");
    CHECK(number("bed_mesh_profile_count") == 2);
    CHECK(number("bed_mesh_version") > version);
    CHECK(BedMeshState::instance().stats().range == Catch::Approx(0.4f));
}

TEST_CASE_METHOD(BedMeshStateFixture, "BedMeshState reads Klipper's [[]] as no mesh",
                 "[bed_mesh][mesh_stats]") {
    BedMeshState::instance().attach(&mock_api);
    mock.dispatch_status_update(mesh_frame(json::array({json::array({0.0, 0.2})}), "default"));
    drain();
    REQUIRE(number("bed_mesh_available") == 1);

    mock.dispatch_status_update(mesh_frame(json::array({json::array()}), "default"));
    drain();

    CHECK(number("bed_mesh_available") == 0);
    CHECK(text("bed_mesh_max_value") == "--");
    CHECK(text("bed_mesh_variance").empty());
    CHECK_FALSE(BedMeshState::instance().stats().loaded);
}

TEST_CASE_METHOD(BedMeshStateFixture, "BedMeshState ignores frames without a bed_mesh object",
                 "[bed_mesh][mesh_stats]") {
    BedMeshState::instance().attach(&mock_api);
    const int version = number("bed_mesh_version");
    mock.dispatch_status_update(json{{"toolhead", {{"homed_axes", "xyz"}}}});
    drain();
    CHECK(number("bed_mesh_version") == version);
}

TEST_CASE_METHOD(BedMeshStateFixture, "BedMeshState stops following once detached",
                 "[bed_mesh][mesh_stats]") {
    BedMeshState::instance().attach(&mock_api);
    BedMeshState::instance().detach();
    mock.dispatch_status_update(mesh_frame(json::array({json::array({0.0, 0.2})}), "default"));
    drain();
    CHECK(number("bed_mesh_available") == 0);
}

TEST_CASE_METHOD(BedMeshStateFixture,
                 "BedMeshState publishes the high and low spots with coordinates",
                 "[bed_mesh][mesh_stats]") {
    BedMeshState::instance().attach(&mock_api);
    mock.dispatch_status_update(mesh_frame(
        json::array({json::array({0.0, 0.1, 0.3}), json::array({-0.1, 0.05, 0.15})}), "adaptive"));
    drain();

    CHECK(text("bed_mesh_max_value") == "0.300 mm");
    CHECK(text("bed_mesh_min_value") == "-0.100 mm");
    CHECK(text("bed_mesh_max_coord") == "[110, 20]");
    CHECK(text("bed_mesh_min_coord") == "[10, 220]");
}

TEST_CASE_METHOD(BedMeshStateFixture, "BedMeshState attach with no API is a no-op",
                 "[bed_mesh][mesh_stats]") {
    BedMeshState::instance().attach(nullptr);
    CHECK(number("bed_mesh_available") == 0);
}

TEST_CASE_METHOD(BedMeshStateFixture, "BedMeshState retranslate leaves a loaded mesh's text alone",
                 "[bed_mesh][mesh_stats]") {
    BedMeshState::instance().attach(&mock_api);
    mock.dispatch_status_update(
        mesh_frame(json::array({json::array({0.0, 0.2}), json::array({0.1, 0.3})}), "default"));
    drain();
    REQUIRE(text("bed_mesh_dimensions") == "2x2 points");

    BedMeshState::instance().retranslate();

    CHECK(text("bed_mesh_dimensions") == "2x2 points");
}
