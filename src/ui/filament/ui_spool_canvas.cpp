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
#include "spool_vents.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"
#include "widget_data_registry.h"

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
static constexpr float kHubRadius = 0.10f; // Center hub hole radius
static constexpr int32_t kDefaultSize = 64;
static constexpr uint32_t kDefaultColor = 0xE0E0E0; // Default white/light filament

// The key light comes from above and in front, falling kLightDrop for each
// unit it travels into the spool along its axis.
static constexpr float kLightDrop = 1.0f;
// The flanges' plastic under it: lifted toward white at the top, pressed
// toward black at the foot.
static constexpr uint8_t kFlangeLift = 60;
static constexpr uint8_t kFlangeDrop = 40;

// Vents in the front flange (spool_vents.h). Seen through a slot, wound
// filament is in the flange's shadow, and an empty spool's far flange darker
// still. The lip is the shadow the flange's thickness casts along each cut's
// top, under the key light.
static constexpr uint8_t kVentFilamentShadeTop = 30;
static constexpr uint8_t kVentFilamentShadeBottom = 110;
static constexpr uint8_t kVentInsideShadeTop = 20;
static constexpr uint8_t kVentInsideShadeBottom = 50;
static constexpr float kFlangeThickness = 3.0f / 240.0f; // 3 px on a 240 px spool
static constexpr float kVentLipFraction = kFlangeThickness * kLightDrop;
static constexpr lv_opa_t kVentLipShade = 115; // 45% toward black
// Face-on, the flange is smoked plastic over the wound filament.
static constexpr lv_opa_t kFrontSmokeOpa = 140;   // 55% flange over 45% filament
static constexpr float kFrontMaxFilament = 0.85f; // a full spool stops short of the rim

// Current-spool glow layer, grown from the spool's silhouette. Capable
// hardware gets a soft halo and a tight rim; reduced effects a solid outline.
namespace {
enum class SpoolGlow : uint8_t { None, Outline, Halo };
} // namespace
static constexpr int kGlowHaloDilate = 3;
static constexpr int kGlowHaloBoxR = 4; // three box passes ~ Gaussian sigma 4.5
static constexpr int kGlowHaloPasses = 3;
static constexpr int kGlowRimDilate = 1;
static constexpr int kGlowRimBoxR = 1; // one box pass ~ Gaussian sigma 0.8
static constexpr uint8_t kGlowRimLighten = 60;
static constexpr lv_opa_t kGlowHaloSecondOpa = 153; // 60%
static constexpr int32_t kGlowMargin = kGlowHaloDilate + kGlowHaloPasses * kGlowHaloBoxR;
static constexpr int32_t kOutlinePx = 2;

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
static constexpr int kSqrtLutSize = 1024;

static const std::array<float, kSqrtLutSize>& get_sqrt_lut() {
    static const std::array<float, kSqrtLutSize> table = []() {
        std::array<float, kSqrtLutSize> t{};
        for (int i = 0; i < kSqrtLutSize; ++i) {
            t[i] = sqrtf(static_cast<float>(i) / static_cast<float>(kSqrtLutSize - 1));
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
    int idx = static_cast<int>(x * static_cast<float>(kSqrtLutSize - 1));
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
static constexpr size_t kCacheMaxEntries = 128;
static constexpr int kFillBuckets = 10;
// At or below this the spool is drawn empty: no filament, its bare core.
// Bucket 0 holds only empty spools, since they render differently from the
// lowest filled ones.
static constexpr float kEmptyFill = 0.01f;

struct SpoolCacheKey {
    uint32_t color_rgb; // 0x00RRGGBB
    uint16_t size;
    uint8_t fill_bucket; // 0..kFillBuckets
    SpoolGlow glow;
    uint32_t filament_hash = 0; // multi-colour list and finish; 0 for a plain single colour
    helix::ui::SpoolView view = helix::ui::SpoolView::Side;

    bool operator==(const SpoolCacheKey& o) const {
        return color_rgb == o.color_rgb && size == o.size && fill_bucket == o.fill_bucket &&
               glow == o.glow && filament_hash == o.filament_hash && view == o.view;
    }
};

struct SpoolCacheKeyHash {
    size_t operator()(const SpoolCacheKey& k) const {
        // Mix: color is the high-cardinality field, so weight it.
        return (static_cast<size_t>(k.color_rgb) * 0x9E3779B1u) ^
               (static_cast<size_t>(k.size) << 17) ^ (static_cast<size_t>(k.glow) << 8) ^
               static_cast<size_t>(k.fill_bucket) ^ (static_cast<size_t>(k.filament_hash) << 3) ^
               (static_cast<size_t>(k.view) << 29);
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
    if (clamped <= kEmptyFill)
        return 0;
    return static_cast<uint8_t>(std::max(1.0f, clamped * static_cast<float>(kFillBuckets) + 0.5f));
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
    if (s_cache_list.size() >= kCacheMaxEntries) {
        s_cache_index.erase(s_cache_list.back().key);
        s_cache_list.pop_back();
    }
    s_cache_list.push_front({key, std::move(pixels)});
    s_cache_index[key] = s_cache_list.begin();
}

struct SpoolCanvasData {
    lv_obj_t* canvas = nullptr;
    lv_draw_buf_t* draw_buf = nullptr;
    int32_t size = kDefaultSize;
    lv_color_t color = lv_color_hex(kDefaultColor);
    float fill_level = 1.0f;
    /// A multi-colour filament's colours (two or more), else empty: the body
    /// is split the way every swatch splits (helix::ui::swatch_split_thresholds).
    std::vector<lv_color_t> colors;
    helix::ui::SwatchFinish finish = helix::ui::SwatchFinish::Plain;
    helix::ui::SpoolView view = helix::ui::SpoolView::Side;

    // Last-rendered key, for dedup (idea #1). Valid only when has_rendered.
    SpoolCacheKey last_key{0, 0, 0, SpoolGlow::None};
    bool has_rendered = false;
};

static helix::WidgetDataRegistry<SpoolCanvasData> s_registry;

static SpoolCanvasData* get_data(lv_obj_t* obj) {
    return s_registry.get(obj);
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

// The vents' see-through colour shades from @p top at the flange's top to
// @p bottom at its foot, the way the flange itself does.
namespace {
struct VentBacking {
    lv_color_t top;
    lv_color_t bottom;
};
} // namespace

// Cut @p face's vents into what is already drawn: each open pixel shows
// @p inside(x, y) (what lies behind the flange there), with a darker lip along
// each cut's upper edge where the flange's thickness shades it.
template <typename Inside> // VentBacking(float face_radius)
static void paint_vents(SpoolLayer* layer, const helix::ui::SpoolVentFace& face,
                        const lv_area_t& bounds, const Inside& inside) {
    if (face.detail == helix::ui::SpoolVentDetail::None)
        return;
    const lv_image_header_t& h = layer->buf->header;
    const int32_t x1 = LV_MAX(bounds.x1, 0);
    const int32_t x2 = LV_MIN(bounds.x2, static_cast<int32_t>(h.w) - 1);
    const int32_t y1 = LV_MAX(bounds.y1, 0);
    const int32_t y2 = LV_MIN(bounds.y2, static_cast<int32_t>(h.h) - 1);
    const float height = static_cast<float>(LV_MAX(bounds.y2 - bounds.y1, 1));
    const float lip = std::max(1.0f, face.size * kVentLipFraction);
    for (int32_t y = y1; y <= y2; y++) {
        auto* row = reinterpret_cast<lv_color32_t*>(layer->buf->data + y * h.stride);
        const float py = static_cast<float>(y) + 0.5f;
        const float t = static_cast<float>(y - bounds.y1) / height;
        for (int32_t x = x1; x <= x2; x++) {
            const float pxc = static_cast<float>(x) + 0.5f;
            const float cover = helix::ui::spool_vent_coverage(face, pxc, py);
            if (cover <= 0.0f)
                continue;
            const VentBacking back = inside(helix::ui::spool_vent_face_radius(face, pxc, py));
            lv_color_t seen = ams_draw::blend_color(back.top, back.bottom, t);
            if (helix::ui::spool_vent_coverage(face, pxc, py - lip) < 1.0f)
                seen = lv_color_mix(lv_color_black(), seen, kVentLipShade);
            const lv_color32_t fg =
                lv_color_to_32(seen, static_cast<lv_opa_t>(cover * 255.0f + 0.5f));
            const uint8_t alpha = row[x].alpha;
            row[x] = lv_color_mix32(fg, row[x]);
            row[x].alpha = alpha;
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

// How a wound body is lit: the top is lifted toward white and the bottom
// pressed toward black, from the filament's side colour. A matte filament
// scatters light, so its body carries a much shallower ramp than a glossy one.
namespace {
struct BodyShade {
    uint8_t lift;
    uint8_t drop;

    static BodyShade of(helix::ui::SwatchFinish finish) {
        return finish == helix::ui::SwatchFinish::Matte ? BodyShade{28, 14} : BodyShade{70, 35};
    }
    lv_color_t side(lv_color_t base) const {
        return ams_draw::darken_color(base, 30);
    }
    lv_color_t top(lv_color_t base) const {
        return ams_draw::lighten_color(side(base), lift);
    }
    lv_color_t bottom(lv_color_t base) const {
        return ams_draw::darken_color(side(base), drop);
    }
};

// A multi-colour filament wound on the spool. A dual or triple colour strand
// carries its colours side by side, so the winding shows them as thin
// alternating stripes, one strand wide, cycling through the colours along the
// axis. Each stripe is a wrap circle seen in perspective: it arcs toward the
// back cap at mid-height and returns to its axis position at the top and
// bottom of the filament. Within one row the arc is a constant leftward shift,
// so a row is the strand sequence slid left by the cap's curve there.
struct FilamentBands {
    std::vector<lv_color_t> colors;
    lv_area_t field;
    float strand; // one strand's width along the axis, in pixels
    float bulge;  // how far a wrap arcs left at mid-height: the cap's x radius
    float blend;  // share of a strand each seam blends over
    BodyShade shading;

    FilamentBands(std::vector<lv_color_t> c, const lv_area_t& axis, float strand_px, float arc,
                  bool soft, BodyShade shade)
        : colors(std::move(c)), field(axis), strand(std::max(strand_px, 1.0f)), bulge(arc),
          // A silk strand's colours shade into each other where they meet;
          // other strands meet at a hard, anti-aliased seam.
          blend(soft ? 0.8f : std::min(1.0f, 0.75f / std::max(strand_px, 1.0f))), shading(shade) {}

    // Strand k spans [k, k + 1) of @p u and is colour k mod n; seams sit at
    // the integers.
    lv_color_t color_at(float u) const {
        const size_t n = colors.size();
        auto color_of_strand = [&](float k) {
            const auto i = static_cast<long>(k) % static_cast<long>(n);
            return colors[static_cast<size_t>(i < 0 ? i + static_cast<long>(n) : i)];
        };
        const float half = blend / 2.0f;
        const float seam = std::round(u);
        if (std::fabs(u - seam) < half) {
            float t = (u - seam + half) / blend;
            t = t * t * (3.0f - 2.0f * t);
            return lv_color_mix(color_of_strand(seam), color_of_strand(seam - 1.0f),
                                static_cast<uint8_t>(255.0f * t));
        }
        return color_of_strand(std::floor(u));
    }

    // Row @p y from @p xa to @p xb at opacity @p opa, bands curved per row.
    void fill_row(SpoolLayer* layer, int32_t y, int32_t xa, int32_t xb, float g,
                  lv_opa_t opa) const {
        const float h = static_cast<float>(lv_area_get_height(&field));
        const float cy = static_cast<float>(field.y1) + h / 2.0f;
        const float ny = h > 0.0f ? (static_cast<float>(y) + 0.5f - cy) / (h / 2.0f) : 0.0f;
        // The wrap circles follow the filament ellipse's curvature.
        const float curve = std::sqrt(std::max(0.0f, 1.0f - ny * ny));
        const float shift = bulge * curve;
        auto color_of = [&](int32_t x) {
            const lv_color_t base = color_at(
                (static_cast<float>(x) + 0.5f + shift - static_cast<float>(field.x1)) / strand);
            return ams_draw::blend_color(shading.top(base), shading.bottom(base), g);
        };

        lv_draw_fill_dsc_t dsc;
        lv_draw_fill_dsc_init(&dsc);
        dsc.opa = opa;
        int32_t from = xa;
        while (from <= xb) {
            const lv_color_t c = color_of(from);
            int32_t to = from;
            while (to < xb && lv_color_eq(color_of(to + 1), c)) {
                ++to;
            }
            dsc.color = c;
            lv_area_t span = {from, y, to, y};
            spool_fill(layer, &dsc, &span);
            from = to + 1;
        }
    }
};
} // namespace

static float row_gradient(int32_t y, int32_t top, int32_t height) {
    return height > 0 ? fast_sqrt_01(static_cast<float>(y - top) / static_cast<float>(height))
                      : 0.0f;
}

// The banded twin of draw_gradient_rect().
static void draw_banded_rect(SpoolLayer* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                             const FilamentBands& bands) {
    for (int32_t y = y1; y <= y2; y++)
        bands.fill_row(layer, y, x1, x2, row_gradient(y, y1, y2 - y1), LV_OPA_COVER);
}

// The banded twin of draw_gradient_ellipse(), with the same anti-aliased rim.
static void draw_banded_ellipse(SpoolLayer* layer, int32_t cx, int32_t cy, int32_t rx, int32_t ry,
                                const FilamentBands& bands) {
    for (int32_t y = -ry; y <= ry; y++) {
        const float y_norm = static_cast<float>(y) / static_cast<float>(ry);
        const float x_extent = rx * fast_sqrt_01(1.0f - y_norm * y_norm);
        const float g = fast_sqrt_01(static_cast<float>(y + ry) / static_cast<float>(2 * ry));
        if (x_extent < 0.5f) {
            const float pole_opa = (x_extent > 0.01f) ? (x_extent * 2.0f) : 0.3f;
            bands.fill_row(layer, cy + y, cx, cx, g, static_cast<lv_opa_t>(pole_opa * 255.0f));
            continue;
        }
        const int32_t x_inner = static_cast<int32_t>(x_extent);
        const float x_frac = x_extent - static_cast<float>(x_inner);
        if (x_frac > 0.01f) {
            const auto edge_opa = static_cast<lv_opa_t>(x_frac * 255.0f);
            bands.fill_row(layer, cy + y, cx - x_inner - 1, cx - x_inner - 1, g, edge_opa);
            bands.fill_row(layer, cy + y, cx + x_inner + 1, cx + x_inner + 1, g, edge_opa);
        }
        if (x_inner > 0)
            bands.fill_row(layer, cy + y, cx - x_inner, cx + x_inner, g, LV_OPA_COVER);
    }
}

// Light the center tube's inside, seen through the hub hole at (cx, cy). Each
// pixel sees the tube's far wall; a wall point is lit when it faces the key
// light and its ray back toward the light leaves through the hole before
// meeting the wall above it. So the floor of the tube catches the light,
// brightest at the bottom, and the hole's top rim shades it at depth.
static void draw_hub_bore_light(SpoolLayer* layer, int32_t cx, int32_t cy, int32_t rx, int32_t ry,
                                lv_color_t lit) {
    if (rx <= 0 || ry <= 0)
        return;
    const lv_image_header_t& h = layer->buf->header;
    const float r = static_cast<float>(ry);
    const float skew = static_cast<float>(rx) / r;
    const int32_t y1 = LV_MAX(cy - ry, 0);
    const int32_t y2 = LV_MIN(cy + ry, static_cast<int32_t>(h.h) - 1);
    const int32_t x1 = LV_MAX(cx - rx - 1, 0);
    const int32_t x2 = LV_MIN(cx + rx + 1, static_cast<int32_t>(h.w) - 1);
    for (int32_t y = y1; y <= y2; y++) {
        auto* row = reinterpret_cast<lv_color32_t*>(layer->buf->data + y * h.stride);
        // v: down from the axis; u: across the face, unskewed.
        const float v = static_cast<float>(y - cy);
        if (v <= 0.0f)
            continue;
        const float half_chord = std::sqrt(std::max(0.0f, r * r - v * v));
        for (int32_t x = x1; x <= x2; x++) {
            const float u = static_cast<float>(x - cx) / skew;
            const float d = std::sqrt(u * u + v * v);
            const float inside = std::clamp(r - d + 0.5f, 0.0f, 1.0f);
            if (inside <= 0.0f)
                continue;
            // A sight line drifts across the face by 1 / skew for each unit of
            // depth, so it meets the far wall this deep.
            const float depth = skew * std::max(0.0f, half_chord - u);
            // Traced back toward the light, the wall point climbs kLightDrop
            // per unit of depth and meets the wall again at -v, its mirror
            // across the axis: deeper than 2v / kLightDrop it is in shadow.
            const float shadow = std::clamp(2.0f * v - depth * kLightDrop + 0.5f, 0.0f, 1.0f);
            const float lambert = v / r;
            const float a = inside * shadow * lambert;
            if (a <= 0.0f)
                continue;
            const lv_color_t seen =
                lv_color_mix(lit, lv_color_make(row[x].red, row[x].green, row[x].blue),
                             static_cast<uint8_t>(a * 255.0f + 0.5f));
            row[x] = lv_color_to_32(seen, row[x].alpha);
        }
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
// reduced effects the mask grown by kOutlinePx in solid accent.
static void paint_glow(lv_draw_buf_t* buf, const std::vector<uint8_t>& mask, SpoolGlow glow,
                       lv_color_t accent) {
    const int32_t w = buf->header.w;
    const int32_t h = buf->header.h;
    auto over = [](lv_color32_t fg, lv_color32_t bg) {
        return fg.alpha <= LV_OPA_MIN ? bg : spool_blend(fg, bg);
    };
    std::vector<uint8_t> halo, rim;
    if (glow == SpoolGlow::Halo) {
        halo = grow_and_soften(mask, w, h, kGlowHaloDilate, kGlowHaloBoxR, kGlowHaloPasses);
        rim = grow_and_soften(mask, w, h, kGlowRimDilate, kGlowRimBoxR, 1);
    } else {
        halo = grow_and_soften(mask, w, h, kOutlinePx, 0, 0);
    }
    const lv_color_t rim_color = ams_draw::lighten_color(accent, kGlowRimLighten);
    for (int32_t y = 0; y < h; y++) {
        auto* row = reinterpret_cast<lv_color32_t*>(buf->data + y * buf->header.stride);
        for (int32_t x = 0; x < w; x++) {
            const size_t i = static_cast<size_t>(y * w + x);
            lv_color32_t px = lv_color_to_32(accent, halo[i]);
            if (glow == SpoolGlow::Halo) {
                px = over(lv_color_to_32(accent, LV_OPA_MIX2(halo[i], kGlowHaloSecondOpa)), px);
                px = over(lv_color_to_32(rim_color, rim[i]), px);
            }
            row[x] = px;
        }
    }
}

// The filament's colours as drawn. Matte filament is drawn in its muted
// colours; the other finishes texture it once it is drawn.
namespace {
struct DrawnFilament {
    lv_color_t color;
    std::vector<lv_color_t> bands; ///< two or more for a multi-colour filament
};
} // namespace

static DrawnFilament drawn_filament(const SpoolCanvasData& d) {
    const bool matte = d.finish == helix::ui::SwatchFinish::Matte;
    DrawnFilament out{matte ? helix::ui::swatch_matte_color(d.color) : d.color, d.colors};
    if (matte) {
        for (auto& c : out.bands)
            c = helix::ui::swatch_matte_color(c);
    }
    return out;
}

// The spool face-on: the wound filament, the vented front flange over it
// smoked so the filament reads through, and the hub. Proportions are the side
// view's, scaled so the flange fills the canvas.
static void render_front_pixels(SpoolCanvasData* data) {
    const int32_t size = data->size;
    const float c = static_cast<float>(size) / 2.0f;
    const float flange_r = c - 1.0f;
    const float face_size = flange_r / helix::ui::tray::kSpoolFlangeRadius;
    const float hub_r = face_size * kHubRadius;
    const float fill = LV_CLAMP(0.0f, data->fill_level, 1.0f);
    const float max_filament_r = flange_r * kFrontMaxFilament;
    const float filament_r = fill > kEmptyFill ? hub_r + (max_filament_r - hub_r) * fill : 0.0f;

    const lv_color_t front_color = theme_manager_get_color("spool_body");
    const lv_color_t back_color = theme_manager_get_color("spool_body_shade");
    const lv_color_t flange_top = ams_draw::lighten_color(front_color, kFlangeLift);
    const lv_color_t flange_bottom = ams_draw::darken_color(front_color, kFlangeDrop);
    const lv_color_t hollow_top = ams_draw::darken_color(back_color, kVentInsideShadeTop);
    const lv_color_t hollow_bottom = ams_draw::darken_color(back_color, kVentInsideShadeBottom);
    const lv_color_t hub_top = theme_manager_get_color("spool_hub_top");
    const lv_color_t hub_bottom = theme_manager_get_color("spool_hub_bottom");
    const BodyShade shading = BodyShade::of(data->finish);
    const DrawnFilament drawn = drawn_filament(*data);

    lv_canvas_fill_bg(data->canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_draw_buf_t* buf = data->draw_buf;
    const lv_image_header_t& h = buf->header;
    auto row_of = [&](int32_t y) {
        return reinterpret_cast<lv_color32_t*>(buf->data + y * h.stride);
    };
    auto dist = [c](int32_t x, int32_t y) {
        const float dx = static_cast<float>(x) + 0.5f - c;
        const float dy = static_cast<float>(y) + 0.5f - c;
        return std::sqrt(dx * dx + dy * dy);
    };
    const int32_t lim = LV_MIN(size, static_cast<int32_t>(LV_MIN(h.w, h.h)));

    // Pass 1: the wound filament disc, split and shaded the way the side
    // view's body is, then textured by its finish.
    const lv_area_t disc = {
        static_cast<int32_t>(c - filament_r), static_cast<int32_t>(c - filament_r),
        static_cast<int32_t>(c + filament_r), static_cast<int32_t>(c + filament_r)};
    if (filament_r > 0.0f) {
        const std::vector<float> stops =
            drawn.bands.size() >= 2
                ? helix::ui::swatch_split_thresholds(disc, LV_RADIUS_CIRCLE, drawn.bands.size())
                : std::vector<float>{};
        const float w = static_cast<float>(disc.x2 - disc.x1 + 1);
        for (int32_t y = 0; y < lim; y++) {
            auto* row = row_of(y);
            const float t = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            for (int32_t x = 0; x < lim; x++) {
                const float a = std::clamp(filament_r - dist(x, y) + 0.5f, 0.0f, 1.0f);
                if (a <= 0.0f)
                    continue;
                lv_color_t base = drawn.color;
                if (!stops.empty()) {
                    const float f = ((static_cast<float>(x - disc.x1) + 0.5f) / w +
                                     (static_cast<float>(y - disc.y1) + 0.5f) / w) /
                                    2.0f;
                    size_t band = 0;
                    while (band < stops.size() && f > stops[band])
                        ++band;
                    base = drawn.bands[band];
                }
                row[x] = lv_color_to_32(
                    ams_draw::blend_color(shading.top(base), shading.bottom(base), t),
                    static_cast<lv_opa_t>(a * 255.0f + 0.5f));
            }
        }
        helix::ui::swatch_paint_finish_pixels(buf, disc, LV_RADIUS_CIRCLE, data->finish);
    }

    // Pass 2: the flange over it. Solid flange is smoked where filament lies
    // behind it; a vent shows what is behind at full strength.
    const helix::ui::SpoolVentFace face{c, c, face_size, 1.0f, helix::ui::spool_vent_detail(size)};
    const float lip = std::max(1.0f, face_size * kVentLipFraction);
    for (int32_t y = 0; y < lim; y++) {
        auto* row = row_of(y);
        const float py = static_cast<float>(y) + 0.5f;
        const float t = py / static_cast<float>(size);
        const lv_color_t flange = ams_draw::blend_color(flange_top, flange_bottom, t);
        const lv_color_t hollow = ams_draw::blend_color(hollow_top, hollow_bottom, t);
        for (int32_t x = 0; x < lim; x++) {
            const float d = dist(x, y);
            const float a = std::clamp(flange_r - d + 0.5f, 0.0f, 1.0f);
            if (a <= 0.0f)
                continue;
            const float pxc = static_cast<float>(x) + 0.5f;
            const bool wound = d <= filament_r;
            const lv_color_t behind =
                wound ? lv_color_make(row[x].red, row[x].green, row[x].blue) : hollow;
            const lv_color_t solid = wound ? lv_color_mix(flange, behind, kFrontSmokeOpa) : flange;
            lv_color_t seen = solid;
            const float vent = helix::ui::spool_vent_coverage(face, pxc, py);
            if (vent > 0.0f) {
                lv_color_t open = behind;
                if (helix::ui::spool_vent_coverage(face, pxc, py - lip) < 1.0f)
                    open = lv_color_mix(lv_color_black(), open, kVentLipShade);
                seen = lv_color_mix(open, solid, static_cast<uint8_t>(vent * 255.0f + 0.5f));
            }
            const float hub = std::clamp(hub_r - d + 0.5f, 0.0f, 1.0f);
            if (hub > 0.0f)
                seen = lv_color_mix(ams_draw::blend_color(hub_top, hub_bottom, t), seen,
                                    static_cast<uint8_t>(hub * 255.0f + 0.5f));
            row[x] = lv_color_to_32(seen, static_cast<lv_opa_t>(a * 255.0f + 0.5f));
        }
    }
    lv_obj_invalidate(data->canvas);
}

// Actual pixel drawing. Callers should prefer redraw_spool(), which handles
// dedup and cache. This function unconditionally draws into data->draw_buf.
static void render_spool_pixels(SpoolCanvasData* data) {
    if (data->view == helix::ui::SpoolView::Front) {
        render_front_pixels(data);
        return;
    }
    int32_t size = data->size;
    int32_t cy = size / 2; // Vertical center

    // Calculate dimensions - vertical radius and horizontal (compressed) radius
    int32_t flange_ry = (int32_t)(size * helix::ui::tray::kSpoolFlangeRadius); // Vertical radius
    int32_t flange_rx = (int32_t)(flange_ry * helix::ui::tray::kDepthSkew); // Horizontal (narrower)
    int32_t hub_ry = (int32_t)(size * kHubRadius);
    int32_t hub_rx = (int32_t)(hub_ry * helix::ui::tray::kDepthSkew);
    int32_t spool_width = (int32_t)(size * helix::ui::tray::kSpoolWidth);

    // X positions for left (back) and right (front) flanges
    int32_t center_x = size / 2;
    int32_t left_x = center_x - spool_width / 2;  // Left side (back flange)
    int32_t right_x = center_x + spool_width / 2; // Right side (front flange)

    // Fill level determines wound filament radius
    // Max filament is smaller than flange so flanges always show as "taller"
    float fill = LV_CLAMP(0.0f, data->fill_level, 1.0f);
    int32_t max_filament_ry = (int32_t)(flange_ry * 0.85f); // Flanges 15% taller than full filament
    int32_t filament_ry = hub_ry + (int32_t)((max_filament_ry - hub_ry) * fill);
    int32_t filament_rx = (int32_t)(filament_ry * helix::ui::tray::kDepthSkew);

    // Colors (from theme tokens)
    lv_color_t back_color = theme_manager_get_color("spool_body_shade");
    lv_color_t front_color = theme_manager_get_color("spool_body");
    const BodyShade shading = BodyShade::of(data->finish);
    const DrawnFilament drawn = drawn_filament(*data);
    const lv_color_t filament_color = drawn.color;
    const std::vector<lv_color_t>& band_colors = drawn.bands;

    // Clear canvas
    lv_canvas_fill_bg(data->canvas, lv_color_black(), LV_OPA_TRANSP);

    SpoolLayer layer{data->draw_buf};

    // ========================================
    // STEP 1: Draw BACK FLANGE (left side) with gradient + edge highlight
    // ========================================
    {
        lv_color_t bf_light = ams_draw::lighten_color(back_color, kFlangeLift);
        lv_color_t bf_dark = ams_draw::darken_color(back_color, kFlangeDrop);
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
    if (fill > kEmptyFill) {
        lv_color_t fil_light = shading.top(filament_color);
        lv_color_t fil_dark = shading.bottom(filament_color);

        int32_t fil_top = cy - filament_ry;
        int32_t fil_bottom = cy + filament_ry;
        // What the viewer sees of the filament: the back cap's left edge to
        // where the front flange covers the body.
        const lv_area_t visible = {left_x - filament_rx, fil_top, right_x, fil_bottom};
        if (band_colors.size() >= 2) {
            // Every wrap is the back cap's circle slid along the axis, so each
            // stripe bulges exactly as the cap does. A 1.75 mm strand on a
            // spool about 200 mm across is an eightieth of its diameter.
            const bool soft = data->finish == helix::ui::SwatchFinish::Silk;
            const lv_area_t axis = {left_x, fil_top, right_x, fil_bottom};
            const float strand_px = std::max(2.0f, 2.0f * static_cast<float>(flange_ry) / 80.0f);
            const FilamentBands bands(band_colors, axis, strand_px, static_cast<float>(filament_rx),
                                      soft, shading);
            draw_banded_ellipse(&layer, left_x, cy, filament_rx, filament_ry, bands);
            draw_banded_rect(&layer, left_x, fil_top, right_x, fil_bottom, bands);
        } else {
            // 2a: Back face ellipse (with gradient)
            draw_gradient_ellipse(&layer, left_x, cy, filament_rx, filament_ry, fil_light,
                                  fil_dark);
            // 2b: Rectangle body connecting the two faces (with gradient)
            draw_gradient_rect(&layer, left_x, fil_top, right_x, fil_bottom, fil_light, fil_dark);
        }

        // 2b': the finish's texture over everything drawn of the filament,
        // back cap included, written into the buffer.
        const float cap_rx = static_cast<float>(std::max<int32_t>(filament_rx, 1));
        const float cap_ry = static_cast<float>(std::max<int32_t>(filament_ry, 1));
        const helix::ui::SwatchCoverage wound = [=](float x, float y) {
            if (y < static_cast<float>(fil_top) || y > static_cast<float>(fil_bottom + 1))
                return 0.0f;
            if (x >= static_cast<float>(left_x))
                return x <= static_cast<float>(right_x + 1) ? 1.0f : 0.0f;
            const float dx = (x - static_cast<float>(left_x)) / cap_rx;
            const float dy = (y - static_cast<float>(cy)) / cap_ry;
            const float d = std::sqrt(dx * dx + dy * dy);
            return std::clamp((1.0f - d) * cap_rx + 0.5f, 0.0f, 1.0f);
        };
        helix::ui::swatch_paint_finish_pixels(data->draw_buf, visible, wound, data->finish);

        // 2c: Front face ellipse (will be covered by front flange anyway)
        lv_color_t front_light = ams_draw::lighten_color(filament_color, 70);
        lv_color_t front_dark = ams_draw::darken_color(filament_color, 35);
        draw_gradient_ellipse(&layer, right_x, cy, filament_rx, filament_ry, front_light,
                              front_dark);
    } else {
        // Empty: the bare core tube joins the flanges. It is the flange's
        // plastic, and the filament would wind from its surface outward.
        const lv_color_t core_light = ams_draw::lighten_color(front_color, kFlangeLift);
        const lv_color_t core_dark = ams_draw::darken_color(front_color, kFlangeDrop);
        draw_gradient_ellipse(&layer, left_x, cy, hub_rx, hub_ry, core_light, core_dark);
        draw_gradient_rect(&layer, left_x, cy - hub_ry, right_x, cy + hub_ry, core_light,
                           core_dark);
    }

    // ========================================
    // STEP 3: Draw FRONT FLANGE (right side) with gradient + edge highlight
    // ========================================
    {
        lv_color_t ff_light = ams_draw::lighten_color(front_color, kFlangeLift);
        lv_color_t ff_dark = ams_draw::darken_color(front_color, kFlangeDrop);
        // Main flange ellipse with gradient
        draw_gradient_ellipse(&layer, right_x, cy, flange_rx, flange_ry, ff_light, ff_dark);
        // Edge highlight on left side (gives 3D thickness illusion)
        // Dramatic gradient: very bright at top, dark at bottom
        lv_color_t edge_light = ams_draw::lighten_color(front_color, 100);
        lv_color_t edge_dark = ams_draw::darken_color(front_color, 40);
        draw_ellipse_left_edge(&layer, right_x, cy, flange_rx, flange_ry, edge_light, edge_dark, 2);

        // Vents: the filament shows through where it is wound that far out,
        // the dark far flange where it is not.
        const helix::ui::SpoolVentFace face{static_cast<float>(right_x) + 0.5f,
                                            static_cast<float>(cy) + 0.5f, static_cast<float>(size),
                                            helix::ui::tray::kDepthSkew,
                                            helix::ui::spool_vent_detail(size)};
        const VentBacking wound{ams_draw::darken_color(filament_color, kVentFilamentShadeTop),
                                ams_draw::darken_color(filament_color, kVentFilamentShadeBottom)};
        const VentBacking hollow{ams_draw::darken_color(back_color, kVentInsideShadeTop),
                                 ams_draw::darken_color(back_color, kVentInsideShadeBottom)};
        const float wound_r = static_cast<float>(filament_ry);
        paint_vents(&layer, face,
                    {right_x - flange_rx, cy - flange_ry, right_x + flange_rx, cy + flange_ry},
                    [&](float face_radius) { return face_radius <= wound_r ? wound : hollow; });
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
    // The tube is the flange's plastic, lit as the flange's top is.
    draw_hub_bore_light(&layer, right_x, cy, hub_rx, hub_ry,
                        ams_draw::lighten_color(front_color, kFlangeLift));

    lv_obj_invalidate(data->canvas);

    spdlog::trace("[SpoolCanvas] Redrawn: size={}, fill={:.0f}%", size, fill * 100.0f);
}

// Folds the multi-colour list and finish into the render cache key.
static uint32_t filament_hash(const SpoolCanvasData& d) {
    uint32_t h = static_cast<uint32_t>(d.finish) * 0x01000193u;
    for (lv_color_t c : d.colors)
        h = (h ^ color_to_rgb24(c)) * 0x01000193u;
    return h;
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

    SpoolCacheKey key{color_to_rgb24(data->color),
                      static_cast<uint16_t>(data->size),
                      compute_fill_bucket(data->fill_level),
                      SpoolGlow::None,
                      filament_hash(*data),
                      data->view};

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
        if (SpoolCanvasData* registered = s_registry.get(obj)) {
            std::unique_ptr<SpoolCanvasData> data(registered);
            // The canvas was a composite source until this delete; a blend of
            // it may still be in flight.
            if (data) {
                helix::safe_draw_buf_destroy(data->draw_buf, "spool");
            }
            unbind_glows(obj);
            lv_obj_set_user_data(obj, nullptr);
            s_registry.take(obj);
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
    data_ptr->size = kDefaultSize;
    data_ptr->color = lv_color_hex(kDefaultColor);
    data_ptr->fill_level = 1.0f;

    sync_draw_buf(data_ptr.get());
    s_registry.set(canvas, data_ptr.get());
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
        size = kDefaultSize;
    }

    lv_obj_t* canvas = lv_canvas_create(parent);
    if (!canvas) {
        spdlog::error("[SpoolCanvas] Failed to create canvas");
        return nullptr;
    }

    auto data_ptr = std::make_unique<SpoolCanvasData>();
    data_ptr->canvas = canvas;
    data_ptr->size = size;
    data_ptr->color = lv_color_hex(kDefaultColor);
    data_ptr->fill_level = 1.0f;

    if (!sync_draw_buf(data_ptr.get())) {
        helix::ui::safe_delete(canvas);
        return nullptr;
    }
    s_registry.set(canvas, data_ptr.get());
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
        data->color.blue == color.blue && data->colors.empty() &&
        data->finish == helix::ui::SwatchFinish::Plain) {
        return;
    }
    data->color = color;
    data->colors.clear();
    data->finish = helix::ui::SwatchFinish::Plain;
    redraw_spool(data);
}

void ui_spool_canvas_set_fill_level(lv_obj_t* canvas, float fill_level) {
    auto* data = get_data(canvas);
    if (!data)
        return;
    float clamped = LV_CLAMP(0.0f, fill_level, 1.0f);
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

namespace helix::ui {

void spool_canvas_set_filament(lv_obj_t* canvas, uint32_t rgb, const std::string& multi_color_hexes,
                               SwatchFinish finish) {
    auto* data = get_data(canvas);
    if (!data)
        return;
    std::vector<lv_color_t> colors = parse_multi_colors(multi_color_hexes);
    if (colors.size() < 2)
        colors.clear();
    data->color = lv_color_hex(rgb);
    data->colors = std::move(colors);
    data->finish = finish;
    redraw_spool(data);
}

void spool_canvas_set_view(lv_obj_t* canvas, SpoolView view) {
    auto* data = get_data(canvas);
    if (!data || data->view == view)
        return;
    data->view = view;
    redraw_spool(data);
}

SpoolView spool_canvas_get_view(lv_obj_t* canvas) {
    auto* data = get_data(canvas);
    return data ? data->view : SpoolView::Side;
}

void spool_canvas_set_filament(lv_obj_t* canvas, const SlotInfo& slot) {
    spool_canvas_set_filament(canvas, slot.color_rgb, slot.multi_color_hexes,
                              swatch_finish_of(slot));
}

} // namespace helix::ui

float ui_spool_canvas_get_fill_level(lv_obj_t* canvas) {
    auto* data = get_data(canvas);
    return data ? data->fill_level : -1.0f;
}

lv_color_t ui_spool_canvas_get_color(lv_obj_t* canvas) {
    auto* data = get_data(canvas);
    return data ? data->color : lv_color_hex(kDefaultColor);
}

void ui_spool_canvas_invalidate_cache(void) {
    s_cache_list.clear();
    s_cache_index.clear();
    clear_glow_cache();
    s_registry.for_each([](lv_obj_t* /*canvas*/, SpoolCanvasData* data) {
        if (data) {
            data->has_rendered = false;
        }
    });
    spdlog::debug("[SpoolCanvas] Render cache invalidated");
}

// ----------------------------------------------------------------------------
// Glow layer
// ----------------------------------------------------------------------------
// A glow is one image per (shape, accent, simple), shared by every layer that
// shows it: the cache and the layers hold references, so a buffer lives
// exactly as long as something needs it. Glows keep their own small LRU so
// they never evict spool renders.
static constexpr size_t kGlowCacheMaxEntries = 4;

namespace {
using GlowImage = std::shared_ptr<lv_draw_buf_t>;

struct GlowCacheEntry {
    SpoolCacheKey key;
    GlowImage image;
};

struct SpoolGlowData {
    lv_obj_t* spool = nullptr; ///< bound spool canvas
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
    if (s_glow_cache.size() > kGlowCacheMaxEntries)
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

// The silhouette, padded by @p m: the spool canvas's own alpha, in whichever
// view it draws.
static std::vector<uint8_t> silhouette_mask(SpoolCanvasData* spool, int32_t size, int32_t m) {
    const int32_t dim = size + 2 * m;
    std::vector<uint8_t> mask(static_cast<size_t>(dim * dim), 0);
    if (!spool || !spool->draw_buf)
        return mask;
    const lv_image_header_t& h = spool->draw_buf->header;
    for (int32_t y = 0; y < std::min<int32_t>(h.h, size); y++) {
        const auto* row =
            reinterpret_cast<const lv_color32_t*>(spool->draw_buf->data + y * h.stride);
        for (int32_t x = 0; x < std::min<int32_t>(h.w, size); x++)
            mask[(y + m) * dim + x + m] = row[x].alpha;
    }
    return mask;
}

// Show the glow for the layer's bound shape. A 3D spool that has not rendered
// yet has no silhouette to read: the layer stays empty until its first render.
static void paint_glow_layer(lv_obj_t* glow, SpoolGlowData& g) {
    SpoolCanvasData* spool = get_data(g.spool);
    if (!spool || !spool->has_rendered) {
        set_glow_image(glow, g, nullptr, 0);
        return;
    }
    const SpoolGlow mode = g.simple ? SpoolGlow::Outline : SpoolGlow::Halo;
    const int32_t margin = g.simple ? kOutlinePx : kGlowMargin;
    const lv_color_t accent = helix::ui::tube_accent();
    // A glow depends only on the silhouette, never the filament color, so the
    // key carries the accent where a spool render carries its color.
    // Face-on, the flange covers the filament at every fill, so one glow
    // serves them all.
    const helix::ui::SpoolView view = spool->last_key.view;
    const uint8_t shape = view == helix::ui::SpoolView::Front ? 0 : spool->last_key.fill_bucket;
    const SpoolCacheKey key{
        color_to_rgb24(accent), static_cast<uint16_t>(g.size), shape, mode, 0, view};
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
