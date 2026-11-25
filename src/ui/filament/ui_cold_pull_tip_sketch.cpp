// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_cold_pull_tip_sketch.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace helix::ui {

namespace {

/// Along the tip as a fraction of its length, half thickness as a fraction of its height.
struct ProfilePoint {
    float along;
    float half;
};

constexpr std::array<ProfilePoint, 5> kProfile = {{
    {0.00f, 0.17f}, // the strand above the hotend
    {0.45f, 0.17f},
    {0.49f, 0.25f}, // the step into the melt zone
    {0.80f, 0.25f},
    {0.97f, 0.00f}, // the nozzle's taper, down to the nib
}};
constexpr float kMeltStep = 0.49f;
constexpr float kLengthPerHeight = 4.2f;
constexpr float kInnerBand = 0.68f; ///< Share of the half thickness in the body colour

struct Frame {
    float left;
    float length;
    float mid;
    float height;

    [[nodiscard]] float x(float along) const {
        return left + along * length;
    }
    [[nodiscard]] float y(float offset) const {
        return mid + offset * height;
    }
};

lv_point_precise_t pt(float x, float y) {
    return {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(y)};
}

void fill_tri(lv_layer_t* layer, lv_point_precise_t a, lv_point_precise_t b, lv_point_precise_t c,
              lv_color_t color, lv_opa_t opa) {
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = opa;
    dsc.p[0] = a;
    dsc.p[1] = b;
    dsc.p[2] = c;
    lv_draw_triangle(layer, &dsc);
}

void stroke(lv_layer_t* layer, lv_point_precise_t a, lv_point_precise_t b, lv_color_t color,
            int32_t width, lv_opa_t opa) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.opa = opa;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1 = a;
    dsc.p2 = b;
    lv_draw_line(layer, &dsc);
}

/// Fills the profile from @p from_along on, scaled across its thickness by @p scale.
void fill_band(lv_layer_t* layer, const Frame& f, float scale, float from_along, lv_color_t color,
               lv_opa_t opa, const ProfilePoint* prof = kProfile.data(),
               size_t n = kProfile.size()) {
    for (size_t i = 1; i < n; ++i) {
        const ProfilePoint& a = prof[i - 1];
        const ProfilePoint& b = prof[i];
        if (b.along <= from_along) {
            continue;
        }
        const float t = a.along < from_along ? (from_along - a.along) / (b.along - a.along) : 0.0f;
        const float along0 = a.along + (b.along - a.along) * t;
        const float half0 = (a.half + (b.half - a.half) * t) * scale;
        const float half1 = b.half * scale;
        const auto tl = pt(f.x(along0), f.y(-half0));
        const auto bl = pt(f.x(along0), f.y(half0));
        const auto tr = pt(f.x(b.along), f.y(-half1));
        const auto br = pt(f.x(b.along), f.y(half1));
        // Two antialiased triangles leave a visible seam down a long diagonal, so
        // the long runs are one rectangle or one triangle.
        if (half0 == half1) {
            lv_draw_rect_dsc_t rect;
            lv_draw_rect_dsc_init(&rect);
            rect.bg_color = color;
            rect.bg_opa = opa;
            const lv_area_t area{static_cast<int32_t>(tl.x), static_cast<int32_t>(tl.y),
                                 static_cast<int32_t>(br.x), static_cast<int32_t>(br.y)};
            lv_draw_rect(layer, &rect, &area);
        } else if (half1 == 0.0f) {
            fill_tri(layer, tl, br, bl, color, opa);
        } else {
            fill_tri(layer, tl, tr, br, color, opa);
            fill_tri(layer, tl, br, bl, color, opa);
        }
    }
}

/// Strokes the profile's edge on one side, offset across the thickness by @p scale.
void stroke_edge(lv_layer_t* layer, const Frame& f, float scale, lv_color_t color, int32_t width,
                 lv_opa_t opa, const ProfilePoint* prof = kProfile.data(),
                 size_t n = kProfile.size()) {
    for (size_t i = 1; i < n; ++i) {
        const ProfilePoint& a = prof[i - 1];
        const ProfilePoint& b = prof[i];
        stroke(layer, pt(f.x(a.along), f.y(a.half * scale)), pt(f.x(b.along), f.y(b.half * scale)),
               color, width, opa);
    }
}

void draw_wisp(lv_layer_t* layer, const Frame& f, lv_color_t color) {
    constexpr std::array<ProfilePoint, 4> kWisp = {
        {{0.97f, 0.00f}, {0.982f, -0.015f}, {0.992f, -0.05f}, {1.00f, -0.10f}}};
    for (size_t i = 1; i < kWisp.size(); ++i) {
        stroke(layer, pt(f.x(kWisp[i - 1].along), f.y(kWisp[i - 1].half)),
               pt(f.x(kWisp[i].along), f.y(kWisp[i].half)), color, 1, LV_OPA_COVER);
    }
}

void draw_dirt(lv_layer_t* layer, const Frame& f, lv_color_t carbon, lv_color_t old_colour) {
    // The old colour stays where the melt was: the taper, streaking back into the body.
    fill_band(layer, f, 0.8f, 0.80f, old_colour, LV_OPA_80);
    stroke(layer, pt(f.x(0.58f), f.y(0.06f)), pt(f.x(0.82f), f.y(0.05f)), old_colour, 2, LV_OPA_60);
    stroke(layer, pt(f.x(0.66f), f.y(-0.08f)), pt(f.x(0.82f), f.y(-0.07f)), old_colour, 2,
           LV_OPA_50);

    struct Speck {
        float along;
        float across;
        int32_t radius;
    };
    constexpr std::array<Speck, 12> kSpecks = {{{0.53f, -0.12f, 2},
                                                {0.57f, 0.09f, 1},
                                                {0.62f, -0.04f, 2},
                                                {0.66f, 0.15f, 2},
                                                {0.70f, -0.14f, 1},
                                                {0.74f, 0.06f, 2},
                                                {0.78f, -0.09f, 2},
                                                {0.82f, 0.13f, 1},
                                                {0.85f, -0.05f, 2},
                                                {0.88f, 0.05f, 1},
                                                {0.91f, -0.02f, 1},
                                                {0.40f, 0.06f, 1}}};
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = carbon;
    dot.bg_opa = LV_OPA_COVER;
    dot.radius = LV_RADIUS_CIRCLE;
    for (const Speck& s : kSpecks) {
        const auto cx = static_cast<int32_t>(f.x(s.along));
        const auto cy = static_cast<int32_t>(f.y(s.across));
        lv_area_t area{cx - s.radius, cy - s.radius, cx + s.radius, cy + s.radius};
        lv_draw_rect(layer, &dot, &area);
    }
}

/// PROTOTYPE: tip-forming reference shapes (shape="clean|hair|ribbed|blob").
enum class TipShape : uintptr_t { Cold = 0, Clean, Hair, Ribbed, Blob };

constexpr std::array<ProfilePoint, 3> kCleanTip = {
    {{0.00f, 0.17f}, {0.66f, 0.17f}, {0.92f, 0.00f}}};
constexpr std::array<ProfilePoint, 3> kHairTip = {{{0.00f, 0.17f}, {0.56f, 0.17f}, {0.74f, 0.00f}}};
constexpr std::array<ProfilePoint, 11> kRibbedTip = {{{0.00f, 0.17f},
                                                      {0.48f, 0.17f},
                                                      {0.52f, 0.27f},
                                                      {0.58f, 0.23f},
                                                      {0.64f, 0.28f},
                                                      {0.70f, 0.23f},
                                                      {0.76f, 0.28f},
                                                      {0.82f, 0.23f},
                                                      {0.86f, 0.26f},
                                                      {0.92f, 0.12f},
                                                      {0.93f, 0.00f}}};
constexpr std::array<ProfilePoint, 2> kBlobStrand = {{{0.00f, 0.17f}, {0.70f, 0.17f}}};

void draw_profile(lv_layer_t* layer, const Frame& f, const ProfilePoint* prof, size_t n,
                  lv_color_t body, lv_color_t shade, lv_color_t light) {
    fill_band(layer, f, 1.0f, 0.0f, shade, LV_OPA_COVER, prof, n);
    fill_band(layer, f, kInnerBand, 0.0f, body, LV_OPA_COVER, prof, n);
    stroke_edge(layer, f, -0.42f, light, 2, LV_OPA_80, prof, n);
}

void draw_shape(lv_layer_t* layer, const Frame& f, TipShape shape, lv_color_t body,
                lv_color_t shade, lv_color_t light) {
    switch (shape) {
    case TipShape::Clean:
        draw_profile(layer, f, kCleanTip.data(), kCleanTip.size(), body, shade, light);
        break;
    case TipShape::Hair: {
        draw_profile(layer, f, kHairTip.data(), kHairTip.size(), body, shade, light);
        constexpr std::array<ProfilePoint, 6> kHair = {{{0.74f, 0.00f},
                                                        {0.80f, 0.03f},
                                                        {0.86f, -0.02f},
                                                        {0.92f, 0.04f},
                                                        {0.97f, 0.00f},
                                                        {1.00f, 0.05f}}};
        for (size_t i = 1; i < kHair.size(); ++i) {
            stroke(layer, pt(f.x(kHair[i - 1].along), f.y(kHair[i - 1].half)),
                   pt(f.x(kHair[i].along), f.y(kHair[i].half)), body, 1, LV_OPA_COVER);
        }
        break;
    }
    case TipShape::Ribbed:
        draw_profile(layer, f, kRibbedTip.data(), kRibbedTip.size(), body, shade, light);
        break;
    case TipShape::Blob: {
        draw_profile(layer, f, kBlobStrand.data(), kBlobStrand.size(), body, shade, light);
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color = shade;
        dot.bg_opa = LV_OPA_COVER;
        dot.radius = LV_RADIUS_CIRCLE;
        const auto cx = static_cast<int32_t>(f.x(0.78f));
        const auto cy = static_cast<int32_t>(f.y(0.0f));
        const auto r = static_cast<int32_t>(f.height * 0.36f);
        lv_area_t area{cx - r, cy - r, cx + r, cy + r};
        lv_draw_rect(layer, &dot, &area);
        dot.bg_color = body;
        const auto ri = static_cast<int32_t>(r * kInnerBand);
        lv_area_t inner{cx - ri, cy - ri, cx + ri, cy + ri};
        lv_draw_rect(layer, &dot, &inner);
        break;
    }
    case TipShape::Cold:
        break;
    }
}

void on_draw(lv_event_t* e) {
    auto* obj = lv_event_get_current_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_obj_get_content_coords(obj, &coords);
    const float w = static_cast<float>(lv_area_get_width(&coords));
    const float h = static_cast<float>(lv_area_get_height(&coords));
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    const float length = std::min(w, h * kLengthPerHeight);
    const Frame f{static_cast<float>(coords.x1) + (w - length) / 2.0f, length,
                  static_cast<float>(coords.y1) + h / 2.0f, h};

    const lv_color_t body = theme_manager_get_color("text_muted");
    const lv_color_t shade = lv_color_darken(body, LV_OPA_40);
    const lv_color_t light = theme_manager_get_color("text");

    const auto flags = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(obj));
    const auto shape = static_cast<TipShape>(flags >> 1);
    if (shape != TipShape::Cold) {
        draw_shape(layer, f, shape, body, shade, light);
        return;
    }
    fill_band(layer, f, 1.0f, 0.0f, shade, LV_OPA_COVER);
    fill_band(layer, f, kInnerBand, 0.0f, body, LV_OPA_COVER);
    stroke_edge(layer, f, -0.42f, light, 2, LV_OPA_80);
    stroke(layer, pt(f.x(kMeltStep), f.y(-0.25f)), pt(f.x(kMeltStep), f.y(0.25f)), shade, 2,
           LV_OPA_70);
    if (flags & 1u) {
        draw_dirt(layer, f, lv_color_darken(body, LV_OPA_90), theme_manager_get_color("primary"));
    }
    draw_wisp(layer, f, body);
}

void* tip_xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    auto* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_add_event_cb(obj, on_draw, LV_EVENT_DRAW_MAIN_END, nullptr);
    return obj;
}

void tip_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    auto* obj = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));
    lv_xml_obj_apply(state, attrs);
    // The user pointer packs the flags: bit 0 dirty, the rest the shape. No storage to free.
    auto flags = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(obj));
    for (int i = 0; attrs[i] != nullptr; i += 2) {
        if (std::strcmp(attrs[i], "dirty") == 0) {
            flags = (flags & ~uintptr_t{1}) | (std::strcmp(attrs[i + 1], "true") == 0 ? 1u : 0u);
        } else if (std::strcmp(attrs[i], "shape") == 0) {
            const char* v = attrs[i + 1];
            TipShape shape = TipShape::Cold;
            if (std::strcmp(v, "clean") == 0)
                shape = TipShape::Clean;
            else if (std::strcmp(v, "hair") == 0)
                shape = TipShape::Hair;
            else if (std::strcmp(v, "ribbed") == 0)
                shape = TipShape::Ribbed;
            else if (std::strcmp(v, "blob") == 0)
                shape = TipShape::Blob;
            flags = (flags & 1u) | (static_cast<uintptr_t>(shape) << 1);
        }
    }
    lv_obj_set_user_data(obj, reinterpret_cast<void*>(flags));
}

} // namespace

void register_cold_pull_tip_sketch_widget() {
    lv_xml_register_widget("cold_pull_tip_sketch", tip_xml_create, tip_xml_apply);
    spdlog::trace("[ColdPullTipSketch] Widget registered with XML system");
}

} // namespace helix::ui
