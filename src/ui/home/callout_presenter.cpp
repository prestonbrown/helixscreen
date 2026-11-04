// SPDX-License-Identifier: GPL-3.0-or-later
#include "callout_presenter.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

namespace helix::ui {

namespace {

const CalloutPresenter::Chip* part_for(const CalloutPresenter::Parts& parts, CalloutKind kind) {
    for (const auto& c : parts.chips)
        if (c.kind == kind)
            return &c;
    if (parts.toolhead && parts.toolhead->kind == kind)
        return &*parts.toolhead;
    return nullptr;
}

} // namespace

bool CalloutPresenter::place_image(lv_obj_t* img, const CalloutRect* moved) {
    if (!img)
        return false;
    const int32_t x = moved ? moved->x : 0;
    const int32_t y = moved ? moved->y : 0;
    const int32_t w = moved ? moved->w : LV_PCT(100);
    const int32_t h = moved ? moved->h : LV_PCT(100);
    if (lv_obj_get_style_x(img, LV_PART_MAIN) == x && lv_obj_get_style_y(img, LV_PART_MAIN) == y &&
        lv_obj_get_style_width(img, LV_PART_MAIN) == w &&
        lv_obj_get_style_height(img, LV_PART_MAIN) == h)
        return false;
    // DECLARATIVE_OK: measured callout layout
    lv_obj_set_pos(img, x, y);
    // DECLARATIVE_OK: measured callout layout
    lv_obj_set_size(img, w, h);
    return true;
}

CalloutLayout CalloutPresenter::apply(const Parts& parts, const ImageRegions* regions,
                                      int natural_w, int natural_h, const Config& config) {
    image_changed_ = false;
    CalloutLayout out;
    const Chip* probe_part = nullptr;
    for (const auto& c : parts.chips)
        if (c.chip) {
            probe_part = &c;
            break;
        }
    if (!parts.area || !probe_part)
        return out;

    CalloutLayoutInput in;
    in.area_w = lv_obj_get_content_width(parts.area);
    in.area_h = lv_obj_get_content_height(parts.area);
    in.single_cell = config.single_cell;
    if (in.area_w <= 0 || in.area_h <= 0) {
        image_changed_ = place_image(parts.image, nullptr);
        return out;
    }
    in.tagged = regions != nullptr;
    in.image_w = regions ? regions->src_w : natural_w;
    in.image_h = regions ? regions->src_h : natural_h;
    in.chip_h = config.chip_h;
    in.gap = config.gap;
    in.min_line = config.min_line;
    in.leader_min_distance = config.leader_min_distance;

    const auto anchor = [&](CalloutKind k) -> std::optional<NormPoint> {
        return regions ? region_anchor(*regions, k) : std::nullopt;
    };
    // The mode pass sheds by the phase's rule in both lists: the engine erases
    // a shed kind from budget and active alike, so an active chip never lands
    // without budgeted room.
    for (const Chip& c : parts.chips) {
        if (c.in_budget)
            in.budget.push_back({c.kind, c.w_budget, anchor(c.kind), c.budget_expendable,
                                 c.w_budget_unlabeled, c.h});
        if (c.active)
            in.active.push_back(
                {c.kind, c.w_full, anchor(c.kind), c.budget_expendable, c.w_full_unlabeled, c.h});
    }
    out = compute_callout_layout(in);
    if (spdlog::should_log(spdlog::level::trace)) {
        int widest = 0;
        std::string widths;
        for (const auto& c : in.budget) {
            widest = std::max(widest, c.w);
            widths += fmt::format(" {}:{}/{}", static_cast<int>(c.kind), c.w, c.w_unlabeled);
        }
        spdlog::trace("[CalloutPresenter] area {}x{}, image {}x{} at {},{}, widest budget chip {}, "
                      "mode {}, budget kind:w/unlabeled{}",
                      in.area_w, in.area_h, out.image.w, out.image.h, out.image.x, out.image.y,
                      widest, static_cast<int>(out.mode), widths);
    }
    if (out.mode == CalloutMode::Pinned) {
        // The budget decided the mode; the chips placed on the picture are
        // compact. Without the budget the second pass cannot pick another mode.
        in.budget.clear();
        in.chip_h = config.compact_chip_h;
        in.active.clear();
        for (const Chip& c : parts.chips)
            if (c.active)
                in.active.push_back(
                    {c.kind, c.w_compact, anchor(c.kind), c.expendable, 0, c.h_compact});
        if (parts.toolhead)
            in.toolhead = CalloutChipIn{parts.toolhead->kind, parts.toolhead->w_compact,
                                        anchor(parts.toolhead->kind)};
        const bool unlabeled = out.unlabeled;
        out = compute_callout_layout(in);
        out.unlabeled = unlabeled;
    } else {
        // The mode stays fixed for the phase: a shed chip whose reading turns
        // unusual docks into the room the line mode left free instead of
        // relayouting the picture. With no free room it stays off: dock_free's
        // fallback row would land on the band's chips.
        for (auto it = out.dropped.begin(); it != out.dropped.end();) {
            const Chip* part = part_for(parts, *it);
            std::vector<CalloutChipOut> placed = out.chips;
            if (!part || !part->active || part->expendable ||
                !callout_detail::dock_free(in, out.image,
                                           {{part->kind, part->w_full, std::nullopt, false,
                                             part->w_full_unlabeled, part->h}},
                                           placed)) {
                ++it;
                continue;
            }
            out.chips = std::move(placed);
            it = out.dropped.erase(it);
        }
    }
    spdlog::trace("[CalloutPresenter] placed {} chips, chip_h {}", out.chips.size(), in.chip_h);

    // A one-side or shrunk image leaves the container's contain-fit rect.
    const bool fitted = out.image.w == fit_image(in.area_w, in.area_h, in.image_w, in.image_h).w;
    image_changed_ = place_image(
        parts.image, out.mode == CalloutMode::OneSide || !fitted ? &out.image : nullptr);

    if (regions) {
        for (const Chip& c : parts.chips)
            place_glow(c, *regions, out.image);
    }
    const bool compact = out.mode == CalloutMode::Pinned;
    for (const CalloutChipOut& c : out.chips) {
        if (const Chip* part = part_for(parts, c.kind)) {
            place_line(c, *part);
            place_chip(c, *part, compact);
        }
    }
    return out;
}

void CalloutPresenter::place_glow(const Chip& part, const ImageRegions& r,
                                  const CalloutRect& image) {
    if (!part.glow)
        return;
    using callout_detail::px;
    int x, y, w, h;
    if (part.kind == CalloutKind::Bed) {
        // An ellipse over the bed's near edge, as wide as the edge.
        x = px(std::min(r.bed_left.x, r.bed_right.x), image.x, image.w);
        w = px(std::max(r.bed_left.x, r.bed_right.x), image.x, image.w) - x;
        h = w / 4;
        y = px((r.bed_left.y + r.bed_right.y) / 2, image.y, image.h) - h / 2;
    } else {
        const auto a = region_anchor(r, part.kind);
        if (!a)
            return;
        // A disc centred on the region's point.
        w = h = image.w / 5;
        x = px(a->x, image.x, image.w) - w / 2;
        y = px(a->y, image.y, image.h) - h / 2;
    }
    if (lv_obj_get_style_x(part.glow, LV_PART_MAIN) != x ||
        lv_obj_get_style_y(part.glow, LV_PART_MAIN) != y) {
        // DECLARATIVE_OK: measured callout layout
        lv_obj_set_pos(part.glow, x, y);
    }
    if (lv_obj_get_style_width(part.glow, LV_PART_MAIN) != w ||
        lv_obj_get_style_height(part.glow, LV_PART_MAIN) != h) {
        // DECLARATIVE_OK: measured callout layout
        lv_obj_set_size(part.glow, w, h);
    }
}

void CalloutPresenter::place_line(const CalloutChipOut& c, const Chip& part) {
    const auto k = static_cast<size_t>(c.kind);
    if (!c.has_line || !part.line || k >= line_pts_.size())
        return;
    auto& pts = line_pts_[k];
    const auto v = [](int n) { return static_cast<lv_value_precise_t>(n); };
    const std::array<lv_point_precise_t, 3> want = {
        {{v(c.line_x0), v(c.line_y0)}, {v(c.line_xm), v(c.line_ym)}, {v(c.line_x1), v(c.line_y1)}}};
    const bool same =
        lv_line_get_points(part.line) == pts.data() &&
        std::equal(pts.begin(), pts.end(), want.begin(),
                   [](const auto& p, const auto& q) { return p.x == q.x && p.y == q.y; });
    if (!same) {
        pts = want;
        // DECLARATIVE_OK: measured callout layout
        lv_line_set_points(part.line, pts.data(), pts.size());
    }
}

void CalloutPresenter::place_chip(const CalloutChipOut& c, const Chip& part, bool compact) {
    lv_obj_t* obj = part.chip;
    if (!obj)
        return;
    // Temperatures relayout every tick while heating; an unchanged chip is
    // not rewritten, since every style write invalidates it.
    if (lv_obj_get_style_x(obj, LV_PART_MAIN) != c.rect.x ||
        lv_obj_get_style_y(obj, LV_PART_MAIN) != c.rect.y) {
        // DECLARATIVE_OK: measured callout layout
        lv_obj_set_pos(obj, c.rect.x, c.rect.y);
    }
    if (lv_obj_get_style_width(obj, LV_PART_MAIN) != c.rect.w) {
        // DECLARATIVE_OK: measured callout layout
        lv_obj_set_width(obj, c.rect.w);
    }
    // The label keeps its content width up to what the icons leave, so a
    // chip clamped narrower than its text ends in dots instead of spilling.
    lv_obj_t* label = lv_obj_find_by_name(obj, "chip_text");
    const int chrome_w = lv_obj_get_style_pad_left(obj, LV_PART_MAIN) +
                         lv_obj_get_style_pad_right(obj, LV_PART_MAIN) +
                         2 * lv_obj_get_style_border_width(obj, LV_PART_MAIN);
    const int around_text = chrome_w + part.icons + lv_obj_get_style_pad_column(obj, LV_PART_MAIN);
    const int label_max = std::max(0, c.rect.w - (compact ? chrome_w : around_text));
    if (label && lv_obj_get_style_max_width(label, LV_PART_MAIN) != label_max) {
        // DECLARATIVE_OK: measured callout layout
        lv_obj_set_style_max_width(label, label_max, 0);
    }
}

} // namespace helix::ui
