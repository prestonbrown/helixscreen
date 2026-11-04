// SPDX-License-Identifier: GPL-3.0-or-later
#include "bed_coord_mapper.h"

#include <algorithm>

namespace helix {

BedCoordMapper::BedCoordMapper(float bed_w_mm, float bed_h_mm, int viewport_w_px, int viewport_h_px,
                               float origin_x, float origin_y)
    : origin_x_(origin_x), origin_y_(origin_y), viewport_h_(viewport_h_px) {
    float scale_x = static_cast<float>(viewport_w_px) / bed_w_mm;
    float scale_y = static_cast<float>(viewport_h_px) / bed_h_mm;
    scale_ = std::min(scale_x, scale_y);
    float rendered_w = bed_w_mm * scale_;
    float rendered_h = bed_h_mm * scale_;
    offset_x_ = (static_cast<float>(viewport_w_px) - rendered_w) / 2.0f;
    offset_y_ = (static_cast<float>(viewport_h_px) - rendered_h) / 2.0f;
}

std::pair<float, float> BedCoordMapper::mm_to_px(float x_mm, float y_mm) const {
    // Subtract origin to handle non-zero-based coordinate systems
    float px = offset_x_ + (x_mm - origin_x_) * scale_;
    float bed_h_px = static_cast<float>(viewport_h_) - 2.0f * offset_y_;
    float py = offset_y_ + bed_h_px - (y_mm - origin_y_) * scale_;
    return {px, py};
}

std::pair<float, float> BedCoordMapper::px_to_mm(float x_px, float y_px) const {
    // Invert mm_to_px term by term: undo the X letterbox offset and scale,
    // then the Y flip (bed y=0 is the BOTTOM of the viewport) and offset.
    float x_mm = (x_px - offset_x_) / scale_ + origin_x_;
    float y_mm = (static_cast<float>(viewport_h_) - offset_y_ - y_px) / scale_ + origin_y_;
    return {x_mm, y_mm};
}

PixelRect BedCoordMapper::bbox_to_rect(glm::vec2 bbox_min, glm::vec2 bbox_max) const {
    auto [x1, y1] = mm_to_px(bbox_min.x, bbox_max.y);
    auto [x2, y2] = mm_to_px(bbox_max.x, bbox_min.y);
    float raw_w = x2 - x1, raw_h = y2 - y1;
    float w = std::max(raw_w, kMinTouchTargetPx);
    float h = std::max(raw_h, kMinTouchTargetPx);
    if (w > raw_w)
        x1 -= (w - raw_w) / 2.0f;
    if (h > raw_h)
        y1 -= (h - raw_h) / 2.0f;
    return {x1, y1, w, h};
}

} // namespace helix
