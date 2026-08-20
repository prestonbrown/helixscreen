// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"

#include <algorithm>
#include <cstdio>
#include <glm/glm.hpp>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

// ---------------------------------------------------------------------------
// How much tonal range does the 2D renderer actually produce, per filament
// colour? Hidden behind [.]; run explicitly:
//
//   ./build/bin/helix-tests "[contrast_bench]"
//
// The complaint this measures: a black model renders as a flat silhouette with
// no readable form, far worse than the slicer's own embedded thumbnail of the
// same object.
//
// The suspected cause is that depth shading is applied as a pure MULTIPLY:
//
//     r = r * brightness;   // brightness in [0.34, 1.0]
//
// A multiply can only ever darken. At the brightest point on the model the
// factor is 1.0, which returns the base colour unchanged, so the lightest a
// pixel can be IS the filament colour. For black that is 0 * anything = 0, and
// every pixel of the object is identically black. The slicer thumbnail adds
// light rather than only subtracting it, which is why it shows form.
// ---------------------------------------------------------------------------

namespace {

constexpr int kCanvas = 240;
constexpr size_t kBufBytes = static_cast<size_t>(kCanvas) * kCanvas * 4;

/// A stepped tower: every layer is a ring, and the rings shrink with height, so
/// the model presents surfaces at many depths and orientations. That is what
/// shading has to differentiate.
ParsedGCodeFile make_cone(int layers) {
    ParsedGCodeFile gcode;
    const int16_t obj = gcode.intern_object_name("cone");
    for (int i = 0; i < layers; ++i) {
        Layer layer;
        const float z = 0.4f * static_cast<float>(i + 1);
        const float inset = static_cast<float>(i) * 0.6f;
        layer.z_height = z;
        const float lo = 20.0f + inset;
        const float hi = 70.0f - inset;
        if (hi - lo < 2.0f) {
            break;
        }
        const glm::vec3 c[4] = {{lo, lo, z}, {hi, lo, z}, {hi, hi, z}, {lo, hi, z}};
        for (int k = 0; k < 4; ++k) {
            ToolpathSegment seg;
            seg.start = c[k];
            seg.end = c[(k + 1) % 4];
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
    gcode.global_bounding_box.expand(glm::vec3(20.0f, 20.0f, 0.4f));
    gcode.global_bounding_box.expand(glm::vec3(70.0f, 70.0f, 0.4f * static_cast<float>(layers)));
    return gcode;
}

struct Contrast {
    int lo = 0;     ///< 5th percentile luminance of painted pixels
    int hi = 0;     ///< 95th percentile
    int spread = 0; ///< hi - lo, the readable tonal range
    size_t painted = 0;
    size_t distinct = 0; ///< how many distinct luminance values appear
};

Contrast measure(const uint8_t* buf) {
    std::vector<int> lum;
    int hist[256] = {0};
    for (size_t i = 0; i < kBufBytes; i += 4) {
        if (buf[i + 3] == 0) {
            continue;
        }
        // ARGB8888 in memory is B, G, R, A. Rec. 601 luma.
        const int y = (299 * buf[i + 2] + 587 * buf[i + 1] + 114 * buf[i]) / 1000;
        lum.push_back(y);
        ++hist[y];
    }
    Contrast c;
    c.painted = lum.size();
    if (lum.empty()) {
        return c;
    }
    std::sort(lum.begin(), lum.end());
    c.lo = lum[lum.size() * 5 / 100];
    c.hi = lum[lum.size() * 95 / 100];
    c.spread = c.hi - c.lo;
    for (int v : hist) {
        if (v) {
            ++c.distinct;
        }
    }
    return c;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "2D tonal range by filament colour", "[.][contrast_bench]") {
    auto gcode = make_cone(60);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    struct Filament {
        const char* name;
        uint32_t rgb;
    };
    const Filament filaments[] = {
        {"black", 0x000000},    {"near-black", 0x141414}, {"dark grey", 0x303030},
        {"mid grey", 0x808080}, {"orange", 0xE07030},     {"white", 0xF0F0F0},
    };

    std::printf("\n  %-11s %6s %6s %8s %10s %8s\n", "filament", "p5", "p95", "spread", "distinct",
                "painted");
    std::printf("  %-11s %6s %6s %8s %10s %8s\n", "-----------", "------", "------", "--------",
                "----------", "--------");

    for (const auto& f : filaments) {
        GCodeLayerRenderer r;
        r.set_gcode(&gcode);
        r.set_view_mode(GCodeLayerRenderer::ViewMode::FRONT);
        r.set_ghost_mode(false);
        r.set_ssao_enabled(true);
        r.set_antialias_enabled(false); // isolate shading from edge coverage
        r.set_extrusion_color(lv_color_hex(f.rgb));
        r.set_canvas_size(kCanvas, kCanvas);
        r.set_current_layer(59);

        lv_obj_update_layout(canvas);
        auto frame = [&]() {
            lv_layer_t layer;
            lv_area_t clip = {0, 0, kCanvas - 1, kCanvas - 1};
            lv_canvas_init_layer(canvas, &layer);
            r.render(&layer, &clip);
            lv_canvas_finish_layer(canvas, &layer);
            lv_timer_handler_safe();
        };
        for (int i = 0; i < 3; ++i) {
            frame();
        }
        int guard = 0;
        while (r.needs_more_frames() && guard++ < 500) {
            frame();
        }
        std::fill(buf, buf + kBufBytes, uint8_t{0});
        frame();

        const Contrast c = measure(buf);
        std::printf("  %-11s %6d %6d %8d %10zu %8zu\n", f.name, c.lo, c.hi, c.spread, c.distinct,
                    c.painted);
    }

    std::printf("\n  spread = p95 - p5 luminance over painted pixels. It is how much\n"
                "  tonal range the object actually shows; near zero reads as a flat\n"
                "  silhouette regardless of what colour it is.\n\n");
    SUCCEED();
}
