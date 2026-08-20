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
    REQUIRE(s.tagged == false);
    REQUIRE(s.fallback_halo == false);
}

TEST_CASE("excluded segments are recolored and translucent", "[gcode_selection_style]") {
    auto s = selection::resolve(true, false, true);
    REQUIRE(s.override_color == true);
    REQUIRE(s.rgb == selection::kExcludedColor);
    REQUIRE(s.opa == selection::kExcludedOpa);
}

// The whole point of the feature: a selected object keeps its filament color and
// is marked by the rim instead of being recolored. A test that asserts
// override_color == false here fails against the old blue-recolor behavior.
TEST_CASE("highlighted segments keep filament color and carry the tag", "[gcode_selection_style]") {
    auto s = selection::resolve(false, true, true);
    REQUIRE(s.override_color == false);
    REQUIRE(s.tagged == true);
    // The tag IS the opacity byte. Anything else and stroke_selection_rim() has
    // nothing to find.
    REQUIRE(s.opa == kSelectedAlpha);
}

// Exclusion still wins on color: you have to be able to see which object you
// just selected in order to un-exclude it from the side list. What it does NOT
// keep is its 60% fade, because the opacity byte is where the tag lives and an
// object cannot be tagged and faded at the same time.
TEST_CASE("an excluded object that is also selected keeps exclusion color and takes the tag",
          "[gcode_selection_style]") {
    auto s = selection::resolve(true, true, true);
    REQUIRE(s.override_color == true);
    REQUIRE(s.rgb == selection::kExcludedColor);
    REQUIRE(s.tagged == true);
    REQUIRE(s.opa == kSelectedAlpha);
    REQUIRE(s.opa != selection::kExcludedOpa);
}

TEST_CASE("travel moves are never tagged", "[gcode_selection_style]") {
    // A travel move belonging to the selected object must not contribute to the
    // silhouette: travels cut across the interior, so tagging them would drag the
    // tagged region out to a bounding box and the rim would trace that instead of
    // the object.
    auto s = selection::resolve(false, true, /*is_extrusion=*/false);
    REQUIRE(s.tagged == false);
    REQUIRE(s.fallback_halo == false);
    REQUIRE(s.opa != kSelectedAlpha);
}

// ---------------------------------------------------------------------------
// outline_width_px(): the rim is measured in SCREEN PIXELS by both renderers.
//
// It replaced two world-space knobs that could not hold a width: the 3D shell
// pushed a fixed 0.25mm along the normal, which at the plate-wide zoom the
// viewer opens on is about half a pixel, so it read as speckle or as nothing.
// ---------------------------------------------------------------------------

TEST_CASE("the rim is at least one pixel on any panel", "[gcode_selection_style]") {
    for (int w : {128, 320, 321, 480, 800, 1024, 1920}) {
        REQUIRE(selection::outline_width_px(w) >= 1);
    }
}

TEST_CASE("the rim is thinner on small panels", "[gcode_selection_style]") {
    // At 480x272 a 2px-per-side rim swallows small objects whole.
    REQUIRE(selection::outline_width_px(320) < selection::outline_width_px(800));
    // And the boundary is inclusive, so 320 itself counts as small.
    REQUIRE(selection::outline_width_px(selection::kSmallPanelWidthPx) ==
            selection::kOutlineSmallPanelPx);
    REQUIRE(selection::outline_width_px(selection::kSmallPanelWidthPx + 1) ==
            selection::kOutlinePx);
}

// ---------------------------------------------------------------------------
// halo_width(): the draw-API fallback for TOP_DOWN / ISOMETRIC, which paint
// straight into the LVGL layer and so have no pixel buffer for the rim scan to
// read. Dilate-and-overpaint is sound there because those modes draw ONE layer:
// there is nothing stacked above to punch through the white.
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

TEST_CASE("only walls contribute to the fallback halo", "[gcode_selection_style]") {
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

TEST_CASE("only the fallback path is offered a halo", "[gcode_selection_style]") {
    // The cached path must NOT be told to paint white: it derives the rim from
    // where the object actually landed, and a white pre-pass would move the
    // boundary outward so the rim stopped tracing the real contour.
    auto s = selection::resolve(false, true, true);
    REQUIRE(s.fallback_halo == true);
    REQUIRE(s.tagged == true);
    REQUIRE(selection::resolve(false, false, true).fallback_halo == false);
}
