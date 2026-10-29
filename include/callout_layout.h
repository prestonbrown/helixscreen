// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure layout decision for the printer image widget's live callouts: which
// mode the widget draws in, where the image sits, and where every chip and
// leader line goes. No LVGL, so every boundary is unit-testable. The widget
// measures chip widths in the fonts the chips render and feeds them here.

#include "printer_image_regions.h"
#include "sort_small.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

namespace helix {

/// Fan is the part-cooling fan. The ints index per-kind arrays; append only.
enum class CalloutKind {
    Nozzle = 0,
    Bed = 1,
    Chamber = 2,
    Fan = 3,
    Light = 4,
    Toolhead = 5,
    HotendFan = 6,
    AuxFan = 7,
    SpeedFlow = 8,
    OtherTools = 9 ///< every head but the printing one, on a multi-head printer
};
inline constexpr size_t kCalloutKindCount = 10;

/// The `printer_callout_mode` subject ints; XML ref_values read these.
enum class CalloutMode { ImageOnly = 0, Pinned = 1, Docked = 2, BothSides = 3, OneSide = 4 };

struct CalloutRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct CalloutChipIn {
    CalloutKind kind = CalloutKind::Nozzle;
    int w = 0;                       ///< measured width including padding and comfort margin
    std::optional<NormPoint> anchor; ///< tagged point; nullopt when the image has none for it
    /// Shows an expected value: dropped before anything else when the chips do not fit.
    bool expendable = false;
    /// Its width with its label ("Part") left off, which is the first width a
    /// layout gives up; 0 when it has no label to lose.
    int w_unlabeled = 0;
    /// Measured height of a chip taller than one text line; 0 is the layout's chip_h.
    int h = 0;
};

struct CalloutChipOut {
    CalloutKind kind = CalloutKind::Nozzle;
    CalloutRect rect;
    bool has_line = false;
    int line_x0 = 0, line_y0 = 0; ///< the tagged point
    int line_xm = 0, line_ym = 0; ///< elbow: 45 degrees from the point, then straight on
    int line_x1 = 0, line_y1 = 0; ///< the chip's inner edge, at its centre
};

struct CalloutPoint {
    int x = 0;
    int y = 0;
};

struct CalloutLayoutInput {
    int area_w = 0; ///< the image container's content box
    int area_h = 0;
    bool single_cell = false; ///< the grid granted one cell on both axes
    int image_w = 0;          ///< natural image size; only the aspect is used
    int image_h = 0;
    bool tagged = false; ///< the image has a regions entry
    int chip_h = 0;      ///< a one-line chip's height; a taller chip carries its own h
    int gap = 0;         ///< between stacked chips, and from the area edge
    int min_line = 0;    ///< shortest leader run worth drawing
    /// Pinned chips draw a leader only when they land farther than this from their
    /// point; 0 keeps pinned chips line-free.
    int leader_min_distance = 0;
    /// Worst-case chips this printer can ever show. Decides the MODE, so the
    /// image never moves when a chip comes or goes. Empty rules out the line
    /// modes: a tagged image then draws Pinned.
    std::vector<CalloutChipIn> budget;
    /// Chips showing now. Only these get positions.
    std::vector<CalloutChipIn> active;
    /// Nozzle+fan combined, for pinned mode when both are active.
    std::optional<CalloutChipIn> toolhead;
};

struct CalloutLayout {
    CalloutMode mode = CalloutMode::ImageOnly;
    CalloutRect image;
    std::vector<CalloutChipOut> chips;
    bool toolhead_merged = false;
    /// Expendable chips left off because they did not fit.
    std::vector<CalloutKind> dropped;
    /// Labeled chips are laid out at their w_unlabeled, so they show no label.
    bool unlabeled = false;
};

/// Where a tagged image puts `k`'s chip: its part's point, the nozzle for the
/// toolhead chip, and the middle of the bed's near edge for the bed.
[[nodiscard]] inline std::optional<NormPoint> region_anchor(const ImageRegions& r, CalloutKind k) {
    switch (k) {
    case CalloutKind::Nozzle:
    case CalloutKind::Toolhead:
    case CalloutKind::HotendFan:
        return r.nozzle;
    case CalloutKind::Fan:
        return r.part_fan;
    case CalloutKind::Bed:
        return NormPoint{(r.bed_left.x + r.bed_right.x) / 2, (r.bed_left.y + r.bed_right.y) / 2};
    case CalloutKind::Chamber:
        return r.chamber;
    case CalloutKind::Light:
        return r.light;
    case CalloutKind::AuxFan:
    case CalloutKind::SpeedFlow:
    case CalloutKind::OtherTools:
        return std::nullopt;
    }
    return std::nullopt;
}

/// Contain-fit the image into the area, centred. Zero rect when either is empty.
[[nodiscard]] inline CalloutRect fit_image(int area_w, int area_h, int img_w, int img_h) {
    if (area_w <= 0 || area_h <= 0 || img_w <= 0 || img_h <= 0)
        return {};
    CalloutRect r;
    if (int64_t(area_w) * img_h <= int64_t(area_h) * img_w) {
        r.w = area_w;
        r.h = int(int64_t(area_w) * img_h / img_w);
    } else {
        r.h = area_h;
        r.w = int(int64_t(area_h) * img_w / img_h);
    }
    r.x = (area_w - r.w) / 2;
    r.y = (area_h - r.h) / 2;
    return r;
}

/// The point of `img` (a fit_image() rect) under area pixel (x, y), or nullopt
/// when that pixel is in the letterbox rather than on the image.
[[nodiscard]] inline std::optional<NormPoint> image_point_at(const CalloutRect& img, int x, int y) {
    if (img.w <= 0 || img.h <= 0 || x < img.x || y < img.y || x >= img.x + img.w ||
        y >= img.y + img.h)
        return std::nullopt;
    return NormPoint{float(x - img.x) / float(img.w), float(y - img.y) / float(img.h)};
}

/// Resolve overlaps along one axis. `start` holds each item's ideal start,
/// sorted ascending; items are pushed apart by `gap`, then pulled back so the
/// last ends by `hi`. The caller has already checked the items fit in [lo, hi].
inline void spread_1d(std::vector<int>& start, const std::vector<int>& size, int lo, int hi,
                      int gap) {
    for (size_t i = 0; i < start.size(); ++i) {
        const int min_s = i ? start[i - 1] + size[i - 1] + gap : lo;
        start[i] = std::max(start[i], min_s);
    }
    for (size_t i = start.size(); i-- > 0;) {
        const int max_s = (i + 1 < start.size() ? start[i + 1] - gap : hi) - size[i];
        start[i] = std::min(start[i], max_s);
    }
}

namespace callout_detail {

/// Where a leader from point `a` to chip edge `c` bends: 45 degrees toward the
/// chip, then straight into it. `horizontal` leaders (side bands) end in a level
/// run, the others (bands above and below) in a vertical one. When 45 degrees
/// leaves less than `min_line` of straight run, the diagonal steepens to keep
/// that stub; it never runs back past the point.
inline CalloutPoint leader_elbow(CalloutPoint a, CalloutPoint c, int min_line, bool horizontal) {
    const int a_run = horizontal ? a.x : a.y, c_run = horizontal ? c.x : c.y;
    const int run = std::abs(c_run - a_run);
    const int cross = std::abs(horizontal ? c.y - a.y : c.x - a.x);
    const int diag = cross <= run - min_line ? cross : std::max(0, run - min_line);
    const int bend = a_run + (c_run >= a_run ? diag : -diag);
    return horizontal ? CalloutPoint{bend, c.y} : CalloutPoint{c.x, bend};
}

inline int px(float n, int origin, int extent) {
    return origin + int(n * float(extent));
}

inline int chip_h_of(const CalloutLayoutInput& in, const CalloutChipIn& c) {
    return c.h > 0 ? c.h : in.chip_h;
}

inline CalloutRect clamp_into(CalloutRect r, int area_w, int area_h) {
    r.w = std::min(r.w, area_w);
    r.h = std::min(r.h, area_h);
    r.x = std::clamp(r.x, 0, std::max(0, area_w - r.w));
    r.y = std::clamp(r.y, 0, std::max(0, area_h - r.h));
    return r;
}

/// Chips with no usable point: a column in the widest side band if one fits
/// them, else a bottom-edge row filled right to left, wrapping upward.
inline void place_docked(const CalloutLayoutInput& in, const CalloutRect& img,
                         const std::vector<CalloutChipIn>& chips,
                         std::vector<CalloutChipOut>& out) {
    if (chips.empty())
        return;
    int widest = 0;
    for (const auto& c : chips)
        widest = std::max(widest, c.w);
    const int band_x = img.x + img.w;
    const int band_w = in.area_w - band_x;
    int stack_h = int(chips.size() - 1) * in.gap;
    for (const auto& c : chips)
        stack_h += chip_h_of(in, c);
    if (band_w >= widest + 2 * in.gap && stack_h <= in.area_h - 2 * in.gap) {
        int y = (in.area_h - stack_h) / 2;
        for (const auto& c : chips) {
            const int h = chip_h_of(in, c);
            out.push_back({c.kind, {band_x + in.gap, y, c.w, h}});
            y += h + in.gap;
        }
        return;
    }
    // Each row's chips share its bottom edge; the next row starts above its tallest chip.
    int x = in.area_w - in.gap;
    int row_bottom = in.area_h - in.gap;
    int row_h = 0;
    for (const auto& c : chips) {
        if (x - c.w < in.gap && x != in.area_w - in.gap) {
            x = in.area_w - in.gap;
            row_bottom -= row_h + in.gap;
            row_h = 0;
        }
        const int h = chip_h_of(in, c);
        row_h = std::max(row_h, h);
        x -= c.w;
        out.push_back({c.kind, clamp_into({x, row_bottom - h, c.w, h}, in.area_w, in.area_h)});
        x -= in.gap;
    }
}

/// The first `n` of `chips` grouped into columns: chips whose x-ranges intersect,
/// transitively. Each column's indices are sorted top to bottom.
inline std::vector<std::vector<size_t>> x_columns(const std::vector<CalloutChipOut>& chips,
                                                  size_t n) {
    std::vector<size_t> by_x(n);
    for (size_t i = 0; i < n; ++i)
        by_x[i] = i;
    sort_small(by_x.begin(), by_x.end(),
               [&](size_t a, size_t b) { return chips[a].rect.x < chips[b].rect.x; });
    std::vector<std::vector<size_t>> cols;
    for (size_t b = 0; b < by_x.size();) {
        size_t e = b + 1;
        int right = chips[by_x[b]].rect.x + chips[by_x[b]].rect.w;
        for (; e < by_x.size() && chips[by_x[e]].rect.x < right; ++e)
            right = std::max(right, chips[by_x[e]].rect.x + chips[by_x[e]].rect.w);
        std::vector<size_t> col(by_x.begin() + long(b), by_x.begin() + long(e));
        sort_small(col.begin(), col.end(),
                   [&](size_t p, size_t q) { return chips[p].rect.y < chips[q].rect.y; });
        cols.push_back(std::move(col));
        b = e;
    }
    return cols;
}

/// Which chip a column too tall to stack gives up to the docked row first.
/// The nozzle and the toolhead never do. The hotend fan goes before everything.
inline int dock_rank(CalloutKind k) {
    switch (k) {
    case CalloutKind::HotendFan:
        return -1;
    case CalloutKind::Light:
    case CalloutKind::AuxFan:
        return 0;
    case CalloutKind::Fan:
    case CalloutKind::SpeedFlow:
    case CalloutKind::OtherTools:
        return 1;
    case CalloutKind::Chamber:
        return 2;
    case CalloutKind::Bed:
        return 3;
    case CalloutKind::Nozzle:
    case CalloutKind::Toolhead:
        break;
    }
    return 4;
}

/// Pinned chips (the first `pinned` of `chips`) sit on their points and can land
/// on each other. Each x-column is moved apart vertically by spread_1d, above the
/// docked row (the rest of `chips`); a lone chip moves only to clear that row.
/// `src` holds the pinned chips' inputs, in the order of the first chips.size() entries.
/// Returns the index of the chip a column too tall to stack gives up to the
/// docked row, before moving anything, or nullopt once every column fits.
inline std::optional<size_t> slide_apart(const CalloutLayoutInput& in,
                                         std::vector<CalloutChipOut>& chips,
                                         const std::vector<CalloutChipIn>& src) {
    const size_t pinned = src.size();
    // An expendable chip gives way before any other; the rest go by dock_rank.
    const auto rank = [&](size_t i) {
        return dock_rank(chips[i].kind) - (src[i].expendable ? 10 : 0);
    };
    int hi = in.area_h - in.gap;
    for (size_t i = pinned; i < chips.size(); ++i)
        hi = std::min(hi, chips[i].rect.y - in.gap);
    const auto cols = x_columns(chips, pinned);
    for (const auto& col : cols) {
        int total = in.gap * int(col.size() - 1);
        for (size_t i : col)
            total += chips[i].rect.h;
        if (total <= hi - in.gap)
            continue;
        std::optional<size_t> victim;
        for (size_t i : col)
            if (rank(i) < 4 && (!victim || rank(i) < rank(*victim)))
                victim = i;
        if (victim)
            return victim;
    }
    for (const auto& col : cols) {
        if (col.size() == 1) {
            auto& r = chips[col[0]].rect;
            r.y = std::min(r.y, hi - r.h);
        } else {
            std::vector<int> start, size;
            for (size_t i : col) {
                start.push_back(chips[i].rect.y);
                size.push_back(chips[i].rect.h);
            }
            spread_1d(start, size, in.gap, hi, in.gap);
            for (size_t k = 0; k < col.size(); ++k)
                chips[col[k]].rect.y = start[k];
        }
        for (size_t i : col)
            chips[i].rect = clamp_into(chips[i].rect, in.area_w, in.area_h);
    }
    return std::nullopt;
}

inline bool rects_near(const CalloutRect& a, const CalloutRect& b, int margin) {
    return a.x < b.x + b.w + margin && b.x < a.x + a.w + margin && a.y < b.y + b.h + margin &&
           b.y < a.y + a.h + margin;
}

/// Chips with no tagged point, placed in whatever the line modes leave free: the
/// right and left strips beside the image, else the bottom and top strips, each
/// hugging the area edge and clear of the image and of everything already in `out`.
/// A chip that fits nowhere falls back to place_docked and the result is false.
inline bool dock_free(const CalloutLayoutInput& in, const CalloutRect& img,
                      const std::vector<CalloutChipIn>& chips, std::vector<CalloutChipOut>& out) {
    bool all_fit = true;
    for (const auto& c : chips) {
        const auto free_at = [&](const CalloutRect& r) {
            if (r.x < in.gap || r.y < in.gap || r.x + r.w > in.area_w - in.gap ||
                r.y + r.h > in.area_h - in.gap || rects_near(r, img, 0))
                return false;
            return std::none_of(out.begin(), out.end(), [&](const CalloutChipOut& o) {
                return rects_near(r, o.rect, in.gap);
            });
        };
        const int h = chip_h_of(in, c);
        // Along a strip, the first free spot either starts the strip or sits
        // just past something it must clear: the image, or a placed chip by a
        // gap. Trying only those positions finds what a pixel-by-pixel scan
        // would, in the same order, at a few tries per obstacle.
        const auto first_free = [&](bool along_y, int fixed, bool from_low) {
            const int size = along_y ? h : c.w;
            const int lo = in.gap;
            const int hi = (along_y ? in.area_h : in.area_w) - in.gap - size;
            std::vector<int> at{lo, hi};
            const auto edges = [&](const CalloutRect& r, int margin) {
                const int pos = along_y ? r.y : r.x;
                const int ext = along_y ? r.h : r.w;
                at.push_back(pos + ext + margin);
                at.push_back(pos - margin - size);
            };
            edges(img, 0);
            for (const auto& o : out)
                edges(o.rect, in.gap);
            std::sort(at.begin(), at.end());
            if (!from_low)
                std::reverse(at.begin(), at.end());
            std::optional<CalloutRect> found;
            for (const int v : at) {
                if (v < lo || v > hi)
                    continue;
                const CalloutRect r =
                    along_y ? CalloutRect{fixed, v, c.w, h} : CalloutRect{v, fixed, c.w, h};
                if (free_at(r)) {
                    found = r;
                    break;
                }
            }
            return found;
        };
        std::optional<CalloutRect> spot;
        for (const int x : {in.area_w - in.gap - c.w, in.gap})
            if (!spot)
                spot = first_free(true, x, true);
        for (const int y : {in.area_h - in.gap - h, in.gap})
            if (!spot)
                spot = first_free(false, y, false);
        if (spot) {
            out.push_back({c.kind, *spot});
        } else {
            place_docked(in, img, {c}, out);
            all_fit = false;
        }
    }
    return all_fit;
}

/// Gives a pinned chip a leader when it sits farther than leader_min_distance from
/// its point: 45 degrees toward the chip, then straight into the nearest edge.
inline void add_pinned_leader(const CalloutLayoutInput& in, CalloutChipOut& o,
                              const CalloutPoint& pt) {
    if (in.leader_min_distance <= 0)
        return;
    const int cx = o.rect.x + o.rect.w / 2, cy = o.rect.y + o.rect.h / 2;
    const int64_t dx = cx - pt.x, dy = cy - pt.y;
    const int64_t m = in.leader_min_distance;
    if (dx * dx + dy * dy <= m * m)
        return;
    const bool horizontal = std::abs(dx) > std::abs(dy);
    if (horizontal) {
        o.line_x1 = dx > 0 ? o.rect.x : o.rect.x + o.rect.w;
        o.line_y1 = cy;
    } else {
        o.line_x1 = cx;
        o.line_y1 = dy > 0 ? o.rect.y : o.rect.y + o.rect.h;
    }
    const CalloutPoint e = leader_elbow(pt, {o.line_x1, o.line_y1}, in.min_line, horizontal);
    o.has_line = true;
    o.line_x0 = pt.x;
    o.line_y0 = pt.y;
    o.line_xm = e.x;
    o.line_ym = e.y;
}

inline CalloutChipOut chip_at(const CalloutChipIn& c, int x, int y, int h, int ax, int ay) {
    CalloutChipOut o{c.kind, {x, y, c.w, h}, true};
    o.line_x0 = ax;
    o.line_y0 = ay;
    return o;
}

/// Stack `chips` (sorted by the point's position along the stacking axis) in a
/// column at x = band_pos (horizontal bands) or a row at y = band_pos (vertical
/// bands). `band_pos` is the chips' near edge when `left_or_top`, else their far edge.
/// Returns false if they do not fit the axis.
inline bool stack(const CalloutLayoutInput& in, const CalloutRect& img, bool horizontal_band,
                  bool left_or_top, int band_pos, std::vector<CalloutChipIn> chips,
                  std::vector<CalloutChipOut>& out) {
    if (chips.empty())
        return true;
    const auto key = [&](const CalloutChipIn& c) {
        return horizontal_band ? c.anchor->y : c.anchor->x;
    };
    sort_small(chips.begin(), chips.end(), [&](auto& a, auto& b) { return key(a) < key(b); });
    std::vector<int> start, size;
    for (const auto& c : chips) {
        const int a =
            horizontal_band ? px(c.anchor->y, img.y, img.h) : px(c.anchor->x, img.x, img.w);
        const int s = horizontal_band ? chip_h_of(in, c) : c.w;
        start.push_back(a - s / 2);
        size.push_back(s);
    }
    const int extent = horizontal_band ? in.area_h : in.area_w;
    int total = in.gap * int(chips.size() - 1);
    for (int s : size)
        total += s;
    if (total > extent - 2 * in.gap)
        return false;
    spread_1d(start, size, in.gap, extent - in.gap, in.gap);
    for (size_t i = 0; i < chips.size(); ++i) {
        const auto& c = chips[i];
        const int ax = px(c.anchor->x, img.x, img.w), ay = px(c.anchor->y, img.y, img.h);
        const int h = chip_h_of(in, c);
        CalloutChipOut o;
        if (horizontal_band) {
            const int x = left_or_top ? band_pos : band_pos - c.w;
            o = chip_at(c, x, start[i], h, ax, ay);
            o.line_x1 = left_or_top ? x + c.w : x;
            o.line_y1 = start[i] + h / 2;
        } else {
            const int y = left_or_top ? band_pos : band_pos - h;
            o = chip_at(c, start[i], y, h, ax, ay);
            o.line_x1 = start[i] + c.w / 2;
            o.line_y1 = left_or_top ? y + h : y;
        }
        const CalloutPoint e =
            leader_elbow({ax, ay}, {o.line_x1, o.line_y1}, in.min_line, horizontal_band);
        o.line_xm = e.x;
        o.line_ym = e.y;
        out.push_back(o);
    }
    return true;
}

/// How far below its contain-fit size the image may shrink to make room for
/// leader-line columns; past this, the picture loses more than the lines give.
/// One column buys more: without it a wide picture falls to Pinned, its chips
/// laid over the photo, which reads worse than a smaller picture with lines.
inline constexpr int kMaxImageShrinkPctOneSide = 40;
inline constexpr int kMaxImageShrinkPctBothSides = 20;

/// The side band's axis (true: bands left and right) and the room one column of
/// budget chips needs across it, for the image at `img`.
inline std::pair<bool, int> line_column(const CalloutLayoutInput& in, const CalloutRect& img) {
    const bool horiz = (in.area_w - img.w) >= (in.area_h - img.h);
    int widest = 0, tallest = in.chip_h;
    for (const auto& c : in.budget) {
        widest = std::max(widest, c.w);
        tallest = std::max(tallest, chip_h_of(in, c));
    }
    return {horiz, (horiz ? widest : tallest) + in.gap + in.min_line};
}

/// The chips of the toolhead: one block in the nozzle's band.
inline bool in_toolhead_group(CalloutKind k) {
    return k == CalloutKind::Nozzle || k == CalloutKind::Fan || k == CalloutKind::HotendFan;
}

/// Which band each kind's chip goes in: true is the right band.
using BandSides = std::array<bool, kCalloutKindCount>;

/// A chip goes in the band on its point's side of the image, the toolhead group
/// follows the nozzle, and a chip with no point takes the band with fewer chips
/// (right on a tie). `all_right` puts everything in the right band.
inline BandSides band_sides(const std::vector<CalloutChipIn>& chips, bool all_right) {
    BandSides sides;
    sides.fill(true);
    if (all_right)
        return sides;
    const auto right_of = [](const CalloutChipIn& c) { return c.anchor->x >= 0.5f; };
    std::optional<bool> nozzle_right;
    for (const auto& c : chips)
        if (c.kind == CalloutKind::Nozzle && c.anchor)
            nozzle_right = right_of(c);
    int left = 0, right = 0;
    for (const auto& c : chips) {
        if (!c.anchor && !(nozzle_right && in_toolhead_group(c.kind)))
            continue;
        const bool r = (nozzle_right && in_toolhead_group(c.kind)) ? *nozzle_right : right_of(c);
        sides[size_t(c.kind)] = r;
        ++(r ? right : left);
    }
    for (const auto& c : chips)
        if (!c.anchor && !(nozzle_right && in_toolhead_group(c.kind))) {
            const bool r = left >= right;
            sides[size_t(c.kind)] = r;
            ++(r ? right : left);
        }
    return sides;
}

/// One band's worth of stacked chips: the toolhead block or a single chip.
struct BandUnit {
    std::vector<CalloutChipIn> chips; ///< top to bottom
    std::optional<float> y;           ///< the first chip's point, in area pixels
    int h = 0;
};

/// Chips of one band as units. Inside the toolhead block chips follow their
/// points top to bottom so no two leaders cross; on a shared point the part fan
/// sits above the hotend fan, above the nozzle.
inline std::vector<BandUnit> band_units(const CalloutLayoutInput& in, const CalloutRect& img,
                                        const std::vector<CalloutChipIn>& chips) {
    const auto py = [&](const CalloutChipIn& c) {
        return c.anchor ? float(px(c.anchor->y, img.y, img.h)) : 1e9f;
    };
    const auto rank = [](CalloutKind k) {
        return k == CalloutKind::Fan ? 0 : k == CalloutKind::HotendFan ? 1 : 2;
    };
    std::vector<CalloutChipIn> group;
    std::vector<BandUnit> units;
    for (const auto& c : chips) {
        if (in_toolhead_group(c.kind)) {
            group.push_back(c);
        } else {
            BandUnit u;
            u.chips = {c};
            u.h = chip_h_of(in, c);
            if (c.anchor)
                u.y = py(c);
            units.push_back(u);
        }
    }
    if (!group.empty()) {
        sort_small(group.begin(), group.end(), [&](const CalloutChipIn& a, const CalloutChipIn& b) {
            return py(a) != py(b) ? py(a) < py(b) : rank(a.kind) < rank(b.kind);
        });
        BandUnit u;
        u.h = int(group.size() - 1) * in.gap;
        for (const auto& c : group)
            u.h += chip_h_of(in, c);
        if (group.front().anchor)
            u.y = py(group.front());
        u.chips = std::move(group);
        units.insert(units.begin(), std::move(u));
    }
    return units;
}

/// Spreads a band's units evenly over its full height, anchored units in anchor
/// order and unanchored ones in the slots that keep the anchored nearest their
/// points. Returns each unit's top, in the order the units end up; false when
/// the units do not fit.
inline bool band_slots(const CalloutLayoutInput& in, std::vector<BandUnit>& units,
                       std::vector<int>& tops) {
    const size_t n = units.size();
    tops.clear();
    if (n == 0)
        return true;
    int sum = 0;
    for (const auto& u : units)
        sum += u.h;
    const int room = in.area_h - 2 * in.gap;
    if (sum + int(n - 1) * in.gap > room)
        return false;
    std::vector<BandUnit> anchored, loose;
    for (auto& u : units)
        (u.y ? anchored : loose).push_back(std::move(u));
    sort_small(anchored.begin(), anchored.end(),
               [](const BandUnit& a, const BandUnit& b) { return *a.y < *b.y; });
    const auto spread = [&](const std::vector<BandUnit>& order) {
        std::vector<int> t;
        const float step = n > 1 ? float(room - sum) / float(n - 1) : 0.0f;
        float y = float(in.gap);
        for (const auto& u : order) {
            t.push_back(int(y + 0.5f));
            y += float(u.h) + step;
        }
        return t;
    };
    if (n == 1) {
        units = anchored.empty() ? loose : anchored;
        const int h = units[0].h;
        tops = {units[0].y ? std::clamp(int(*units[0].y) - h / 2, in.gap, in.area_h - in.gap - h)
                           : in.gap};
        return true;
    }
    std::vector<BandUnit> best;
    std::vector<int> best_tops;
    float best_cost = 0;
    for (uint32_t mask = 0; mask < (1u << n); ++mask) {
        if (size_t(__builtin_popcount(mask)) != loose.size())
            continue;
        std::vector<BandUnit> order;
        size_t ai = 0, li = 0;
        for (size_t k = 0; k < n; ++k)
            order.push_back((mask >> k) & 1 ? loose[li++] : anchored[ai++]);
        const auto t = spread(order);
        float cost = 0;
        for (size_t k = 0; k < n; ++k)
            if (order[k].y)
                cost += std::abs(float(t[k]) + float(order[k].h) / 2 - *order[k].y);
        if (best.empty() || cost < best_cost) {
            best = order;
            best_tops = t;
            best_cost = cost;
        }
    }
    units = std::move(best);
    tops = std::move(best_tops);
    return true;
}

/// The chips of `chips` that `sides` puts in the right (or left) band, placed
/// against the area edge with leaders running 45 degrees, then level into the chip.
inline bool place_band(const CalloutLayoutInput& in, const CalloutRect& img, bool right,
                       const BandSides& sides, const std::vector<CalloutChipIn>& chips,
                       std::vector<CalloutChipOut>& out) {
    std::vector<CalloutChipIn> mine;
    for (const auto& c : chips)
        if (sides[size_t(c.kind)] == right)
            mine.push_back(c);
    auto units = band_units(in, img, mine);
    std::vector<int> tops;
    if (!band_slots(in, units, tops))
        return false;
    for (size_t k = 0; k < units.size(); ++k) {
        int y = tops[k];
        for (const auto& c : units[k].chips) {
            const int x = right ? in.area_w - in.gap - c.w : in.gap;
            const int h = chip_h_of(in, c);
            CalloutChipOut o;
            if (c.anchor) {
                const int ax = px(c.anchor->x, img.x, img.w), ay = px(c.anchor->y, img.y, img.h);
                o = chip_at(c, x, y, h, ax, ay);
                o.line_x1 = right ? x : x + c.w;
                o.line_y1 = y + h / 2;
                const CalloutPoint e =
                    leader_elbow({ax, ay}, {o.line_x1, o.line_y1}, in.min_line, true);
                o.line_xm = e.x;
                o.line_ym = e.y;
            } else {
                o.kind = c.kind;
                o.rect = {x, y, c.w, h};
            }
            out.push_back(o);
            y += h + in.gap;
        }
    }
    return true;
}

/// Both sides, else one side, with the image centred at `centred`. In side bands
/// every chip lands by place_band; in bands above and below, chips with no point
/// dock in the space left (`in` then holds only chips with points).
inline bool try_line_modes_at(const CalloutLayoutInput& in, const CalloutRect& centred,
                              const std::vector<CalloutChipIn>& loose_budget,
                              const std::vector<CalloutChipIn>& loose_active, CalloutLayout& out) {
    // Plain locals, not a structured binding: clang before 16 rejects a lambda
    // capturing a binding.
    CalloutLayoutInput widths = in;
    widths.budget.insert(widths.budget.end(), loose_budget.begin(), loose_budget.end());
    const auto column = line_column(widths, centred);
    const bool horiz = column.first;
    const int col = column.second;
    const int far_edge = (horiz ? in.area_w : in.area_h) - in.gap;
    const auto split = [&](const std::vector<CalloutChipIn>& v, bool near_half) {
        std::vector<CalloutChipIn> r;
        for (const auto& c : v)
            if (((horiz ? c.anchor->x : c.anchor->y) < 0.5f) == near_half)
                r.push_back(c);
        return r;
    };
    const auto joined = [](std::vector<CalloutChipIn> a, const std::vector<CalloutChipIn>& b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    };
    const auto budget = joined(in.budget, loose_budget);
    const auto active = joined(in.active, loose_active);

    std::vector<CalloutChipOut> scratch;
    const int band = horiz ? centred.x : centred.y;
    if (horiz) {
        const BandSides sides = band_sides(budget, false);
        if (band >= col && place_band(in, centred, false, sides, budget, scratch) &&
            place_band(in, centred, true, sides, budget, scratch)) {
            out.image = centred;
            out.chips.clear();
            place_band(in, centred, false, sides, active, out.chips);
            place_band(in, centred, true, sides, active, out.chips);
            out.mode = CalloutMode::BothSides;
            return true;
        }
    } else if (band >= col &&
               stack(in, centred, horiz, true, in.gap, split(in.budget, true), scratch) &&
               stack(in, centred, horiz, false, far_edge, split(in.budget, false), scratch) &&
               dock_free(in, centred, loose_budget, scratch)) {
        out.image = centred;
        out.chips.clear();
        stack(in, centred, horiz, true, in.gap, split(in.active, true), out.chips);
        stack(in, centred, horiz, false, far_edge, split(in.active, false), out.chips);
        dock_free(in, centred, loose_active, out.chips);
        out.mode = CalloutMode::BothSides;
        return true;
    }

    const int free_extent = horiz ? in.area_w - centred.w : in.area_h - centred.h;
    CalloutRect moved = centred;
    (horiz ? moved.x : moved.y) = 0;
    scratch.clear();
    if (horiz) {
        const BandSides sides = band_sides(budget, true);
        if (free_extent >= col && place_band(in, moved, true, sides, budget, scratch)) {
            out.image = moved;
            out.chips.clear();
            place_band(in, moved, true, sides, active, out.chips);
            out.mode = CalloutMode::OneSide;
            return true;
        }
    } else if (free_extent >= col && stack(in, moved, horiz, false, far_edge, in.budget, scratch) &&
               dock_free(in, moved, loose_budget, scratch)) {
        out.image = moved;
        out.chips.clear();
        stack(in, moved, horiz, false, far_edge, in.active, out.chips);
        dock_free(in, moved, loose_active, out.chips);
        out.mode = CalloutMode::OneSide;
        return true;
    }
    return false;
}

/// Line modes at the contain-fit size, else with the image shrunk just enough
/// for one side (up to `one_side_shrink_pct`), then for both (up to
/// kMaxImageShrinkPctBothSides). Fit is tested
/// against the BUDGET and only the ACTIVE chips are placed, so the image never
/// moves as chips come and go. Chips with no tagged point have no line; they dock
/// in the space the line modes leave, and must fit there too.
inline bool try_line_modes(const CalloutLayoutInput& in, CalloutLayout& out,
                           int one_side_shrink_pct) {
    CalloutLayoutInput lined = in;
    std::vector<CalloutChipIn> loose_budget, loose_active;
    const auto split_loose = [](const std::vector<CalloutChipIn>& all,
                                std::vector<CalloutChipIn>& tied,
                                std::vector<CalloutChipIn>& loose) {
        tied.clear();
        for (const auto& c : all)
            (c.anchor ? tied : loose).push_back(c);
    };
    split_loose(in.budget, lined.budget, loose_budget);
    split_loose(in.active, lined.active, loose_active);
    if (lined.budget.empty())
        return false;

    const CalloutRect fitted = out.image;
    if (try_line_modes_at(lined, fitted, loose_budget, loose_active, out))
        return true;
    // Sized from the same widths try_line_modes_at() accepts against: the
    // loose chips ride in the side band too.
    const auto [horiz, col] = line_column(in, fitted);
    const int area = horiz ? in.area_w : in.area_h;
    const int full = horiz ? fitted.w : fitted.h;
    for (const int columns : {1, 2}) {
        const int max_shrink_pct = columns == 1 ? one_side_shrink_pct : kMaxImageShrinkPctBothSides;
        const int min_extent = full - full * max_shrink_pct / 100;
        const int extent = area - columns * col;
        if (extent < min_extent || extent >= full)
            continue;
        CalloutRect shrunk;
        shrunk.w = horiz ? extent : int(int64_t(fitted.w) * extent / full);
        shrunk.h = horiz ? int(int64_t(fitted.h) * extent / full) : extent;
        shrunk.x = (in.area_w - shrunk.w) / 2;
        shrunk.y = (in.area_h - shrunk.h) / 2;
        if (try_line_modes_at(lined, shrunk, loose_budget, loose_active, out))
            return true;
    }
    return false;
}

} // namespace callout_detail

[[nodiscard]] inline CalloutLayout compute_callout_layout(const CalloutLayoutInput& in) {
    using namespace callout_detail;
    CalloutLayout out;
    out.image = fit_image(in.area_w, in.area_h, in.image_w, in.image_h);
    if (in.single_cell || out.image.w <= 0 || out.image.h < 3 * in.chip_h) {
        out.mode = CalloutMode::ImageOnly;
        return out;
    }

    std::vector<CalloutChipIn> chips = in.active;
    if (in.tagged) {
        // A chip with a line keeps its label: two unlabeled fan chips read the
        // same. Expendable chips go instead, before the image gives up its
        // lines, the most expendable first, one more at a time. Only then may
        // the image shrink further for a single column: a smaller picture with
        // lines reads better than chips laid over it.
        const auto has_label = [](const CalloutChipIn& c) { return c.w_unlabeled > 0; };
        const bool can_unlabel = std::any_of(in.budget.begin(), in.budget.end(), has_label) ||
                                 std::any_of(in.active.begin(), in.active.end(), has_label);
        const CalloutLayoutInput& base = in;
        std::vector<CalloutKind> by_rank;
        for (const auto& c : base.budget)
            if (c.expendable)
                by_rank.push_back(c.kind);
        sort_small(by_rank.begin(), by_rank.end(),
                   [](CalloutKind a, CalloutKind b) { return dock_rank(a) < dock_rank(b); });
        for (const int one_side_pct : {kMaxImageShrinkPctBothSides, kMaxImageShrinkPctOneSide}) {
            if (try_line_modes(in, out, one_side_pct))
                return out;
            for (size_t n = 1; n <= by_rank.size(); ++n) {
                const auto gone = [&](const CalloutChipIn& c) {
                    return c.expendable && std::find(by_rank.begin(), by_rank.begin() + long(n),
                                                     c.kind) != by_rank.begin() + long(n);
                };
                CalloutLayoutInput lean = base;
                for (auto* v : {&lean.budget, &lean.active})
                    v->erase(std::remove_if(v->begin(), v->end(), gone), v->end());
                if (try_line_modes(lean, out, one_side_pct)) {
                    for (const auto& c : in.active)
                        if (gone(c))
                            out.dropped.push_back(c.kind);
                    return out;
                }
            }
        }
        // Pinned chips sit on their parts, so their place names them: no labels.
        out.unlabeled = can_unlabel;
        chips = base.active;
        for (auto& c : chips)
            if (has_label(c))
                c.w = c.w_unlabeled;
        const auto has = [&](CalloutKind k) {
            return std::any_of(chips.begin(), chips.end(),
                               [k](const CalloutChipIn& c) { return c.kind == k; });
        };
        if (in.toolhead && has(CalloutKind::Nozzle) && has(CalloutKind::Fan)) {
            chips.erase(std::remove_if(chips.begin(), chips.end(),
                                       [](const CalloutChipIn& c) {
                                           return c.kind == CalloutKind::Nozzle ||
                                                  c.kind == CalloutKind::Fan;
                                       }),
                        chips.end());
            chips.push_back(*in.toolhead);
            out.toolhead_merged = true;
        }
        out.mode = CalloutMode::Pinned;
        std::vector<CalloutChipIn> pinned, docked;
        for (const auto& c : chips)
            (c.anchor ? pinned : docked).push_back(c);
        // A docked row longer than the area is wide would climb over the
        // picture, so its expected values give way first, least needed first.
        for (;;) {
            int row = docked.empty() ? 0 : in.gap * int(docked.size() - 1);
            for (const auto& c : docked)
                row += c.w;
            if (row <= in.area_w - 2 * in.gap)
                break;
            auto victim = docked.end();
            for (auto it = docked.begin(); it != docked.end(); ++it)
                if (it->expendable &&
                    (victim == docked.end() || dock_rank(it->kind) < dock_rank(victim->kind)))
                    victim = it;
            if (victim == docked.end())
                break;
            out.dropped.push_back(victim->kind);
            docked.erase(victim);
        }
        // Each pass places the pinned chips on their points and the rest in the
        // docked row, until every column of pinned chips stacks above that row.
        for (;;) {
            out.chips.clear();
            for (const auto& c : pinned) {
                const int cx = px(c.anchor->x, out.image.x, out.image.w);
                const int cy = px(c.anchor->y, out.image.y, out.image.h);
                const int h = chip_h_of(in, c);
                out.chips.push_back(
                    {c.kind, clamp_into({cx - c.w / 2, cy - h / 2, c.w, h}, in.area_w, in.area_h)});
            }
            // The docked row is the bottom edge: the side bands belong to the image here.
            place_docked(in, CalloutRect{0, 0, in.area_w, 0}, docked, out.chips);
            const auto victim = slide_apart(in, out.chips, pinned);
            if (!victim) {
                for (size_t i = 0; i < pinned.size(); ++i)
                    add_pinned_leader(in, out.chips[i],
                                      {px(pinned[i].anchor->x, out.image.x, out.image.w),
                                       px(pinned[i].anchor->y, out.image.y, out.image.h)});
                return out;
            }
            if (pinned[*victim].expendable)
                out.dropped.push_back(pinned[*victim].kind);
            else
                docked.push_back(pinned[*victim]);
            pinned.erase(pinned.begin() + long(*victim));
        }
    }

    out.mode = CalloutMode::Docked;
    place_docked(in, out.image, chips, out.chips);
    return out;
}

/// Chip widths for review_callout_layout(), indexed by CalloutKind.
using CalloutChipWidths = std::array<int, kCalloutKindCount>;

/// The tagger's review: a chip for every tagged part of `r`, placed over an
/// `area_w` x `area_h` view of the image the way the home widget pins them
/// (sliding apart, the toolhead merge, the overflow dock).
[[nodiscard]] inline CalloutLayout review_callout_layout(const ImageRegions& r, int area_w,
                                                         int area_h, const CalloutChipWidths& w,
                                                         int chip_h, int gap) {
    CalloutLayoutInput in;
    in.area_w = area_w;
    in.area_h = area_h;
    in.image_w = r.src_w;
    in.image_h = r.src_h;
    in.tagged = true;
    in.chip_h = chip_h;
    in.gap = gap;
    for (const CalloutKind k : {CalloutKind::Nozzle, CalloutKind::Bed, CalloutKind::Chamber,
                                CalloutKind::Fan, CalloutKind::Light}) {
        if (const auto a = region_anchor(r, k))
            in.active.push_back({k, w[size_t(k)], a});
    }
    const auto toolhead = size_t(CalloutKind::Toolhead);
    in.toolhead = CalloutChipIn{CalloutKind::Toolhead, w[toolhead], r.nozzle};
    return compute_callout_layout(in);
}

} // namespace helix
