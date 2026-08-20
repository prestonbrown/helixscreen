// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"

#include <algorithm>
#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

// ---------------------------------------------------------------------------
// apply_ssao() runs IN PLACE on the render cache and keeps an undo log of the
// pixels it darkened, instead of darkening into a full-canvas second buffer.
// That is a ~427KB saving against a ~6KB log, measured across a 218-layer
// print.
//
// It is only correct because of two things that no compiler will check:
//
//   1. The pass reads neighbour ALPHA and writes only RGB, so a pixel it has
//      already rewritten still answers every later neighbour test the same way.
//   2. The darkening is a MULTIPLY, so it must never run over its own output,
//      and pixels that stop being edges when new layers land must be put back.
//
// Break either and the failure is gradual and plausible-looking: the model
// slowly darkens, or a band of stale shading follows the print up the plate.
// Nothing crashes. These tests are the only thing standing between that and a
// release.
// ---------------------------------------------------------------------------

namespace {

/// A model tall enough that rendering "up to layer N" is meaningfully different
/// from rendering the whole thing, so the progressive-append path is actually
/// exercised rather than trivially skipped.
ParsedGCodeFile make_layered_gcode(int layer_count) {
    ParsedGCodeFile gcode;
    const int16_t obj = gcode.intern_object_name("tower");

    for (int i = 0; i < layer_count; ++i) {
        Layer layer;
        const float z = 0.2f * static_cast<float>(i + 1);
        layer.z_height = z;

        // A square ring per layer. A ring has interior and exterior edges, so
        // the SSAO pass has real work to do rather than one straight line.
        const float lo = 20.0f;
        const float hi = 60.0f;
        const glm::vec3 corners[4] = {{lo, lo, z}, {hi, lo, z}, {hi, hi, z}, {lo, hi, z}};
        for (int c = 0; c < 4; ++c) {
            ToolpathSegment seg;
            seg.start = corners[c];
            seg.end = corners[(c + 1) % 4];
            seg.is_extrusion = true;
            seg.object_name_index = obj;
            layer.segments.push_back(seg);
            layer.bounding_box.expand(seg.start);
            layer.bounding_box.expand(seg.end);
        }
        layer.segment_count_extrusion = 4;
        gcode.layers.push_back(std::move(layer));
        gcode.total_segments += 4;
    }

    gcode.global_bounding_box.expand(glm::vec3(20.0f, 20.0f, 0.2f));
    gcode.global_bounding_box.expand(
        glm::vec3(60.0f, 60.0f, 0.2f * static_cast<float>(layer_count)));
    return gcode;
}

constexpr int kCanvas = 200;
constexpr size_t kBufBytes = static_cast<size_t>(kCanvas) * kCanvas * 4;

/// Drive a renderer until its progressive cache has caught up, then take one
/// clean blit of the finished cache.
///
/// The clear before the last frame is load-bearing. blit_cache() composites the
/// cache over whatever is already on the canvas, and the progressive build blits
/// on every frame, so antialiased partial-alpha pixels accumulate across frames.
/// Two renders that reach the same cache by a different number of frames then
/// differ on those pixels for reasons that have nothing to do with what is being
/// tested. Clearing first makes the snapshot exactly "one blit of the completed
/// cache", which is the thing these tests are comparing.
void drive(GCodeLayerRenderer& renderer, lv_obj_t* canvas, uint8_t* buf) {
    lv_obj_update_layout(canvas);
    auto frame = [&]() {
        lv_layer_t layer;
        lv_area_t clip = {0, 0, kCanvas - 1, kCanvas - 1};
        lv_canvas_init_layer(canvas, &layer);
        renderer.render(&layer, &clip);
        lv_canvas_finish_layer(canvas, &layer);
        lv_timer_handler_safe();
    };
    // Warm-up frames deliberately skip heavy caching; get past them first.
    for (int i = 0; i < 3; ++i) {
        frame();
    }
    int guard = 0;
    while (renderer.needs_more_frames() && guard++ < 500) {
        frame();
    }
    REQUIRE(guard < 500); // cache never completed: harness bug
    std::fill(buf, buf + kBufBytes, uint8_t{0});
    frame();
}

/// Configure a renderer the same way every time, so the only variable in a
/// comparison is the thing under test.
void configure(GCodeLayerRenderer& renderer, ParsedGCodeFile& gcode, bool ssao) {
    renderer.set_gcode(&gcode);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::FRONT);
    renderer.set_ghost_mode(false); // no background thread: deterministic

    // A BRIGHT filament colour, and it is load-bearing. The SSAO pass darkens by
    // multiplying RGB by 0.3, so against the default fixture colour (black) it
    // writes back exactly what it read and the entire pass is invisible. Every
    // assertion in this file would then hold trivially, including the one meant
    // to catch that. Measured during development: the undo log recorded 710
    // entries of which 0 differed from the buffer.
    renderer.set_extrusion_color(lv_color_hex(0xC08040));

    renderer.set_ssao_enabled(ssao);
    renderer.set_canvas_size(kCanvas, kCanvas);
}

std::vector<uint8_t> snapshot(const uint8_t* buf) {
    return std::vector<uint8_t>(buf, buf + kBufBytes);
}

size_t differing_pixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    REQUIRE(a.size() == b.size());
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] || a[i + 3] != b[i + 3]) {
            ++n;
        }
    }
    return n;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "SSAO darkens something, so the rest of these tests mean something",
                 "[layer_renderer][ssao]") {
    // Guard against the whole suite passing vacuously because SSAO silently
    // stopped running: every "identical output" assertion below would hold
    // trivially if the pass were a no-op.
    auto gcode = make_layered_gcode(20);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer plain;
    configure(plain, gcode, /*ssao=*/false);
    plain.set_current_layer(19);
    drive(plain, canvas, buf);
    const auto without = snapshot(buf);

    GCodeLayerRenderer shaded;
    configure(shaded, gcode, /*ssao=*/true);
    shaded.set_current_layer(19);
    drive(shaded, canvas, buf);
    const auto with = snapshot(buf);

    const size_t diff = differing_pixels(without, with);
    INFO("pixels changed by the SSAO pass: " << diff);
    REQUIRE(diff > 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "rendering the same scene twice does not darken it twice",
                 "[layer_renderer][ssao]") {
    // The darkening is a multiply. Running it over its own output would give
    // 0.3 * 0.3, and the model would creep darker every time the cache was
    // revalidated. Nothing would crash and no test other than this one would
    // notice.
    auto gcode = make_layered_gcode(20);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer renderer;
    configure(renderer, gcode, /*ssao=*/true);
    renderer.set_current_layer(19);
    drive(renderer, canvas, buf);
    const auto once = snapshot(buf);

    // Scrub backwards and forwards again. That is a real user action (the layer
    // slider) and it takes the "target below what is cached" branch, which
    // clears and rebuilds, so the pass runs a second time over the same scene.
    renderer.set_current_layer(10);
    drive(renderer, canvas, buf);
    renderer.set_current_layer(19);
    drive(renderer, canvas, buf);
    const auto twice = snapshot(buf);

    CHECK(differing_pixels(once, twice) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "layers appended onto a shaded cache leave no stale shading behind",
                 "[layer_renderer][ssao]") {
    // THE case the undo log exists for. The cache grows a layer at a time during
    // a print. Pixels shaded when they were on the boundary stop being on the
    // boundary once the layers above them land, and without a restore they stay
    // dark: a band of shading that follows the print up the plate.
    auto gcode = make_layered_gcode(24);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    // Reference: one renderer that goes straight to the top.
    GCodeLayerRenderer direct;
    configure(direct, gcode, /*ssao=*/true);
    direct.set_current_layer(23);
    drive(direct, canvas, buf);
    const auto reference = snapshot(buf);

    // Subject: the same renderer, but it settles and shades at half height
    // first, then grows. Same final scene, so the same final pixels.
    GCodeLayerRenderer grown;
    configure(grown, gcode, /*ssao=*/true);
    grown.set_current_layer(11);
    drive(grown, canvas, buf);
    grown.set_current_layer(23);
    drive(grown, canvas, buf);
    const auto after_growth = snapshot(buf);

    const size_t diff = differing_pixels(reference, after_growth);
    INFO("pixels differing after progressive growth: " << diff);
    CHECK(diff == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "growing one layer at a time also leaves the cache clean",
                 "[layer_renderer][ssao]") {
    // The single-step version of the case above, which is what an actual print
    // does. Each step re-shades, so an unrestored pass would compound once per
    // layer rather than once overall.
    auto gcode = make_layered_gcode(16);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer direct;
    configure(direct, gcode, /*ssao=*/true);
    direct.set_current_layer(15);
    drive(direct, canvas, buf);
    const auto reference = snapshot(buf);

    GCodeLayerRenderer stepped;
    configure(stepped, gcode, /*ssao=*/true);
    for (int layer = 0; layer <= 15; ++layer) {
        stepped.set_current_layer(layer);
        drive(stepped, canvas, buf);
    }
    const auto after_steps = snapshot(buf);

    CHECK(differing_pixels(reference, after_steps) == 0);
}

/// Total luminance of every non-transparent pixel. Darkening lowers it, so this
/// gives the comparison a DIRECTION rather than just "these differ".
uint64_t luminance(const std::vector<uint8_t>& px) {
    uint64_t sum = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        if (px[i + 3] != 0) {
            sum += static_cast<uint64_t>(px[i]) + px[i + 1] + px[i + 2];
        }
    }
    return sum;
}

TEST_CASE_METHOD(LVGLTestFixture, "switching SSAO off undoes the shading, and back on redoes it",
                 "[layer_renderer][ssao]") {
    // The shading now lives IN the render cache, so switching off has to
    // actively put it back rather than just dropping a separate buffer.
    //
    // Note what this deliberately does NOT assert: that switching off matches a
    // renderer which never had SSAO on. It cannot, and never could.
    // ssao_enabled_ also selects Aa::On for the strokes themselves, so a cache
    // built with SSAO on holds ANTIALIASED geometry, and toggling the flag does
    // not re-render it. The AA fringe is several times larger than the shading,
    // so an equality assertion there fails on a difference that has nothing to
    // do with the undo log.
    auto gcode = make_layered_gcode(20);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer renderer;
    configure(renderer, gcode, /*ssao=*/true);
    renderer.set_current_layer(19);
    drive(renderer, canvas, buf);
    const auto shaded = snapshot(buf);

    renderer.set_ssao_enabled(false);
    drive(renderer, canvas, buf);
    const auto unshaded = snapshot(buf);

    renderer.set_ssao_enabled(true);
    drive(renderer, canvas, buf);
    const auto reshaded = snapshot(buf);

    // The shading was really taken back...
    const size_t undone = differing_pixels(shaded, unshaded);
    INFO("pixels restored by switching off: " << undone);
    CHECK(undone > 0);

    // ...in the lightening direction, which is what "undo a darkening" means.
    // Without this the test would pass if switching off corrupted the pixels
    // in any direction at all.
    CHECK(luminance(unshaded) > luminance(shaded));

    // ...and re-applying reproduces the original exactly. Restore and re-scan
    // are both exact, or these two would drift apart.
    CHECK(differing_pixels(shaded, reshaded) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "toggling SSAO repeatedly does not accumulate darkening",
                 "[layer_renderer][ssao]") {
    auto gcode = make_layered_gcode(16);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer renderer;
    configure(renderer, gcode, /*ssao=*/true);
    renderer.set_current_layer(15);
    drive(renderer, canvas, buf);
    const auto first = snapshot(buf);

    for (int i = 0; i < 4; ++i) {
        renderer.set_ssao_enabled(false);
        drive(renderer, canvas, buf);
        renderer.set_ssao_enabled(true);
        drive(renderer, canvas, buf);
    }
    drive(renderer, canvas, buf);
    const auto after = snapshot(buf);

    CHECK(differing_pixels(first, after) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "a canvas resize does not replay the log into the wrong pixels",
                 "[layer_renderer][ssao]") {
    // The log holds pixel OFFSETS. A resize reallocates the cache, so every
    // offset in it points somewhere meaningless. Replaying it would scatter old
    // colours across the new buffer; the log has to be dropped, not restored.
    auto gcode = make_layered_gcode(16);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer renderer;
    configure(renderer, gcode, /*ssao=*/true);
    renderer.set_current_layer(15);
    drive(renderer, canvas, buf);

    // Shrink and grow back. Both transitions reallocate.
    renderer.set_canvas_size(kCanvas / 2, kCanvas / 2);
    renderer.set_canvas_size(kCanvas, kCanvas);
    drive(renderer, canvas, buf);
    const auto after_resize = snapshot(buf);

    GCodeLayerRenderer fresh;
    configure(fresh, gcode, /*ssao=*/true);
    fresh.set_current_layer(15);
    drive(fresh, canvas, buf);
    const auto reference = snapshot(buf);

    CHECK(differing_pixels(reference, after_resize) == 0);
}
