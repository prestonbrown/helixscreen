// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_bed_mesh_tile_layout.cpp
 * @brief decide_bed_mesh_tile_layout()'s flags for each rung and state, and the numbers it
 * feeds. Rung choice itself is choose_size_rung's (test_size_rung.cpp).
 *
 * The needs below are the boxes BedMeshTileWidget measured at the medium tier, in the faces
 * the labels draw in.
 *
 * Run with: ./build/bin/helix-tests "[bed_mesh_tile][layout]"
 */

#include "src/ui/panel_widgets/bed_mesh_tile_layout.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A mesh is loaded: every rung draws.
constexpr RungNeeds kLoaded{rung_box(26, 40),  rung_box(76, 39),  rung_box(40, 60),
                            rung_box(53, 69),  rung_box(163, 97), rung_box(163, 159),
                            rung_box(253, 244)};
/// No mesh, nothing printing: Strip and Column would draw what Glance does, and Full holds
/// two stacked buttons.
constexpr RungNeeds kEmptyIdle{
    rung_box(18, 19),  {}, {}, rung_box(56, 101), rung_box(179, 54), rung_box(163, 187),
    rung_box(253, 244)};
/// No mesh while printing: the buttons are gone, so Full needs no more than Cell's content.
constexpr RungNeeds kEmptyPrinting{
    rung_box(18, 19),  {}, {}, rung_box(56, 101), rung_box(179, 54), rung_box(163, 159),
    rung_box(253, 244)};

struct Scenario {
    bool loaded = true;
    bool printing = false;
    bool full_tier = true;
    bool canvas = true;
    int profiles = 2;
    bool panel = true;
};

BedMeshTileVerdict verdict_for(int w, int h, const Scenario& s = {}) {
    BedMeshTileInput in;
    in.content_w = w;
    in.content_h = h;
    in.mesh_loaded = s.loaded;
    in.print_active = s.printing;
    in.full_tier = s.full_tier;
    in.canvas_available = s.canvas;
    in.panel_available = s.panel;
    in.profile_count = s.profiles;
    in.needs = s.loaded ? kLoaded : (s.printing ? kEmptyPrinting : kEmptyIdle);
    return decide_bed_mesh_tile_layout(in);
}

} // namespace

TEST_CASE("bed_mesh_tile layout: flags follow the rung, mesh loaded", "[bed_mesh_tile][layout]") {
    struct Row {
        int w, h;
        SizeRung rung;
        int decimals;
        bool map, glyph, range, details, scale, footer, render_3d;
    };
    const Row rows[] = {
        //  w    h    rung              dec  map    glyph  range  details scale  footer 3d
        {26, 40, SizeRung::Glance, 2, false, true, true, false, false, false, false},
        {76, 39, SizeRung::Strip, 3, true, false, true, false, false, false, false},
        {40, 60, SizeRung::Column, 3, true, false, true, false, false, false, false},
        {53, 69, SizeRung::Cell, 3, true, false, true, false, false, false, false},
        {163, 97, SizeRung::Wide, 3, true, false, false, true, false, false, false},
        {163, 159, SizeRung::Full, 3, true, false, false, false, true, true, false},
        {253, 244, SizeRung::Large, 3, true, false, false, false, true, true, true},
        {600, 600, SizeRung::Large, 3, true, false, false, false, true, true, true},
    };
    for (const auto& r : rows) {
        INFO(r.w << "x" << r.h);
        const auto v = verdict_for(r.w, r.h);
        CHECK(v.rung == r.rung);
        CHECK(v.decimals == r.decimals);
        CHECK(v.show_map == r.map);
        CHECK(v.show_glyph == r.glyph);
        CHECK(v.show_range == r.range);
        CHECK(v.show_details == r.details);
        CHECK(v.show_scale == r.scale);
        CHECK(v.show_footer == r.footer);
        CHECK(v.render_3d == r.render_3d);
        CHECK_FALSE(v.show_empty_text);
        CHECK_FALSE(v.show_calibrate);
        CHECK_FALSE(v.show_load_profile);
    }
}

TEST_CASE("bed_mesh_tile layout: 3D needs a full-tier board and the Large rung",
          "[bed_mesh_tile][layout]") {
    CHECK(verdict_for(253, 244, {true, false, true}).render_3d);
    CHECK_FALSE(verdict_for(253, 244, {true, false, false}).render_3d);
    CHECK_FALSE(verdict_for(252, 244, {true, false, true}).render_3d);
    // The 2D heatmap still fills the Large rung on a limited board.
    const auto limited = verdict_for(253, 244, {true, false, false});
    CHECK(limited.rung == SizeRung::Large);
    CHECK(limited.show_map);
    // No mesh, nothing to render.
    CHECK_FALSE(verdict_for(253, 244, {false, false, true}).render_3d);
}

TEST_CASE("bed_mesh_tile layout: with no mesh the glyph and what it means",
          "[bed_mesh_tile][layout]") {
    struct Row {
        int w, h;
        SizeRung rung;
        bool empty_text;
    };
    const Row rows[] = {
        {18, 19, SizeRung::Glance, false}, {56, 101, SizeRung::Cell, true},
        {179, 54, SizeRung::Wide, true},   {163, 187, SizeRung::Full, true},
        {400, 400, SizeRung::Large, true},
    };
    for (const auto& r : rows) {
        INFO(r.w << "x" << r.h);
        const auto v = verdict_for(r.w, r.h, {false});
        CHECK(v.rung == r.rung);
        CHECK_FALSE(v.show_map);
        CHECK(v.show_glyph);
        CHECK_FALSE(v.show_range);
        CHECK_FALSE(v.show_details);
        CHECK_FALSE(v.show_scale);
        CHECK_FALSE(v.show_footer);
        CHECK(v.show_empty_text == r.empty_text);
    }
}

TEST_CASE("bed_mesh_tile layout: Calibrate and Load profile need Full, no mesh and an idle printer",
          "[bed_mesh_tile][layout]") {
    // Idle, no mesh, a stored profile: both.
    auto v = verdict_for(163, 187, {false, false, true, true, 2});
    CHECK(v.rung == SizeRung::Full);
    CHECK(v.show_calibrate);
    CHECK(v.show_load_profile);

    // Nothing stored to load.
    v = verdict_for(163, 187, {false, false, true, true, 0});
    CHECK(v.show_calibrate);
    CHECK_FALSE(v.show_load_profile);

    // Not below Full.
    v = verdict_for(179, 158, {false});
    CHECK(v.rung == SizeRung::Wide);
    CHECK_FALSE(v.show_calibrate);
    CHECK_FALSE(v.show_load_profile);

    // Never while printing.
    v = verdict_for(163, 187, {false, true});
    CHECK(v.rung == SizeRung::Full);
    CHECK_FALSE(v.show_calibrate);
    CHECK_FALSE(v.show_load_profile);

    // Not with a mesh loaded.
    v = verdict_for(163, 187, {true});
    CHECK_FALSE(v.show_calibrate);
}

TEST_CASE("bed_mesh_tile layout: without the renderer a glyph stands in for the map",
          "[bed_mesh_tile][layout]") {
    const Scenario no_canvas{true, false, true, false};
    for (const auto& wh :
         {std::pair{76, 39}, std::pair{163, 97}, std::pair{163, 159}, std::pair{253, 244}}) {
        INFO(wh.first << "x" << wh.second);
        const auto v = verdict_for(wh.first, wh.second, no_canvas);
        CHECK_FALSE(v.show_map);
        CHECK(v.show_glyph);
        CHECK_FALSE(v.show_scale);
        CHECK_FALSE(v.render_3d);
    }
}

TEST_CASE("bed_mesh_tile text: range, extent and the unit-free forms", "[bed_mesh_tile][layout]") {
    CHECK(format_mesh_range(0.0f, 2) == "0.00");
    CHECK(format_mesh_range(0.1234f, 3) == "0.123");
    CHECK(format_mesh_range(0.1234f, 2) == "0.12");
    CHECK(format_mesh_range(1.5f, 3) == "1.500");
    CHECK(format_mesh_extent(-0.104f, 0.296f) == "-0.10..0.30");
    CHECK(format_mesh_extent(0.0f, 0.1f) == "0.00..0.10");
}

TEST_CASE("bed_mesh_tile layout: Load profile needs the Bed Mesh panel, Calibrate does not",
          "[bed_mesh_tile][layout]") {
    auto v = verdict_for(163, 187, {false, false, true, true, 2, false});
    CHECK(v.rung == SizeRung::Full);
    CHECK(v.show_calibrate);
    CHECK_FALSE(v.show_load_profile);

    v = verdict_for(163, 187, {false, false, true, true, 2, true});
    CHECK(v.show_load_profile);
}
