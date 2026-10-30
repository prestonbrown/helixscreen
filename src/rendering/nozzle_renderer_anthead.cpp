// SPDX-License-Identifier: GPL-3.0-or-later
// Micron AntHead toolhead, drawn from assets/images/nozzle_anthead.png

#include "nozzle_renderer_anthead.h"

// The AntHead is one selectable toolhead-visualization style. ESP32 does not
// ship the image, so there draw_nozzle_anthead draws the default toolhead glyph
// instead, and toolhead_top_y() (ui_filament_path_glyphs.cpp) reports that
// glyph's top for ANTHEAD. Every other platform draws the image.
#if !defined(HELIX_PLATFORM_ESP32)

#include "data_root_resolver.h"

#include <spdlog/spdlog.h>

#include <image/lv_image_decoder_private.h>
#include <string>

const lv_draw_buf_t* anthead_image() {
    // Decoded once and kept for the process: LV_CACHE_DEF_SIZE is 0, so a path
    // source would be PNG-decoded again on every redraw. A failure is not retried.
    static const lv_draw_buf_t* decoded = []() -> const lv_draw_buf_t* {
        const std::string path = helix::asset_component_uri("assets/images/nozzle_anthead.png");
        lv_image_decoder_dsc_t dsc;
        if (lv_image_decoder_open(&dsc, path.c_str(), nullptr) != LV_RESULT_OK) {
            spdlog::warn("[AntHead] Failed to decode {}", path);
            return nullptr;
        }
        lv_draw_buf_t* copy = dsc.decoded ? lv_draw_buf_dup(dsc.decoded) : nullptr;
        lv_image_decoder_close(&dsc);
        if (!copy) {
            spdlog::warn("[AntHead] Failed to copy decoded {}", path);
        }
        return copy;
    }();
    return decoded;
}

// Source image dimensions and visual center (measured from content bounds)
static constexpr int32_t kImgW = 100;
static constexpr int32_t kImgH = 163;
static constexpr int32_t kImgCenterX = 50; // horizontal center of content
static constexpr int32_t kImgCenterY = 81; // vertical center of main body

void draw_nozzle_anthead(lv_layer_t* layer, int32_t cx, int32_t cy,
                         std::optional<lv_color_t> filament, int32_t scale_unit, lv_opa_t opa) {
    (void)filament;

    // Scale to match other toolhead renderers' visual footprint
    int32_t render_height = (scale_unit * 65) / 10;
    // LV_SCALE_NONE (256) = 1:1.  Scale maps source height to desired rendered height.
    int32_t scale = 256 * render_height / kImgH;
    const lv_draw_buf_t* img = anthead_image();
    if (scale <= 0 || !img)
        return;

    // Pivot is at the visual center of the toolhead body.  LVGL scales around
    // this point, keeping it stationary on screen.
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = img;
    dsc.opa = opa;
    dsc.scale_x = scale;
    dsc.scale_y = scale;
    dsc.pivot.x = kImgCenterX;
    dsc.pivot.y = kImgCenterY;

    // coords defines where the ORIGINAL (unscaled) image top-left goes.
    // The pivot stays at (coords.x1 + pivot.x, coords.y1 + pivot.y) after scaling,
    // so we offset to place the pivot at (cx, cy).
    lv_area_t coords;
    coords.x1 = cx - kImgCenterX;
    coords.y1 = cy - kImgCenterY;
    coords.x2 = coords.x1 + kImgW - 1;
    coords.y2 = coords.y1 + kImgH - 1;

    lv_draw_image(layer, &dsc, &coords);
}

#else // HELIX_PLATFORM_ESP32 - AntHead image not shipped (see top of file)

#include "nozzle_renderer_bambu.h"

void draw_nozzle_anthead(lv_layer_t* layer, int32_t cx, int32_t cy,
                         std::optional<lv_color_t> filament, int32_t scale_unit, lv_opa_t opa) {
    draw_nozzle_bambu(layer, cx, cy, filament, scale_unit, opa);
}

#endif // !HELIX_PLATFORM_ESP32
