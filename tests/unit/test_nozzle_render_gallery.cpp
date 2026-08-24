// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nozzle_render_gallery.cpp
 * @brief Offline render gallery for all procedural toolhead/nozzle renderers
 *
 * Renders each of the 7 nozzle renderers to an offscreen ARGB8888 canvas and
 * writes the result to /tmp/nozzle-gallery/<name>.bmp for visual review.
 * Also computes the bounding box of non-background pixels so the render
 * extent and anchor offset relative to (cx, cy) are visible in the logs.
 *
 * Uses XMLTestFixture because the renderers call theme_manager_get_color()
 * (e.g. "filament_metal"), which requires theme_manager_init() to have run.
 *
 * Tagged [render-gallery][.] — hidden from default runs (it writes files to
 * /tmp); run explicitly with: ./build/bin/helix-tests "[render-gallery]"
 */

#include "../test_fixtures.h"
#include "nozzle_renderer_a4t.h"
#include "nozzle_renderer_anthead.h"
#include "nozzle_renderer_bambu.h"
#include "nozzle_renderer_creality_k1.h"
#include "nozzle_renderer_creality_k2.h"
#include "nozzle_renderer_jabberwocky.h"
#include "nozzle_renderer_stealthburner.h"
#include "screenshot.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

constexpr int32_t CANVAS_W = 600;
constexpr int32_t CANVAS_H = 600;
constexpr int32_t CENTER_X = CANVAS_W / 2;
constexpr int32_t CENTER_Y = CANVAS_H / 2;
// Z-offset indicator uses LV_CLAMP(5, h/10, 12). Scale 40 makes the narrowest
// renderer (creality_k2, ~2.5 units wide) exceed 100px while the tallest
// (stealthburner, ~8.5 units) still fits the 600px canvas.
constexpr int32_t SCALE_UNIT = 40;
constexpr uint32_t BG_HEX = 0x1A1A1A;       // dark neutral, matches app dark theme
constexpr uint32_t FILAMENT_HEX = 0xFF6A00; // visible orange

using DrawFn = void (*)(lv_layer_t*, int32_t, int32_t, lv_color_t, int32_t, lv_opa_t);

struct RendererEntry {
    const char* name;
    DrawFn draw;
};

const RendererEntry RENDERERS[] = {
    {"stealthburner", draw_nozzle_stealthburner},
    {"a4t", draw_nozzle_a4t},
    {"bambu", draw_nozzle_bambu},
    {"creality_k1", draw_nozzle_creality_k1},
    {"creality_k2", draw_nozzle_creality_k2},
    {"jabberwocky", draw_nozzle_jabberwocky},
    {"anthead", draw_nozzle_anthead},
};

struct BBox {
    int32_t min_x = INT32_MAX;
    int32_t min_y = INT32_MAX;
    int32_t max_x = -1;
    int32_t max_y = -1;

    bool empty() const {
        return max_x < 0;
    }
    int32_t width() const {
        return empty() ? 0 : (max_x - min_x + 1);
    }
    int32_t height() const {
        return empty() ? 0 : (max_y - min_y + 1);
    }
};

/// Scan tightly-packed BGRA pixels for anything that differs from the
/// background color (RGB compared, alpha ignored).
BBox compute_bbox(const std::vector<uint8_t>& bgra, int32_t w, int32_t h) {
    const uint8_t bg_b = BG_HEX & 0xFF;
    const uint8_t bg_g = (BG_HEX >> 8) & 0xFF;
    const uint8_t bg_r = (BG_HEX >> 16) & 0xFF;

    BBox box;
    for (int32_t y = 0; y < h; y++) {
        const uint8_t* row = bgra.data() + static_cast<size_t>(y) * w * 4;
        for (int32_t x = 0; x < w; x++) {
            const uint8_t* px = row + static_cast<size_t>(x) * 4;
            if (px[0] != bg_b || px[1] != bg_g || px[2] != bg_r) {
                if (x < box.min_x)
                    box.min_x = x;
                if (x > box.max_x)
                    box.max_x = x;
                if (y < box.min_y)
                    box.min_y = y;
                if (y > box.max_y)
                    box.max_y = y;
            }
        }
    }
    return box;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "Nozzle render gallery writes BMPs for all renderers",
                 "[render-gallery][.]") {
    const std::filesystem::path out_dir = "/tmp/nozzle-gallery";
    std::filesystem::create_directories(out_dir);

    // One shared canvas, repainted per renderer.
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    lv_draw_buf_t* draw_buf =
        lv_draw_buf_create(CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    REQUIRE(draw_buf != nullptr);
    lv_canvas_set_draw_buf(canvas, draw_buf);

    const lv_color_t filament = lv_color_hex(FILAMENT_HEX);

    for (const auto& entry : RENDERERS) {
        SECTION(entry.name) {
            lv_canvas_fill_bg(canvas, lv_color_hex(BG_HEX), LV_OPA_COVER);

            lv_layer_t layer;
            lv_canvas_init_layer(canvas, &layer);
            entry.draw(&layer, CENTER_X, CENTER_Y, filament, SCALE_UNIT, LV_OPA_COVER);
            lv_canvas_finish_layer(canvas, &layer);

            // Repack stride-padded rows into a tight BGRA buffer.
            const uint32_t stride = draw_buf->header.stride;
            std::vector<uint8_t> tight(static_cast<size_t>(CANVAS_W) * CANVAS_H * 4);
            for (int32_t y = 0; y < CANVAS_H; y++) {
                std::memcpy(tight.data() + static_cast<size_t>(y) * CANVAS_W * 4,
                            draw_buf->data + static_cast<size_t>(y) * stride,
                            static_cast<size_t>(CANVAS_W) * 4);
            }

            const BBox box = compute_bbox(tight, CANVAS_W, CANVAS_H);
            spdlog::info("[render-gallery] {}: bbox x=[{},{}] y=[{},{}] size={}x{} "
                         "anchor_offset=({},{}) scale_unit={}",
                         entry.name, box.min_x, box.max_x, box.min_y, box.max_y, box.width(),
                         box.height(), box.empty() ? 0 : (box.min_x + box.max_x) / 2 - CENTER_X,
                         box.empty() ? 0 : (box.min_y + box.max_y) / 2 - CENTER_Y, SCALE_UNIT);

            // Real assertion: the renderer must have drawn something substantial.
            REQUIRE_FALSE(box.empty());
            CHECK(box.width() >= 100);
            CHECK(box.height() >= 100);
            // Not clipped at canvas edges.
            CHECK(box.min_x > 0);
            CHECK(box.min_y > 0);
            CHECK(box.max_x < CANVAS_W - 1);
            CHECK(box.max_y < CANVAS_H - 1);

            const std::string path = (out_dir / (std::string(entry.name) + ".bmp")).string();
            REQUIRE(helix::write_bmp(path.c_str(), tight.data(), CANVAS_W, CANVAS_H));
            spdlog::info("[render-gallery] {}: wrote {}", entry.name, path);
        }
    }
}
