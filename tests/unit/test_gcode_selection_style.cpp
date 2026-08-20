// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/gcode_selection_style.h"

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

// ---------------------------------------------------------------------------
// resolve(): the single answer to "how does a selected/excluded segment look".
// Before this header existed there were five divergent answers across the three
// renderers (blue in the 2D isometric cache path, green via theme "success" in
// the CPU wireframe 3D path, nothing at all in the GLES path, plus a dead 1.8x
// brightness bake in the geometry builder). These cases pin the one answer.
// ---------------------------------------------------------------------------

TEST_CASE("plain extrusion keeps the caller's color at full opacity", "[gcode_selection_style]") {
    auto s = selection::resolve(/*excluded=*/false, /*highlighted=*/false, /*is_extrusion=*/true);
    REQUIRE(s.override_color == false);
    REQUIRE(s.opa == 255);
    REQUIRE(s.halo == false);
}

TEST_CASE("excluded segments are recolored and translucent", "[gcode_selection_style]") {
    auto s = selection::resolve(true, false, true);
    REQUIRE(s.override_color == true);
    REQUIRE(s.rgb == selection::kExcludedColor);
    REQUIRE(s.opa == selection::kExcludedOpa);
}

// The whole point of the feature: a selected object keeps its filament color and
// is marked by the halo instead of being recolored. A test that asserts
// override_color == false here fails against the old blue-recolor behavior.
TEST_CASE("highlighted segments keep filament color and get a halo", "[gcode_selection_style]") {
    auto s = selection::resolve(false, true, true);
    REQUIRE(s.override_color == false);
    REQUIRE(s.halo == true);
    REQUIRE(s.opa == 255);
}

// Exclusion wins on color because that is the existing cache-path precedence
// (the excluded branch `continue`s before the highlight check ever runs), but
// the halo must still be drawn: you have to be able to see which object you
// just selected in order to un-exclude it from the side list.
TEST_CASE("an excluded object that is also selected keeps exclusion color plus halo",
          "[gcode_selection_style]") {
    auto s = selection::resolve(true, true, true);
    REQUIRE(s.override_color == true);
    REQUIRE(s.rgb == selection::kExcludedColor);
    REQUIRE(s.opa == selection::kExcludedOpa);
    REQUIRE(s.halo == true);
}

TEST_CASE("travel moves are never haloed", "[gcode_selection_style]") {
    // A travel move belonging to the selected object must not contribute to the
    // silhouette: travels cut across the interior and would spray white through
    // the middle of the shape.
    auto s = selection::resolve(false, true, /*is_extrusion=*/false);
    REQUIRE(s.halo == false);
}

// ---------------------------------------------------------------------------
// halo_width(): shared so the software rasterizer and the lv_draw_line paths
// cannot disagree about how thick the outline is.
// ---------------------------------------------------------------------------

TEST_CASE("halo is wider than the core line it outlines", "[gcode_selection_style]") {
    for (int base = 1; base <= 8; ++base) {
        REQUIRE(selection::halo_width(base, /*small_panel=*/false) > base);
        REQUIRE(selection::halo_width(base, /*small_panel=*/true) > base);
    }
}

TEST_CASE("halo is thinner on small panels", "[gcode_selection_style]") {
    // At 480x272 a 2px-per-side halo swallows small objects whole.
    REQUIRE(selection::halo_width(2, true) < selection::halo_width(2, false));
}

TEST_CASE("halo adds an even total so it is symmetric about the core", "[gcode_selection_style]") {
    // An odd delta puts more halo on one side than the other, which reads as a
    // drop shadow rather than an outline.
    for (int base = 1; base <= 8; ++base) {
        REQUIRE((selection::halo_width(base, false) - base) % 2 == 0);
        REQUIRE((selection::halo_width(base, true) - base) % 2 == 0);
    }
}

// ---------------------------------------------------------------------------
// bracket_arm_length(): was duplicated at gcode_layer_renderer.cpp:1485 and
// gcode_gles_renderer.cpp:1942, each commented as matching the other.
// ---------------------------------------------------------------------------

TEST_CASE("bracket arm is 20% of the shortest edge", "[gcode_selection_style]") {
    AABB bbox{glm::vec3(0.0f), glm::vec3(20.0f, 30.0f, 40.0f)};
    REQUIRE(selection::bracket_arm_length(bbox) == Catch::Approx(4.0f)); // 20 * 0.2
}

TEST_CASE("bracket arm is capped at 5mm on large objects", "[gcode_selection_style]") {
    AABB bbox{glm::vec3(0.0f), glm::vec3(200.0f, 200.0f, 200.0f)};
    REQUIRE(selection::bracket_arm_length(bbox) == Catch::Approx(5.0f));
}

TEST_CASE("a degenerate bbox yields no arm", "[gcode_selection_style]") {
    // Below 0.01mm the brackets are sub-pixel noise; both renderers skipped the
    // object entirely. 0 is the shared "do not draw" signal.
    AABB bbox{glm::vec3(0.0f), glm::vec3(0.001f, 0.001f, 0.001f)};
    REQUIRE(selection::bracket_arm_length(bbox) == 0.0f);
}

// The 2D emitter had no is_empty() check and relied incidentally on an -inf
// edge tripping the degeneracy guard; the 3D emitter checked explicitly. This
// pins the behavior so the shared helper is safe for both.
TEST_CASE("an empty bbox yields no arm rather than inf or nan", "[gcode_selection_style]") {
    AABB empty;
    REQUIRE(empty.is_empty());
    REQUIRE(selection::bracket_arm_length(empty) == 0.0f);
}

// ---------------------------------------------------------------------------
// to_vec4(): the GLES path needs the same constants as normalized floats. The
// bracket color previously disagreed: 0xC0C0C0 in 2D versus a hand-written
// 0.75f in 3D under a comment claiming they matched. 0.75 * 255 = 191, not 192.
// ---------------------------------------------------------------------------

TEST_CASE("to_vec4 round-trips the bracket color exactly", "[gcode_selection_style]") {
    auto v = selection::to_vec4(selection::kBracketColor);
    REQUIRE(v.r == Catch::Approx(192.0f / 255.0f));
    REQUIRE(v.g == Catch::Approx(192.0f / 255.0f));
    REQUIRE(v.b == Catch::Approx(192.0f / 255.0f));
    REQUIRE(v.a == Catch::Approx(1.0f));
    // Guard against anyone reintroducing the 0.75f approximation.
    REQUIRE(v.r != Catch::Approx(0.75f));
}

TEST_CASE("to_vec4 carries alpha through", "[gcode_selection_style]") {
    auto v = selection::to_vec4(selection::kExcludedColor, selection::kExcludedOpa);
    REQUIRE(v.a == Catch::Approx(153.0f / 255.0f));
}

// ---------------------------------------------------------------------------
// halo_feature(): which features trace the silhouette.
//
// This exists because of a defect the pixel tests below could not see. The halo
// was drawn for every extrusion of the selected object, so each INFILL line got
// its own white band and the object rendered as white stripes across a dark
// middle with a correct-looking ring around it. The outer wall is the contour;
// infill never is.
// ---------------------------------------------------------------------------

TEST_CASE("only walls contribute to the silhouette", "[gcode_selection_style]") {
    REQUIRE(selection::halo_feature(FeatureType::OuterWall));
    REQUIRE(selection::halo_feature(FeatureType::OverhangWall));

    // Infill is what produced the stripes.
    REQUIRE_FALSE(selection::halo_feature(FeatureType::SparseInfill));
    REQUIRE_FALSE(selection::halo_feature(FeatureType::SolidInfill));
    REQUIRE_FALSE(selection::halo_feature(FeatureType::GapInfill));

    // Inner walls sit behind the outer one, so they would draw a second rim
    // inside the object.
    REQUIRE_FALSE(selection::halo_feature(FeatureType::InnerWall));

    // Skins and supports are not the object's outline either.
    REQUIRE_FALSE(selection::halo_feature(FeatureType::TopSurface));
    REQUIRE_FALSE(selection::halo_feature(FeatureType::BottomSurface));
    REQUIRE_FALSE(selection::halo_feature(FeatureType::Support));
    REQUIRE_FALSE(selection::halo_feature(FeatureType::Skirt));
    REQUIRE_FALSE(selection::halo_feature(FeatureType::Brim));
}

TEST_CASE("a file without feature annotations still gets a halo", "[gcode_selection_style]") {
    // Not every slicer emits ;TYPE comments. Treating Unknown as ineligible
    // would silently leave those files with no selection cue at all, which is a
    // worse failure than a slightly generous one.
    REQUIRE(selection::halo_feature(FeatureType::Unknown));
}

TEST_CASE("the halo cover width leaves a rim at least two pixels wide", "[gcode_selection_style]") {
    // apply_ssao() darkens every filled pixel that has an empty neighbour, which
    // consumes a one-pixel rim entirely - white 255 becomes 76 and nothing
    // reaches the screen. The gap between halo and cover has to survive that.
    const int rim_per_side = (selection::kHaloDeltaPx - selection::kHaloCoverBonusPx) / 2;
    REQUIRE(rim_per_side >= 2);
    // And the cover must actually be narrower than the halo, or there is no rim.
    REQUIRE(selection::kHaloCoverBonusPx < selection::kHaloDeltaPx);
    REQUIRE(selection::kHaloCoverBonusPx >= 0);
}
