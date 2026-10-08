// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spool_canvas.h"

#include "ui_utils.h"

#include "ams_tray_projection.h"
#include "filament_tube_stroker.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lv_draw_buf_guard.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h" // not in the lvgl.h umbrella
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <list>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

// Geometry constants for Bambu-style 3D spool (SIDE VIEW)
// Spool axis is HORIZONTAL - we view from an angle
// Shows: back flange (left), filament cylinder (middle), front flange (right), hub hole (center)
// Flange size, depth skew and flange spacing are the tray's camera
// (ams_tray_projection.h), shared with the box and lid.
static constexpr float HUB_RADIUS = 0.10f; // Center hub hole radius
static constexpr int32_t DEFAULT_SIZE = 64;
static constexpr uint32_t DEFAULT_COLOR = 0xE0E0E0; // Default white/light filament

// Current-spool glow layer, grown from the spool's silhouette. Capable
// hardware gets a soft halo and a tight rim; reduced effects a solid outline.
namespace {
enum class SpoolGlow : uint8_t { None, Outline, Halo };
} // namespace
static constexpr int GLOW_HALO_DILATE = 3;
static constexpr int GLOW_HALO_BOX_R = 4; // three box passes ~ Gaussian sigma 4.5
static constexpr int GLOW_HALO_PASSES = 3;
static constexpr int GLOW_RIM_DILATE = 1;
static constexpr int GLOW_RIM_BOX_R = 1; // one box pass ~ Gaussian sigma 0.8
static constexpr uint8_t GLOW_RIM_LIGHTEN = 60;
static constexpr lv_opa_t GLOW_HALO_SECOND_OPA = 153; // 60%
static constexpr int32_t GLOW_MARGIN = GLOW_HALO_DILATE + GLOW_HALO_PASSES * GLOW_HALO_BOX_R;
static constexpr int32_t OUTLINE_PX = 2;
static constexpr uint8_t GLOW_DISC_SHAPE = 0xFF; // fill_bucket of a flat-style (disc) glow

// Note: Spool body colors now come from theme tokens in globals.xml:
// - spool_body: Front flange color
// - spool_body_shade: Back flange color (darker shade)
// - spool_hub_top, spool_hub_bottom: Center hub gradient

// ----------------------------------------------------------------------------
// sqrt LUT (idea #4)
// ----------------------------------------------------------------------------
// draw_gradient_ellipse / draw_gradient_rect / draw_ellipse_left_edge all call
// sqrtf() on values in [0,1] in their per-scanline inner loops. A 1024-entry
// table fits comfortably in L1 and collapses those to a single load + branch.
static constexpr int SQRT_LUT_SIZE = 1024;

static const std::array<float, SQRT_LUT_SIZE>& get_sqrt_lut() {
    static const std::array<float, SQRT_LUT_SIZE> table = []() {
        std::array<float, SQRT_LUT_SIZE> t{};
        for (int i = 0; i < SQRT_LUT_SIZE; ++i) {
            t[i] = sqrtf(static_cast<float>(i) / static_cast<float>(SQRT_LUT_SIZE - 1));
        }
        return t;
    }();
    return table;
}

static inline float fast_sqrt_01(float x) {
    if (x <= 0.0f)
        return 0.0f;
    if (x >= 1.0f)
        return 1.0f;
    int idx = static_cast<int>(x * static_cast<float>(SQRT_LUT_SIZE - 1));
    return get_sqrt_lut()[idx];
}

// ----------------------------------------------------------------------------
// Render cache (idea #2)
// ----------------------------------------------------------------------------
// Bucketed LRU of rendered ARGB pixel buffers keyed on (color, fill_bucket,
// size). Rendering a 64x64 spool costs ~10 gradient-fill scanline loops; a
// cache hit collapses that to a single memcpy (~16KB). Fill level is bucketed
// into 10% steps — visually indistinguishable at this scale, and it keeps the
// working set small while scrolling a list of many spools.
static constexpr size_t CACHE_MAX_ENTRIES = 128;
static constexpr int FILL_BUCKETS = 10;

struct SpoolCacheKey {
    uint32_t color_rgb; // 0x00RRGGBB
    uint16_t size;
    uint8_t fill_bucket; // 0..FILL_BUCKETS
    SpoolGlow glow;

    bool operator==(const SpoolCacheKey& o) const {
        return color_rgb == o.color_rgb && size == o.size && fill_bucket == o.fill_bucket &&
               glow == o.glow;
    }
};

struct SpoolCacheKeyHash {
    size_t operator()(const SpoolCacheKey& k) const {
        // Mix: color is the high-cardinality field, so weight it.
        return (static_cast<size_t>(k.color_rgb) * 0x9E3779B1u) ^
               (static_cast<size_t>(k.size) << 17) ^ (static_cast<size_t>(k.glow) << 8) ^
               static_cast<size_t>(k.fill_bucket);
    }
};

struct SpoolCacheEntry {
    SpoolCacheKey key;
    std::vector<uint8_t> pixels;
};

using SpoolCacheList = std::list<SpoolCacheEntry>;
using SpoolCacheIndex =
    std::unordered_map<SpoolCacheKey, SpoolCacheList::iterator, SpoolCacheKeyHash>;

static SpoolCacheList s_cache_list;
static SpoolCacheIndex s_cache_index;

static uint8_t compute_fill_bucket(float fill) {
    float clamped = fill < 0.0f ? 0.0f : (fill > 1.0f ? 1.0f : fill);
    return static_cast<uint8_t>(clamped * static_cast<float>(FILL_BUCKETS) + 0.5f);
}

static uint32_t color_to_rgb24(lv_color_t c) {
    return (static_cast<uint32_t>(c.red) << 16) | (static_cast<uint32_t>(c.green) << 8) |
           static_cast<uint32_t>(c.blue);
}

static const SpoolCacheEntry* cache_get(const SpoolCacheKey& key) {
    auto it = s_cache_index.find(key);
    if (it == s_cache_index.end())
        return nullptr;
    // Promote to MRU.
    s_cache_list.splice(s_cache_list.begin(), s_cache_list, it->second);
    return &(*it->second);
}

static void cache_put(const SpoolCacheKey& key, std::vector<uint8_t>&& pixels) {
    auto idx_it = s_cache_index.find(key);
    if (idx_it != s_cache_index.end()) {
        idx_it->second->pixels = std::move(pixels);
        s_cache_list.splice(s_cache_list.begin(), s_cache_list, idx_it->second);
        return;
    }
    if (s_cache_list.size() >= CACHE_MAX_ENTRIES) {
        s_cache_index.erase(s_cache_list.back().key);
        s_cache_list.pop_back();
    }
    s_cache_list.push_front({key, std::move(pixels)});
    s_cache_index[key] = s_cache_list.begin();
}

struct SpoolCanvasData {
    lv_obj_t* canvas = nullptr;
    lv_draw_buf_t* draw_buf = nullptr;
    int32_t size = DEFAULT_SIZE;
    lv_color_t color = lv_color_hex(DEFAULT_COLOR);
    float fill_level = 1.0f;

    // Last-rendered key, for dedup (idea #1). Valid only when has_rendered.
    SpoolCacheKey last_key{0, 0, 0, SpoolGlow::None};
    bool has_rendered = false;
};

static std::unordered_map<lv_obj_t*, SpoolCanvasData*> s_registry;

static SpoolCanvasData* get_data(lv_obj_t* obj) {
    auto it = s_registry.find(obj);
    return (it != s_registry.end()) ? it->second : nullptr;
}

// Size @p canvas's draw buffer to @p size plus @p margin on every side. The
// object keeps @p size so layout never moves; the image is centred on it and
// the overhang draws in the ext draw area.
static bool sync_canvas_buf(lv_obj_t* canvas, lv_draw_buf_t*& draw_buf, int32_t size,
                            int32_t margin) {
    const int32_t dim = size + 2 * margin;
    if (!draw_buf || static_cast<int32_t>(draw_buf->header.w) != dim ||
        static_cast<int32_t>(draw_buf->header.h) != dim) {
        lv_draw_buf_t* buf = lv_draw_buf_create(dim, dim, LV_COLOR_FORMAT_ARGB8888, 0);
        if (!buf) {
            spdlog::error("[SpoolCanvas] Failed to create draw buffer for size {}", dim);
            return false;
        }
        // Cleared before set_draw_buf: its image-source format probe reads the pixels.
        lv_draw_buf_clear(buf, nullptr);
        // Set the new buffer BEFORE destroying the old one: lv_canvas_set_draw_buf
        // drops the old image source from LVGL's cache, which reads its header.
        lv_draw_buf_t* old_buf = draw_buf;
        draw_buf = buf;
        lv_canvas_set_draw_buf(canvas, buf);
        helix::safe_draw_buf_destroy(old_buf, "spool");
    }
    // An image larger than its object is overhang, not scrollable content.
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_image_set_inner_align(canvas, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_size(canvas, size, size);
    lv_obj_refresh_ext_draw_size(canvas);
    return true;
}

static bool sync_draw_buf(SpoolCanvasData* d) {
    const lv_draw_buf_t* before = d->draw_buf;
    if (!sync_canvas_buf(d->canvas, d->draw_buf, d->size, 0))
        return false;
    if (d->draw_buf != before)
        d->has_rendered = false; // a fresh buffer holds no render yet
    return true;
}

// The spool is drawn as one fill per row span, hundreds per render. They are
// blended straight into the canvas's ARGB8888 buffer with the arithmetic of
// LVGL's software fill: queued as LVGL draw tasks, each one is checked against
// every earlier overlapping task before dispatch, which cost ~30ms per 64px
// spool on an ESP32-S3.
namespace {
struct SpoolLayer {
    lv_draw_buf_t* buf;
};
} // namespace

// lv_color_32_32_mix() from LVGL's ARGB8888 blender, which is file-static there.
static inline lv_color32_t spool_blend(lv_color32_t fg, lv_color32_t bg) {
    if (fg.alpha >= LV_OPA_MAX || bg.alpha <= LV_OPA_MIN)
        return fg;
    if (bg.alpha == 255)
        return lv_color_mix32(fg, bg);
    uint8_t res_alpha = 255 - LV_OPA_MIX2(255 - fg.alpha, 255 - bg.alpha);
    fg.alpha = static_cast<uint8_t>((static_cast<uint32_t>(fg.alpha) * 255) / res_alpha);
    lv_color32_t res = lv_color_mix32(fg, bg);
    res.alpha = res_alpha;
    return res;
}

static void spool_fill(SpoolLayer* l, const lv_draw_fill_dsc_t* dsc, const lv_area_t* area) {
    if (dsc->opa <= LV_OPA_MIN)
        return;
    const lv_image_header_t& h = l->buf->header;
    int32_t x1 = LV_MAX(area->x1, 0);
    int32_t x2 = LV_MIN(area->x2, static_cast<int32_t>(h.w) - 1);
    int32_t y1 = LV_MAX(area->y1, 0);
    int32_t y2 = LV_MIN(area->y2, static_cast<int32_t>(h.h) - 1);
    lv_color32_t fg = lv_color_to_32(dsc->color, dsc->opa >= LV_OPA_MAX ? LV_OPA_COVER : dsc->opa);
    for (int32_t y = y1; y <= y2; y++) {
        auto* row = reinterpret_cast<lv_color32_t*>(l->buf->data + y * h.stride);
        for (int32_t x = x1; x <= x2; x++)
            row[x] = spool_blend(fg, row[x]);
    }
}

// Draw ellipse with vertical gradient (top_color at top, bottom_color at bottom)
// Includes coverage-based anti-aliasing at left/right edges
static void draw_gradient_ellipse(SpoolLayer* layer, int32_t cx, int32_t cy, int32_t rx, int32_t ry,
                                  lv_color_t top_color, lv_color_t bottom_color) {
    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);

    for (int32_t y = -ry; y <= ry; y++) {
        float y_norm = (float)y / (float)ry;
        float x_extent = rx * fast_sqrt_01(1.0f - y_norm * y_norm);

        // Gradient factor: 0.0 at top (-ry), 1.0 at bottom (+ry)
        float gradient_factor = (float)(y + ry) / (float)(2 * ry);
        gradient_factor = fast_sqrt_01(gradient_factor); // Quick transition from light to dark
        fill_dsc.color = ams_draw::blend_color(top_color, bottom_color, gradient_factor);

        // Handle pole pixels (very narrow scanlines near top/bottom)
        if (x_extent < 0.5f) {
            // Draw single center pixel with proportional opacity
            float pole_opa = (x_extent > 0.01f) ? (x_extent * 2.0f) : 0.3f;
            fill_dsc.opa = (lv_opa_t)(pole_opa * 255.0f);
            lv_area_t pole_pixel = {cx, cy + y, cx, cy + y};
            spool_fill(layer, &fill_dsc, &pole_pixel);
            continue;
        }

        // Calculate integer bounds and fractional coverage
        int32_t x_inner = (int32_t)x_extent;
        float x_frac = x_extent - (float)x_inner;

        // Draw anti-aliased left/right edge pixels
        if (x_frac > 0.01f) {
            fill_dsc.opa = (lv_opa_t)(x_frac * 255.0f);
            lv_area_t left_edge = {cx - x_inner - 1, cy + y, cx - x_inner - 1, cy + y};
            spool_fill(layer, &fill_dsc, &left_edge);
            lv_area_t right_edge = {cx + x_inner + 1, cy + y, cx + x_inner + 1, cy + y};
            spool_fill(layer, &fill_dsc, &right_edge);
        }

        // Draw fully opaque interior
        if (x_inner > 0) {
            fill_dsc.opa = LV_OPA_COVER;
            lv_area_t line_area = {cx - x_inner, cy + y, cx + x_inner, cy + y};
            spool_fill(layer, &fill_dsc, &line_area);
        }
    }
}

// Draw rectangle with vertical gradient (top_color at top, bottom_color at bottom)
static void draw_gradient_rect(SpoolLayer* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                               lv_color_t top_color, lv_color_t bottom_color) {
    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.opa = LV_OPA_COVER;

    int32_t height = y2 - y1;
    if (height <= 0)
        return;

    // Draw scanline by scanline with gradient
    for (int32_t y = y1; y <= y2; y++) {
        float gradient_factor = (float)(y - y1) / (float)height;
        gradient_factor = fast_sqrt_01(gradient_factor); // Quick transition from light to dark
        fill_dsc.color = ams_draw::blend_color(top_color, bottom_color, gradient_factor);

        lv_area_t line = {x1, y, x2, y};
        spool_fill(layer, &fill_dsc, &line);
    }
}

// Draw a highlight edge along the LEFT side of an ellipse (simulates 3D thickness)
// width_px: how many pixels wide the highlight band is
static void draw_ellipse_left_edge(SpoolLayer* layer, int32_t cx, int32_t cy, int32_t rx,
                                   int32_t ry, lv_color_t top_color, lv_color_t bottom_color,
                                   int32_t width_px) {
    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);

    // Draw left edge highlight following ellipse curvature
    for (int32_t y = -ry; y <= ry; y++) {
        float y_norm = (float)y / (float)ry;
        float x_extent = rx * fast_sqrt_01(1.0f - y_norm * y_norm);
        if (x_extent < 0.5f)
            continue;

        // Gradient factor for vertical shading (same curve as main ellipse)
        float gradient_factor = (float)(y + ry) / (float)(2 * ry);
        gradient_factor = fast_sqrt_01(gradient_factor); // Quick transition from light to dark
        fill_dsc.color = ams_draw::blend_color(top_color, bottom_color, gradient_factor);
        fill_dsc.opa = LV_OPA_COVER;

        // Draw only the leftmost pixels (the edge highlight)
        // Use floorf to align with ellipse edge (truncation causes 1px offset at middle)
        int32_t left_edge = cx - (int32_t)floorf(x_extent + 0.5f);
        int32_t right_edge = left_edge + width_px - 1;
        if (right_edge > cx)
            right_edge = cx; // Don't go past center

        lv_area_t edge = {left_edge, cy + y, right_edge, cy + y};
        spool_fill(layer, &fill_dsc, &edge);
    }
}

// One axis of a separable filter over an alpha plane: max (dilate) or box
// mean (blur). Outside the plane reads as transparent.
static void alpha_pass(std::vector<uint8_t>& a, int32_t w, int32_t h, int r, bool horizontal,
                       bool dilate) {
    std::vector<uint8_t> out(a.size());
    const int32_t len = horizontal ? w : h;
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            const int32_t pos = horizontal ? x : y;
            uint32_t acc = 0;
            uint8_t mx = 0;
            for (int32_t k = std::max<int32_t>(0, pos - r);
                 k <= std::min<int32_t>(len - 1, pos + r); k++) {
                const uint8_t v = horizontal ? a[y * w + k] : a[k * w + x];
                acc += v;
                mx = std::max(mx, v);
            }
            out[y * w + x] = dilate ? mx : static_cast<uint8_t>(acc / (2 * r + 1));
        }
    }
    a.swap(out);
}

static std::vector<uint8_t> grow_and_soften(std::vector<uint8_t> a, int32_t w, int32_t h,
                                            int dilate_r, int box_r, int passes) {
    alpha_pass(a, w, h, dilate_r, true, true);
    alpha_pass(a, w, h, dilate_r, false, true);
    for (int i = 0; i < passes; i++) {
        alpha_pass(a, w, h, box_r, true, false);
        alpha_pass(a, w, h, box_r, false, false);
    }
    return a;
}

// Paint the glow for a padded alpha mask into @p buf (same dimensions): two
// accent halo passes (100%, then 60%) and a lighter tight rim, or under
// reduced effects the mask grown by OUTLINE_PX in solid accent.
static void paint_glow(lv_draw_buf_t* buf, const std::vector<uint8_t>& mask, SpoolGlow glow,
                       lv_color_t accent) {
    const int32_t w = buf->header.w;
    const int32_t h = buf->header.h;
    auto over = [](lv_color32_t fg, lv_color32_t bg) {
        return fg.alpha <= LV_OPA_MIN ? bg : spool_blend(fg, bg);
    };
    std::vector<uint8_t> halo, rim;
    if (glow == SpoolGlow::Halo) {
        halo = grow_and_soften(mask, w, h, GLOW_HALO_DILATE, GLOW_HALO_BOX_R, GLOW_HALO_PASSES);
        rim = grow_and_soften(mask, w, h, GLOW_RIM_DILATE, GLOW_RIM_BOX_R, 1);
    } else {
        halo = grow_and_soften(mask, w, h, OUTLINE_PX, 0, 0);
    }
    const lv_color_t rim_color = ams_draw::lighten_color(accent, GLOW_RIM_LIGHTEN);
    for (int32_t y = 0; y < h; y++) {
        auto* row = reinterpret_cast<lv_color32_t*>(buf->data + y * buf->header.stride);
        for (int32_t x = 0; x < w; x++) {
            const size_t i = static_cast<size_t>(y * w + x);
            lv_color32_t px = lv_color_to_32(accent, halo[i]);
            if (glow == SpoolGlow::Halo) {
                px = over(lv_color_to_32(accent, LV_OPA_MIX2(halo[i], GLOW_HALO_SECOND_OPA)), px);
                px = over(lv_color_to_32(rim_color, rim[i]), px);
            }
            row[x] = px;
        }
    }
}

// Actual pixel drawing. Callers should prefer redraw_spool(), which handles
// dedup and cache. This function unconditionally draws into data->draw_buf.
static void render_spool_pixels(SpoolCanvasData* data) {
    int32_t size = data->size;
    int32_t cy = size / 2; // Vertical center

    // Calculate dimensions - vertical radius and horizontal (compressed) radius
    int32_t flange_ry = (int32_t)(size * helix::ui::tray::SPOOL_FLANGE_RADIUS); // Vertical radius
    int32_t flange_rx = (int32_t)(flange_ry * helix::ui::tray::DEPTH_SKEW); // Horizontal (narrower)
    int32_t hub_ry = (int32_t)(size * HUB_RADIUS);
    int32_t hub_rx = (int32_t)(hub_ry * helix::ui::tray::DEPTH_SKEW);
    int32_t spool_width = (int32_t)(size * helix::ui::tray::SPOOL_WIDTH);

    // X positions for left (back) and right (front) flanges
    int32_t center_x = size / 2;
    int32_t left_x = center_x - spool_width / 2;  // Left side (back flange)
    int32_t right_x = center_x + spool_width / 2; // Right side (front flange)

    // Fill level determines wound filament radius
    // Max filament is smaller than flange so flanges always show as "taller"
    float fill = LV_CLAMP(data->fill_level, 0.0f, 1.0f);
    int32_t max_filament_ry = (int32_t)(flange_ry * 0.85f); // Flanges 15% taller than full filament
    int32_t filament_ry = hub_ry + (int32_t)((max_filament_ry - hub_ry) * fill);
    int32_t filament_rx = (int32_t)(filament_ry * helix::ui::tray::DEPTH_SKEW);

    // Colors (from theme tokens)
    lv_color_t back_color = theme_manager_get_color("spool_body_shade");
    lv_color_t front_color = theme_manager_get_color("spool_body");
    lv_color_t filament_color = data->color;
    lv_color_t filament_side = ams_draw::darken_color(filament_color, 30);

    // Clear canvas
    lv_canvas_fill_bg(data->canvas, lv_color_black(), LV_OPA_TRANSP);

    SpoolLayer layer{data->draw_buf};

    // ========================================
    // STEP 1: Draw BACK FLANGE (left side) with gradient + edge highlight
    // ========================================
    {
        lv_color_t bf_light = ams_draw::lighten_color(back_color, 40);
        lv_color_t bf_dark = ams_draw::darken_color(back_color, 25);
        // Main flange ellipse with gradient
        draw_gradient_ellipse(&layer, left_x, cy, flange_rx, flange_ry, bf_light, bf_dark);
        // Edge highlight on left side (gives 3D thickness illusion)
        // Dramatic gradient: very bright at top, dark at bottom
        lv_color_t edge_light = ams_draw::lighten_color(back_color, 100);
        lv_color_t edge_dark = ams_draw::darken_color(back_color, 40);
        draw_ellipse_left_edge(&layer, left_x, cy, flange_rx, flange_ry, edge_light, edge_dark, 2);
    }

    // ========================================
    // STEP 2: Draw complete FILAMENT cylinder
    // Back ellipse + rectangle body + front ellipse
    // Gradient: lighter at top (lit), darker at bottom (shadow)
    // ========================================
    if (fill > 0.01f) {
        // Gradient colors for 3D lighting effect (stronger gradient)
        lv_color_t fil_light = ams_draw::lighten_color(filament_side, 70); // Top: much brighter
        lv_color_t fil_dark = ams_draw::darken_color(filament_side, 35);   // Bottom: darker

        // 2a: Back face ellipse (with gradient)
        draw_gradient_ellipse(&layer, left_x, cy, filament_rx, filament_ry, fil_light, fil_dark);

        // 2b: Rectangle body connecting the two faces (with gradient)
        int32_t fil_top = cy - filament_ry;
        int32_t fil_bottom = cy + filament_ry;
        draw_gradient_rect(&layer, left_x, fil_top, right_x, fil_bottom, fil_light, fil_dark);

        // 2c: Front face ellipse (will be covered by front flange anyway)
        lv_color_t front_light = ams_draw::lighten_color(filament_color, 70);
        lv_color_t front_dark = ams_draw::darken_color(filament_color, 35);
        draw_gradient_ellipse(&layer, right_x, cy, filament_rx, filament_ry, front_light,
                              front_dark);
    }

    // ========================================
    // STEP 3: Draw FRONT FLANGE (right side) with gradient + edge highlight
    // ========================================
    {
        lv_color_t ff_light = ams_draw::lighten_color(front_color, 40);
        lv_color_t ff_dark = ams_draw::darken_color(front_color, 25);
        // Main flange ellipse with gradient
        draw_gradient_ellipse(&layer, right_x, cy, flange_rx, flange_ry, ff_light, ff_dark);
        // Edge highlight on left side (gives 3D thickness illusion)
        // Dramatic gradient: very bright at top, dark at bottom
        lv_color_t edge_light = ams_draw::lighten_color(front_color, 100);
        lv_color_t edge_dark = ams_draw::darken_color(front_color, 40);
        draw_ellipse_left_edge(&layer, right_x, cy, flange_rx, flange_ry, edge_light, edge_dark, 2);
    }

    // ========================================
    // STEP 4: Draw CENTER HOLE ellipse (hub)
    // Stronger gradient: dark at top (deep shadow), lighter at bottom (illuminated)
    // ========================================
    lv_color_t hub_top =
        theme_manager_get_color("spool_hub_top"); // Nearly black at top (deep in shadow)
    lv_color_t hub_bottom =
        theme_manager_get_color("spool_hub_bottom"); // Noticeably lighter at bottom (light hits it)
    draw_gradient_ellipse(&layer, right_x, cy, hub_rx, hub_ry, hub_top, hub_bottom);

    lv_obj_invalidate(data->canvas);

    spdlog::trace("[SpoolCanvas] Redrawn: size={}, fill={:.0f}%", size, fill * 100.0f);
}

static void repaint_bound_glows(lv_obj_t* spool_canvas);
static void unbind_glows(lv_obj_t* spool_canvas);
static void clear_glow_cache();

static bool redraw_spool_pixels(SpoolCanvasData* data);

// Redraw entry point. A new silhouette repaints the glows bound to this spool.
static void redraw_spool(SpoolCanvasData* data) {
    if (redraw_spool_pixels(data))
        repaint_bound_glows(data->canvas);
}

// Cache-aware render. Handles:
//   - dedup (idea #1): same state as last render → no-op
//   - cache hit (idea #2): memcpy cached pixels into draw_buf
//   - cache miss: full render + capture into cache
// Returns true when the buffer now holds a different render.
static bool redraw_spool_pixels(SpoolCanvasData* data) {
    if (!data || !data->canvas || !data->draw_buf)
        return false;

    SpoolCacheKey key{color_to_rgb24(data->color), static_cast<uint16_t>(data->size),
                      compute_fill_bucket(data->fill_level), SpoolGlow::None};

    // Dedup: already rendered this bucketed state, nothing to do.
    if (data->has_rendered && data->last_key == key) {
        return false;
    }

    // Cache hit: copy rendered pixels into the canvas buffer.
    if (const SpoolCacheEntry* entry = cache_get(key); entry != nullptr) {
        if (data->draw_buf->data && entry->pixels.size() == data->draw_buf->data_size) {
            memcpy(data->draw_buf->data, entry->pixels.data(), entry->pixels.size());
            lv_obj_invalidate(data->canvas);
            data->last_key = key;
            data->has_rendered = true;
            spdlog::trace("[SpoolCanvas] cache hit (size={} bucket={} color=0x{:06X})", key.size,
                          key.fill_bucket, key.color_rgb);
            return true;
        }
    }

    // Cache miss: render and capture.
    render_spool_pixels(data);

    if (data->draw_buf->data && data->draw_buf->data_size > 0) {
        std::vector<uint8_t> snapshot(data->draw_buf->data,
                                      data->draw_buf->data + data->draw_buf->data_size);
        cache_put(key, std::move(snapshot));
    }

    data->last_key = key;
    data->has_rendered = true;
    return true;
}

static void spool_canvas_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        lv_obj_t* obj = lv_event_get_target_obj(e);
        auto it = s_registry.find(obj);
        if (it != s_registry.end()) {
            std::unique_ptr<SpoolCanvasData> data(it->second);
            // The canvas was a composite source until this delete; a blend of
            // it may still be in flight.
            if (data) {
                helix::safe_draw_buf_destroy(data->draw_buf, "spool");
            }
            unbind_glows(obj);
            lv_obj_set_user_data(obj, nullptr);
            s_registry.erase(it);
            // data automatically freed
        }
    }
}

static void* spool_canvas_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* canvas = lv_canvas_create(static_cast<lv_obj_t*>(parent));
    if (!canvas)
        return nullptr;

    auto data_ptr = std::make_unique<SpoolCanvasData>();
    data_ptr->canvas = canvas;
    data_ptr->size = DEFAULT_SIZE;
    data_ptr->color = lv_color_hex(DEFAULT_COLOR);
    data_ptr->fill_level = 1.0f;

    sync_draw_buf(data_ptr.get());
    s_registry[canvas] = data_ptr.get();
    lv_obj_add_event_cb(canvas, spool_canvas_event_cb, LV_EVENT_DELETE, nullptr);

    SpoolCanvasData* data = data_ptr.release();

    redraw_spool(data);

    spdlog::debug("[SpoolCanvas] Created widget");
    return canvas;
}

static void spool_canvas_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = static_cast<lv_obj_t*>(item);
    if (!obj)
        return;

    lv_xml_obj_apply(state, attrs);

    auto* data = get_data(obj);
    if (!data)
        return;

    bool needs_redraw = false;

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "color") == 0) {
            uint32_t hex = strtoul(value, nullptr, 0);
            data->color = lv_color_hex(hex);
            needs_redraw = true;
            spdlog::debug("[SpoolCanvas] Set color=0x{:06X}", hex);
        } else if (strcmp(name, "fill_level") == 0) {
            data->fill_level = strtof(value, nullptr);
            needs_redraw = true;
            spdlog::debug("[SpoolCanvas] Set fill_level={:.2f}", data->fill_level);
        } else if (strcmp(name, "size") == 0) {
            int32_t new_size = atoi(value);
            if (new_size != data->size && new_size > 0) {
                data->size = new_size;
                sync_draw_buf(data);
                needs_redraw = true;
                spdlog::debug("[SpoolCanvas] Set size={}", new_size);
            }
        }
    }

    if (needs_redraw) {
        redraw_spool(data);
    }
}

void ui_spool_canvas_register(void) {
    lv_xml_register_widget("spool_canvas", spool_canvas_xml_create, spool_canvas_xml_apply);
    spdlog::debug("[SpoolCanvas] Registered spool_canvas widget with XML system");
}

lv_obj_t* ui_spool_canvas_create(lv_obj_t* parent, int32_t size) {
    if (!parent) {
        spdlog::error("[SpoolCanvas] Cannot create: parent is null");
        return nullptr;
    }

    // Use default size if not specified
    if (size <= 0) {
        size = DEFAULT_SIZE;
    }

    lv_obj_t* canvas = lv_canvas_create(parent);
    if (!canvas) {
        spdlog::error("[SpoolCanvas] Failed to create canvas");
        return nullptr;
    }

    auto data_ptr = std::make_unique<SpoolCanvasData>();
    data_ptr->canvas = canvas;
    data_ptr->size = size;
    data_ptr->color = lv_color_hex(DEFAULT_COLOR);
    data_ptr->fill_level = 1.0f;

    if (!sync_draw_buf(data_ptr.get())) {
        helix::ui::safe_delete(canvas);
        return nullptr;
    }
    s_registry[canvas] = data_ptr.get();
    lv_obj_add_event_cb(canvas, spool_canvas_event_cb, LV_EVENT_DELETE, nullptr);

    SpoolCanvasData* data = data_ptr.release();

    redraw_spool(data);

    spdlog::debug("[SpoolCanvas] Created widget programmatically (size={})", size);
    return canvas;
}

void ui_spool_canvas_set_color(lv_obj_t* canvas, lv_color_t color) {
    auto* data = get_data(canvas);
    if (!data)
        return;
    // Short-circuit: same color → no state change, skip the redraw pipeline
    // entirely (the cache-aware redraw would dedup, but this saves the hash
    // + lookup on the common scroll-recycle-same-color path).
    if (data->has_rendered && data->color.red == color.red && data->color.green == color.green &&
        data->color.blue == color.blue) {
        return;
    }
    data->color = color;
    redraw_spool(data);
}

void ui_spool_canvas_set_fill_level(lv_obj_t* canvas, float fill_level) {
    auto* data = get_data(canvas);
    if (!data)
        return;
    float clamped = LV_CLAMP(fill_level, 0.0f, 1.0f);
    data->fill_level = clamped;
    // redraw_spool() will dedup on bucket; no need to skip here since the
    // bucket comparison is the only thing that actually matters visually.
    redraw_spool(data);
}

void ui_spool_canvas_set_size(lv_obj_t* canvas, int32_t size) {
    auto* data = get_data(canvas);
    if (!data || size <= 0 || size == data->size)
        return;

    const int32_t old_size = data->size;
    data->size = size;
    if (!sync_draw_buf(data)) {
        data->size = old_size;
        return;
    }
    redraw_spool(data);
}

void ui_spool_canvas_redraw(lv_obj_t* canvas) {
    auto* data = get_data(canvas);
    if (data) {
        redraw_spool(data);
    }
}

float ui_spool_canvas_get_fill_level(lv_obj_t* canvas) {
    auto* data = get_data(canvas);
    return data ? data->fill_level : -1.0f;
}

lv_color_t ui_spool_canvas_get_color(lv_obj_t* canvas) {
    auto* data = get_data(canvas);
    return data ? data->color : lv_color_hex(DEFAULT_COLOR);
}

void ui_spool_canvas_invalidate_cache(void) {
    s_cache_list.clear();
    s_cache_index.clear();
    clear_glow_cache();
    for (auto& [canvas, data] : s_registry) {
        if (data) {
            data->has_rendered = false;
        }
    }
    spdlog::debug("[SpoolCanvas] Render cache invalidated");
}

// ----------------------------------------------------------------------------
// Glow layer
// ----------------------------------------------------------------------------
// A glow is one image per (shape, accent, simple), shared by every layer that
// shows it: the cache and the layers hold references, so a buffer lives
// exactly as long as something needs it. Glows keep their own small LRU so
// they never evict spool renders.
static constexpr size_t GLOW_CACHE_MAX_ENTRIES = 4;

namespace {
using GlowImage = std::shared_ptr<lv_draw_buf_t>;

struct GlowCacheEntry {
    SpoolCacheKey key;
    GlowImage image;
};

struct SpoolGlowData {
    lv_obj_t* spool = nullptr; ///< bound 3D spool canvas; null = disc (flat style)
    int32_t size = 0;
    bool simple = false;
    bool lit = false; ///< painted, or waiting on the spool's first render
    GlowImage image;
    int32_t margin = 0;
};
} // namespace

static std::list<GlowCacheEntry> s_glow_cache;
static std::unordered_map<lv_obj_t*, SpoolGlowData> s_glow_registry;

static GlowImage make_glow_image(int32_t dim) {
    lv_draw_buf_t* buf = lv_draw_buf_create(dim, dim, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!buf)
        return nullptr;
    return GlowImage(buf, [](lv_draw_buf_t* b) {
        if (!lv_is_initialized())
            return; // process teardown: LVGL's image cache is already gone
        // A layer that showed it may still be in an in-flight blend.
        lv_image_cache_drop(b);
        helix::safe_draw_buf_destroy(b, "spool_glow");
    });
}

static GlowImage glow_cache_get(const SpoolCacheKey& key) {
    for (auto it = s_glow_cache.begin(); it != s_glow_cache.end(); ++it) {
        if (it->key == key) {
            s_glow_cache.splice(s_glow_cache.begin(), s_glow_cache, it);
            return it->image;
        }
    }
    return nullptr;
}

static void glow_cache_put(const SpoolCacheKey& key, GlowImage image) {
    s_glow_cache.push_front({key, std::move(image)});
    if (s_glow_cache.size() > GLOW_CACHE_MAX_ENTRIES)
        s_glow_cache.pop_back();
}

// Layers keep the images they show; only the cache's references go.
static void clear_glow_cache() {
    s_glow_cache.clear();
}

static void set_glow_image(lv_obj_t* glow, SpoolGlowData& g, GlowImage image, int32_t margin) {
    g.image = std::move(image);
    g.margin = g.image ? margin : 0;
    lv_image_set_src(glow, g.image.get());
    if (g.size > 0)
        lv_obj_set_size(glow, g.size, g.size);
    helix::ui::refresh_overhang_chain(glow);
}

// The silhouette, padded by @p m: the 3D canvas's own alpha, or a disc of
// diameter @p size for the flat style.
static std::vector<uint8_t> silhouette_mask(SpoolCanvasData* spool, int32_t size, int32_t m) {
    const int32_t dim = size + 2 * m;
    std::vector<uint8_t> mask(static_cast<size_t>(dim * dim), 0);
    if (spool && spool->draw_buf) {
        const lv_image_header_t& h = spool->draw_buf->header;
        for (int32_t y = 0; y < std::min<int32_t>(h.h, size); y++) {
            const auto* row =
                reinterpret_cast<const lv_color32_t*>(spool->draw_buf->data + y * h.stride);
            for (int32_t x = 0; x < std::min<int32_t>(h.w, size); x++)
                mask[(y + m) * dim + x + m] = row[x].alpha;
        }
        return mask;
    }
    const float c = static_cast<float>(dim - 1) / 2.0f;
    const float r = static_cast<float>(size) / 2.0f;
    for (int32_t y = 0; y < dim; y++) {
        for (int32_t x = 0; x < dim; x++) {
            const float d = sqrtf((x - c) * (x - c) + (y - c) * (y - c));
            mask[y * dim + x] = static_cast<uint8_t>(LV_CLAMP(r - d + 0.5f, 0.0f, 1.0f) * 255.0f);
        }
    }
    return mask;
}

// Show the glow for the layer's bound shape. A 3D spool that has not rendered
// yet has no silhouette to read: the layer stays empty until its first render.
static void paint_glow_layer(lv_obj_t* glow, SpoolGlowData& g) {
    SpoolCanvasData* spool = get_data(g.spool);
    if (g.spool && (!spool || !spool->has_rendered)) {
        set_glow_image(glow, g, nullptr, 0);
        return;
    }
    const SpoolGlow mode = g.simple ? SpoolGlow::Outline : SpoolGlow::Halo;
    const int32_t margin = g.simple ? OUTLINE_PX : GLOW_MARGIN;
    const lv_color_t accent = helix::ui::tube_accent();
    // A glow depends only on the silhouette, never the filament color, so the
    // key carries the accent where a spool render carries its color.
    const SpoolCacheKey key{color_to_rgb24(accent), static_cast<uint16_t>(g.size),
                            spool ? spool->last_key.fill_bucket : GLOW_DISC_SHAPE, mode};
    GlowImage image = glow_cache_get(key);
    if (!image) {
        image = make_glow_image(g.size + 2 * margin);
        if (!image) {
            spdlog::error("[SpoolCanvas] Failed to create glow buffer for size {}", g.size);
            return;
        }
        paint_glow(image.get(), silhouette_mask(spool, g.size, margin), mode, accent);
        glow_cache_put(key, image);
    }
    if (image != g.image)
        set_glow_image(glow, g, std::move(image), margin);
}

static void repaint_bound_glows(lv_obj_t* spool_canvas) {
    for (auto& [glow, g] : s_glow_registry)
        if (g.lit && g.spool == spool_canvas)
            paint_glow_layer(glow, g);
}

static void unbind_glows(lv_obj_t* spool_canvas) {
    for (auto& [glow, g] : s_glow_registry) {
        if (g.spool == spool_canvas) {
            g.spool = nullptr;
            g.lit = false;
            set_glow_image(glow, g, nullptr, 0);
        }
    }
}

static void spool_glow_event_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    auto it = s_glow_registry.find(obj);
    if (it == s_glow_registry.end())
        return;
    if (lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        lv_event_set_ext_draw_size(e, it->second.margin);
    } else if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        s_glow_registry.erase(it);
    }
}

namespace helix::ui {

lv_obj_t* spool_glow_create(lv_obj_t* parent) {
    lv_obj_t* glow = lv_image_create(parent);
    if (!glow)
        return nullptr;
    lv_obj_remove_flag(glow, LV_OBJ_FLAG_CLICKABLE);
    // An image larger than its object is overhang, not scrollable content.
    lv_obj_remove_flag(glow, LV_OBJ_FLAG_SCROLLABLE);
    lv_image_set_inner_align(glow, LV_IMAGE_ALIGN_CENTER);
    s_glow_registry[glow] = SpoolGlowData{};
    lv_obj_add_event_cb(glow, spool_glow_event_cb, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(glow, spool_glow_event_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
    return glow;
}

void spool_glow_paint(lv_obj_t* glow, lv_obj_t* spool_canvas, int32_t size, bool simple) {
    auto it = s_glow_registry.find(glow);
    if (it == s_glow_registry.end() || size <= 0)
        return;
    SpoolGlowData& g = it->second;
    g.spool = spool_canvas;
    g.size = size;
    g.simple = simple;
    g.lit = true;
    paint_glow_layer(glow, g);
}

void spool_glow_clear(lv_obj_t* glow) {
    auto it = s_glow_registry.find(glow);
    if (it == s_glow_registry.end())
        return;
    it->second.lit = false;
    it->second.spool = nullptr;
    set_glow_image(glow, it->second, nullptr, 0);
}

} // namespace helix::ui
