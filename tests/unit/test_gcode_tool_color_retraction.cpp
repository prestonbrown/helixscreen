// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Retracting per-tool AMS colours, at all three layers that hold them.
//
// An empty answer from AmsState::routed_tool_colors() means "nothing knowable",
// and every layer below deliberately ignores an empty override vector so that a
// FIRST apply with no AMS data leaves the file's slicer palette alone instead of
// painting over it. That made "stop overriding" inexpressible: the retraction
// added alongside those guards passed {} into all three of them, so all three
// dropped it and stale lane colours stayed on the renderers for the rest of the
// file. The suite stayed green because reverting a no-op is still a no-op.
//
// So retraction gets its own path, and these pin BOTH halves — that the
// retraction actually lands, and that the guards it routes around are still
// there. The 3D case is the one with teeth: overrides are written into
// RibbonGeometry::color_palette IN PLACE (the vertex data indexes into it), so
// without a snapshot taken before the first override the slicer's colours are
// simply gone and there is nothing for a clear to restore.

#include "ui_gcode_viewer.h"

#include "../lvgl_test_fixture.h"
#include "ams_state.h"
#include "gcode_layer_renderer.h"
#include "test_helpers/gcode_layer_renderer_test_access.h"

#ifdef ENABLE_GLES_3D
#include "gcode_geometry_builder.h"
#include "gcode_gles_renderer.h"
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

namespace {

/// The slicer's answer for a two-tool print, and the AMS lanes that would
/// override it. Deliberately unrelated values so a restore that returns the
/// override, or an override that returns the slicer colour, cannot alias onto
/// a passing result.
constexpr uint32_t kSlicerT0 = 0x112233u;
constexpr uint32_t kSlicerT1 = 0x445566u;
constexpr uint32_t kLaneT0 = 0xED1C24u;
constexpr uint32_t kLaneT1 = 0x00A651u;

const std::vector<std::string> kSlicerPaletteHex = {"#112233", "#445566"};

/// lv_color_t as 0xRRGGBB. The test build is LV_COLOR_DEPTH 32 (lv_conf.h only
/// drops to RGB565 on the constrained FBDEV platforms), so the channels are the
/// bytes that went in.
uint32_t rgb_of(lv_color_t c) {
    return (static_cast<uint32_t>(c.red) << 16) | (static_cast<uint32_t>(c.green) << 8) |
           static_cast<uint32_t>(c.blue);
}

/// Resolve tool @p tool through the renderer's palette, with a fallback that is
/// none of the colours above — so "fell through to the fallback" is a distinct,
/// visible outcome rather than something that could pass for a real answer.
constexpr uint32_t kFallback = 0x808080u;

uint32_t resolved(const GCodeLayerRenderer& r, int tool) {
    return rgb_of(
        GCodeLayerRendererTestAccess::tool_palette(r).resolve(tool, lv_color_hex(kFallback)));
}

/// AmsState is a process singleton and other files in the shard install
/// backends into it. Clearing it is what makes routed_tool_colors() answer
/// "nothing knowable" deterministically; putting it back cleared matches what
/// every other AMS test leaves behind.
class NoAmsBackend {
  public:
    NoAmsBackend() {
        AmsState::instance().set_backend(nullptr);
    }
    ~NoAmsBackend() {
        AmsState::instance().set_backend(nullptr);
    }
    NoAmsBackend(const NoAmsBackend&) = delete;
    NoAmsBackend& operator=(const NoAmsBackend&) = delete;
};

#ifdef ENABLE_GLES_3D
/// Geometry carrying nothing but a palette and its tool mapping — enough for
/// every colour decision, and with no prepared_buffers so the repaint path
/// patch_prepared_buffer_colors() takes is a no-op rather than needing real
/// vertex data.
std::unique_ptr<RibbonGeometry> make_palette_geometry(uint32_t t0, uint32_t t1) {
    auto geom = std::make_unique<RibbonGeometry>();
    geom->color_palette = {t0, t1};
    geom->tool_palette_map[0] = 0;
    geom->tool_palette_map[1] = 1;
    return geom;
}
#endif

} // namespace

// ============================================================================
// 3D (GLES) — the palette is overwritten in place, so only a snapshot restores
// ============================================================================

#ifdef ENABLE_GLES_3D

TEST_CASE_METHOD(LVGLTestFixture,
                 "the 3D renderer restores the palette its geometry was built with",
                 "[gcode][gles][colors][retraction]") {
    GCodeGLESRenderer renderer;

    auto geom = make_palette_geometry(kSlicerT0, kSlicerT1);
    RibbonGeometry* baked = geom.get(); // the renderer owns it from here
    const std::vector<uint32_t> slicer_palette = baked->color_palette;
    renderer.set_prebuilt_geometry(std::move(geom), "retraction.gcode");

    renderer.set_tool_color_overrides({kLaneT0, kLaneT1});
    // Assert the setup reached the branch: a restore test proves nothing if the
    // override never landed in the first place.
    REQUIRE(baked->color_palette == std::vector<uint32_t>{kLaneT0, kLaneT1});

    renderer.clear_tool_color_overrides();

    // The slicer's own colours, byte for byte. This is the assertion the
    // in-place overwrite defeats: there is no second copy of them anywhere in
    // the geometry, so a clear that only forgets the override leaves the lane
    // colours sitting here.
    CHECK(baked->color_palette == slicer_palette);
}

TEST_CASE_METHOD(LVGLTestFixture, "an empty 3D override answer leaves the baked palette alone",
                 "[gcode][gles][colors][retraction]") {
    GCodeGLESRenderer renderer;

    auto geom = make_palette_geometry(kSlicerT0, kSlicerT1);
    RibbonGeometry* baked = geom.get();
    const std::vector<uint32_t> slicer_palette = baked->color_palette;
    renderer.set_prebuilt_geometry(std::move(geom), "retraction.gcode");

    // "Nothing knowable" on a FIRST apply. Not a retraction — there is nothing
    // to retract — so the file's palette must survive untouched.
    renderer.set_tool_color_overrides({});
    CHECK(baked->color_palette == slicer_palette);

    // And it must not have counted as an override either: a clear now has
    // nothing to restore and must leave the palette exactly where it is.
    renderer.clear_tool_color_overrides();
    CHECK(baked->color_palette == slicer_palette);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a new 3D geometry does not inherit the previous file's palette snapshot",
                 "[gcode][gles][colors][retraction]") {
    GCodeGLESRenderer renderer;

    renderer.set_prebuilt_geometry(make_palette_geometry(kSlicerT0, kSlicerT1), "first.gcode");
    renderer.set_tool_color_overrides({kLaneT0, kLaneT1});

    // Second file, different slicer colours, no overrides applied to it.
    auto second = make_palette_geometry(0xAABBCCu, 0xDDEEFFu);
    RibbonGeometry* baked = second.get();
    const std::vector<uint32_t> second_palette = baked->color_palette;
    renderer.set_prebuilt_geometry(std::move(second), "second.gcode");

    // The snapshot described the FIRST file's palette. Restored onto this one it
    // would repaint a print in the previous file's colours — worse than the bug
    // it exists to fix, and silent.
    renderer.clear_tool_color_overrides();
    CHECK(baked->color_palette == second_palette);
}

#endif // ENABLE_GLES_3D

// ============================================================================
// 2D — retraction runs through set_tool_color_palette(), so that has to clear
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "an empty 2D override answer leaves the slicer palette alone",
                 "[gcode][colors][retraction]") {
    GCodeLayerRenderer renderer;
    renderer.set_tool_color_palette(kSlicerPaletteHex);
    REQUIRE(resolved(renderer, 0) == kSlicerT0);

    renderer.set_tool_color_overrides({});

    CHECK(resolved(renderer, 0) == kSlicerT0);
    CHECK(resolved(renderer, 1) == kSlicerT1);
}

TEST_CASE_METHOD(LVGLTestFixture, "re-installing the file palette drops the 2D AMS overrides",
                 "[gcode][colors][retraction]") {
    GCodeLayerRenderer renderer;
    renderer.set_tool_color_palette(kSlicerPaletteHex);
    renderer.set_tool_color_overrides({kLaneT0, kLaneT1});
    REQUIRE(resolved(renderer, 0) == kLaneT0);

    // What apply_2d_renderer_colors() does on the retraction path: rebuild from
    // the file, with no overrides left to layer on top.
    renderer.set_tool_color_palette(kSlicerPaletteHex);

    CHECK(resolved(renderer, 0) == kSlicerT0);
    CHECK(resolved(renderer, 1) == kSlicerT1);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "an empty file palette clears a 2D renderer carrying tool colours",
                 "[gcode][colors][retraction]") {
    GCodeLayerRenderer renderer;
    renderer.set_tool_color_palette(kSlicerPaletteHex);
    renderer.set_tool_color_overrides({kLaneT0, kLaneT1});
    REQUIRE(resolved(renderer, 0) == kLaneT0);

    // A file the slicer named no colours for. Retraction still has to land, or
    // the lane colours outlive the AMS answer on exactly the single-colour
    // prints where the file cannot argue back.
    renderer.set_tool_color_palette({});

    CHECK_FALSE(GCodeLayerRendererTestAccess::tool_palette(renderer).has_tool_colors());
    CHECK(resolved(renderer, 0) == kFallback);
}

// ============================================================================
// Widget — the wiring, end to end
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "a dead AMS answer retracts the viewer's applied tool colours",
                 "[gcode_viewer][colors][retraction]") {
    NoAmsBackend no_backend;
    // Assert the setup reached the branch under test: this whole case is about
    // what happens when routed_tool_colors() comes back empty.
    REQUIRE(AmsState::instance().routed_tool_colors().empty());

    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    REQUIRE(parent != nullptr);
    lv_obj_t* viewer = ui_gcode_viewer_create(parent);
    REQUIRE(viewer != nullptr);

    ui_gcode_viewer_set_tool_colors(viewer, {kLaneT0, kLaneT1});
    REQUIRE(ui_gcode_viewer_get_tool_colors(viewer) == std::vector<uint32_t>{kLaneT0, kLaneT1});

    // The shipped defect: this returned false and left the overrides applied,
    // because the retraction it performed was set_tool_colors(viewer, {}) and
    // an empty vector is the one value that function drops on the floor.
    CHECK_FALSE(ui_gcode_viewer_apply_ams_tool_colors(viewer));
    CHECK(ui_gcode_viewer_get_tool_colors(viewer).empty());

    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLTestFixture, "an empty vector on the viewer's setter is not a retraction",
                 "[gcode_viewer][colors][retraction]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    REQUIRE(parent != nullptr);
    lv_obj_t* viewer = ui_gcode_viewer_create(parent);
    REQUIRE(viewer != nullptr);

    ui_gcode_viewer_set_tool_colors(viewer, {kLaneT0, kLaneT1});
    REQUIRE(ui_gcode_viewer_get_tool_colors(viewer).size() == 2);

    // The guard this pins is load-bearing for the OTHER caller: a first apply
    // with no AMS data must leave the file's own colours alone. Making {} mean
    // "clear" here is the tempting one-line fix, and it is the wrong one — the
    // two meanings have to stay on different entry points.
    ui_gcode_viewer_set_tool_colors(viewer, {});
    CHECK(ui_gcode_viewer_get_tool_colors(viewer) == std::vector<uint32_t>{kLaneT0, kLaneT1});

    // The explicit path does retract.
    ui_gcode_viewer_clear_tool_colors(viewer);
    CHECK(ui_gcode_viewer_get_tool_colors(viewer).empty());

    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "retracting the viewer's tool colours with none applied is a no-op",
                 "[gcode_viewer][colors][retraction]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    REQUIRE(parent != nullptr);
    lv_obj_t* viewer = ui_gcode_viewer_create(parent);
    REQUIRE(viewer != nullptr);

    REQUIRE(ui_gcode_viewer_get_tool_colors(viewer).empty());
    ui_gcode_viewer_clear_tool_colors(viewer);
    CHECK(ui_gcode_viewer_get_tool_colors(viewer).empty());

    // Also safe on a null widget — the retraction runs from an AMS observer that
    // can fire while a panel is being torn down.
    ui_gcode_viewer_clear_tool_colors(nullptr);

    lv_obj_delete(parent);
}
