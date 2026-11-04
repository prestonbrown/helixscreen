// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "bed_plate_draw.h"

#include "bed_grid.h"

#include <algorithm>

namespace helix {

namespace {
/// Dash and gap of an outline stroke, as multiples of its width.
constexpr int kOutlineDashPerWidth = 3;
} // namespace

BedPlateLayers plan_bed_plate_layers(const BedPlateLayersInput& in, const BedCoordMapper& mapper) {
    BedPlateLayers out;
    if (in.x_max <= in.x_min || in.y_max <= in.y_min) {
        return out;
    }

    if (static_cast<float>(kBedGridSpacingMm) * mapper.scale() >= kMinGridSpacingPx) {
        for (const auto& seg : bed_grid_lines(in.x_min, in.x_max, in.y_min, in.y_max,
                                              kBedGridSpacingMm, in.circular)) {
            const auto [x1, y1] =
                mapper.mm_to_px(static_cast<float>(seg.x1), static_cast<float>(seg.y1));
            const auto [x2, y2] =
                mapper.mm_to_px(static_cast<float>(seg.x2), static_cast<float>(seg.y2));
            out.grid.push_back({x1, y1, x2, y2});
        }
    }

    for (const auto& outline : in.outlines) {
        if (outline.points.size() < 3) {
            continue;
        }
        std::vector<PxPoint> poly;
        poly.reserve(outline.points.size());
        for (const auto& p : outline.points) {
            const auto [x, y] = mapper.mm_to_px(std::clamp(p.x, in.x_min, in.x_max),
                                                std::clamp(p.y, in.y_min, in.y_max));
            poly.push_back({x, y});
        }
        out.outlines.push_back(std::move(poly));
    }
    return out;
}

void draw_bed_plate_layers(lv_layer_t* layer, const lv_point_t& origin,
                           const BedPlateLayers& layers, const BedPlateStyle& style) {
    if (!layer) {
        return;
    }
    const auto ox = static_cast<float>(origin.x);
    const auto oy = static_cast<float>(origin.y);
    auto at = [ox, oy](float x, float y) {
        return lv_point_precise_t{static_cast<lv_value_precise_t>(ox + x),
                                  static_cast<lv_value_precise_t>(oy + y)};
    };

    lv_draw_line_dsc_t grid;
    lv_draw_line_dsc_init(&grid);
    grid.color = style.grid_color;
    grid.opa = style.grid_opa;
    grid.width = 1;
    for (const auto& seg : layers.grid) {
        grid.p1 = at(seg.x1, seg.y1);
        grid.p2 = at(seg.x2, seg.y2);
        lv_draw_line(layer, &grid);
    }

    lv_draw_line_dsc_t edge;
    lv_draw_line_dsc_init(&edge);
    edge.color = style.outline_color;
    edge.width = style.outline_width_px;
    edge.dash_width = style.outline_width_px * kOutlineDashPerWidth;
    edge.dash_gap = style.outline_width_px * kOutlineDashPerWidth;

    lv_draw_triangle_dsc_t fill;
    lv_draw_triangle_dsc_init(&fill);
    fill.color = style.outline_color;
    fill.opa = style.outline_fill_opa;

    for (const auto& poly : layers.outlines) {
        // A fan from the first vertex fills a convex outline exactly; a concave one gets
        // extra fill inside its hull, which a translucent fill tolerates.
        if (style.outline_fill_opa > LV_OPA_TRANSP) {
            for (size_t i = 1; i + 1 < poly.size(); ++i) {
                fill.p[0] = at(poly[0].x, poly[0].y);
                fill.p[1] = at(poly[i].x, poly[i].y);
                fill.p[2] = at(poly[i + 1].x, poly[i + 1].y);
                lv_draw_triangle(layer, &fill);
            }
        }
        for (size_t i = 0; i < poly.size(); ++i) {
            const auto& a = poly[i];
            const auto& b = poly[(i + 1) % poly.size()];
            edge.p1 = at(a.x, a.y);
            edge.p2 = at(b.x, b.y);
            lv_draw_line(layer, &edge);
        }
    }
}

} // namespace helix
