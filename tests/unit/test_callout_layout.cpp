// SPDX-License-Identifier: GPL-3.0-or-later
#include "callout_layout.h"

#include <fstream>
#include <iterator>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// 800x480 2x2 widget: 160x160 container, K1C aspect (1601x1204).
CalloutLayoutInput base() {
    CalloutLayoutInput in;
    in.area_w = 160;
    in.area_h = 140;
    in.image_w = 1601;
    in.image_h = 1204;
    in.tagged = true;
    in.chip_h = 18;
    in.gap = 4;
    in.min_line = 8;
    return in;
}

// TEST_MIRROR_OK: find() looks a chip up in a CalloutLayout; it shares only its name with
// platform_table::find()
const CalloutChipOut* find(const CalloutLayout& l, CalloutKind k) {
    for (const auto& c : l.chips)
        if (c.kind == k)
            return &c;
    return nullptr;
}

bool overlaps(const CalloutRect& a, const CalloutRect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

bool inside(const CalloutRect& r, int w, int h) {
    return r.x >= 0 && r.y >= 0 && r.x + r.w <= w && r.y + r.h <= h;
}

} // namespace

TEST_CASE("fit_image: contain-fit, centred", "[printer_image][callout_layout]") {
    const auto r = fit_image(200, 100, 400, 400);
    CHECK(r.w == 100);
    CHECK(r.h == 100);
    CHECK(r.x == 50);
    CHECK(r.y == 0);
    CHECK(fit_image(0, 100, 400, 400).w == 0);
    CHECK(fit_image(200, 100, 0, 0).w == 0);
}

TEST_CASE("spread_1d: resolves overlaps and stays within bounds",
          "[printer_image][callout_layout]") {
    std::vector<int> start{10, 12, 90};
    const std::vector<int> size{18, 18, 18};
    spread_1d(start, size, 0, 100, 4);
    CHECK(start[0] == 10);
    CHECK(start[1] == 32);
    CHECK(start[2] == 82); // pulled back up to fit the bottom edge
}

TEST_CASE("single cell shows the image only", "[printer_image][callout_layout]") {
    auto in = base();
    in.single_cell = true;
    in.active = {{CalloutKind::Nozzle, 60, NormPoint{0.5f, 0.3f}}};
    const auto l = compute_callout_layout(in);
    CHECK(l.mode == CalloutMode::ImageOnly);
    CHECK(l.chips.empty());
}

TEST_CASE("image shorter than three chips shows the image only (2x1 cells)",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.area_w = 160;
    in.area_h = 3 * 18 - 1; // fitted K1C image is 53px tall
    in.active = {{CalloutKind::Nozzle, 60, NormPoint{0.5f, 0.3f}}};
    CHECK(compute_callout_layout(in).mode == CalloutMode::ImageOnly);
    in.area_h = 3 * 18;
    CHECK(compute_callout_layout(in).mode != CalloutMode::ImageOnly);
}

TEST_CASE("unlaid-out container shows the image only", "[printer_image][callout_layout]") {
    auto in = base();
    in.area_w = 0;
    in.area_h = 0;
    in.active = {{CalloutKind::Nozzle, 60, NormPoint{0.5f, 0.3f}}};
    CHECK(compute_callout_layout(in).mode == CalloutMode::ImageOnly);
}

TEST_CASE("pinned: chip centred on its point, inside the area", "[printer_image][callout_layout]") {
    auto in = base();
    in.active = {{CalloutKind::Bed, 60, NormPoint{0.46f, 0.57f}}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Pinned);
    const auto* bed = find(l, CalloutKind::Bed);
    REQUIRE(bed);
    const int ax = l.image.x + int(0.46f * l.image.w);
    const int ay = l.image.y + int(0.57f * l.image.h);
    CHECK(bed->rect.x + bed->rect.w / 2 == Catch::Approx(ax).margin(1));
    CHECK(bed->rect.y + bed->rect.h / 2 == Catch::Approx(ay).margin(1));
    CHECK_FALSE(bed->has_line);
    CHECK(inside(bed->rect, in.area_w, in.area_h));
}

TEST_CASE("pinned: a point at the image edge is clamped inside",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.active = {{CalloutKind::Light, 40, NormPoint{0.99f, 0.01f}}};
    const auto l = compute_callout_layout(in);
    CHECK(inside(find(l, CalloutKind::Light)->rect, in.area_w, in.area_h));
}

TEST_CASE("pinned: nozzle and fan merge into the toolhead chip",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.active = {{CalloutKind::Nozzle, 70, NormPoint{0.51f, 0.28f}},
                 {CalloutKind::Fan, 40, NormPoint{0.49f, 0.21f}}};
    in.toolhead = CalloutChipIn{CalloutKind::Toolhead, 110, NormPoint{0.51f, 0.28f}};
    const auto l = compute_callout_layout(in);
    CHECK(l.toolhead_merged);
    CHECK(find(l, CalloutKind::Toolhead));
    CHECK_FALSE(find(l, CalloutKind::Nozzle));
    CHECK_FALSE(find(l, CalloutKind::Fan));
}

TEST_CASE("pinned: nozzle alone does not merge", "[printer_image][callout_layout]") {
    auto in = base();
    in.active = {{CalloutKind::Nozzle, 70, NormPoint{0.51f, 0.28f}}};
    in.toolhead = CalloutChipIn{CalloutKind::Toolhead, 110, NormPoint{0.51f, 0.28f}};
    const auto l = compute_callout_layout(in);
    CHECK_FALSE(l.toolhead_merged);
    CHECK(find(l, CalloutKind::Nozzle));
}

TEST_CASE("tagged image, chip with no point is docked, not at the origin",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.active = {{CalloutKind::Nozzle, 60, NormPoint{0.5f, 0.3f}},
                 {CalloutKind::Chamber, 50, std::nullopt}};
    const auto l = compute_callout_layout(in);
    const auto* ch = find(l, CalloutKind::Chamber);
    REQUIRE(ch);
    CHECK(inside(ch->rect, in.area_w, in.area_h));
    CHECK(ch->rect.y + ch->rect.h == in.area_h - in.gap); // bottom dock row
}

TEST_CASE("untagged image: chips docked along the bottom, no lines, no overlap",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.tagged = false;
    in.active = {{CalloutKind::Nozzle, 60, std::nullopt},
                 {CalloutKind::Bed, 60, std::nullopt},
                 {CalloutKind::Fan, 40, std::nullopt}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Docked);
    REQUIRE(l.chips.size() == 3);
    for (size_t i = 0; i < l.chips.size(); ++i) {
        CHECK_FALSE(l.chips[i].has_line);
        CHECK(inside(l.chips[i].rect, in.area_w, in.area_h));
        for (size_t j = i + 1; j < l.chips.size(); ++j) {
            const auto& a = l.chips[i].rect;
            const auto& b = l.chips[j].rect;
            const bool overlap =
                a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
            CHECK_FALSE(overlap);
        }
    }
}

TEST_CASE("untagged image with a side band: chips stack in the band",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.tagged = false;
    in.area_w = 330;
    in.area_h = 140; // image 186 wide, 72px band each side, chip column needs 68
    in.active = {{CalloutKind::Nozzle, 60, std::nullopt}, {CalloutKind::Bed, 60, std::nullopt}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Docked);
    for (const auto& c : l.chips)
        CHECK(c.rect.x >= l.image.x + l.image.w);
}

TEST_CASE("docked: a chip wider than the area is clamped to fit, not left overhanging",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.tagged = false;
    in.active = {{CalloutKind::Nozzle, 200, std::nullopt}}; // wider than area_w (160)
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Docked);
    REQUIRE(l.chips.size() == 1);
    CHECK(inside(l.chips[0].rect, in.area_w, in.area_h));
    CHECK(l.chips[0].rect.w <= in.area_w);
}

TEST_CASE("pinned: a merged toolhead chip wider than the area is clamped inside",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.area_w = 136; // narrower than the merged toolhead chip (150)
    in.active = {{CalloutKind::Nozzle, 70, NormPoint{0.51f, 0.28f}},
                 {CalloutKind::Fan, 40, NormPoint{0.49f, 0.21f}}};
    in.toolhead = CalloutChipIn{CalloutKind::Toolhead, 150, NormPoint{0.51f, 0.28f}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.toolhead_merged);
    const auto* th = find(l, CalloutKind::Toolhead);
    REQUIRE(th);
    CHECK(inside(th->rect, in.area_w, in.area_h));
}

namespace {
// K1C in a 4x2 widget at 800x480: 320x140 container, fitted image 186 wide.
CalloutLayoutInput wide() {
    auto in = base();
    in.area_w = 320;
    in.area_h = 140;
    in.budget = {{CalloutKind::Nozzle, 70, NormPoint{0.51f, 0.28f}},
                 {CalloutKind::Bed, 70, NormPoint{0.46f, 0.57f}},
                 {CalloutKind::Fan, 45, NormPoint{0.49f, 0.21f}},
                 {CalloutKind::Chamber, 70, NormPoint{0.31f, 0.38f}}};
    in.active = in.budget;
    return in;
}
} // namespace

TEST_CASE("both sides when each band fits a column", "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 186 + 2 * (70 + in.gap + in.min_line); // exactly one column per side
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::BothSides);
    CHECK(l.image.x == (in.area_w - l.image.w) / 2); // image stays centred
    for (const auto& c : l.chips) {
        CHECK(c.has_line);
        const bool left = c.rect.x + c.rect.w <= l.image.x;
        const bool right = c.rect.x >= l.image.x + l.image.w;
        CHECK((left || right));
    }
    // Chamber's point is left of centre, so its chip is on the left.
    for (const auto& c : l.chips)
        if (c.kind == CalloutKind::Chamber)
            CHECK(c.rect.x < l.image.x);
}

TEST_CASE("one pixel short of both sides falls to one side", "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 186 + 2 * (70 + in.gap + in.min_line) - 2;
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::OneSide);
    CHECK(l.image.x == 0); // image moved left
    for (const auto& c : l.chips) {
        CHECK(c.has_line);
        CHECK(c.rect.x >= l.image.w);
    }
}

TEST_CASE("one side keeps the image at its full fitted size when it fits",
          "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 186 + (70 + in.gap + in.min_line); // exactly one column beside it
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::OneSide);
    CHECK(l.image.w == 186);
    CHECK(l.image.h == 140);
}

TEST_CASE("a small image shrink unlocks one side instead of pinned",
          "[printer_image][callout_layout]") {
    auto in = wide();
    const int col = 70 + in.gap + in.min_line;
    in.area_w = 186 + col - 14; // the column fits once the image gives up 14px: 7.5%
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::OneSide);
    CHECK(l.image.x == 0);
    CHECK(l.image.w == 186 - 14);
    CHECK(l.image.h == 140 * (186 - 14) / 186); // aspect kept
    CHECK(l.image.y == (in.area_h - l.image.h) / 2);
    for (const auto& c : l.chips) {
        CHECK(c.has_line);
        CHECK(c.rect.x >= l.image.x + l.image.w);
        CHECK(c.rect.x + c.rect.w <= in.area_w);
    }
    // Fit is decided by the budget: a chip going away does not grow the image back.
    in.active = {{CalloutKind::Bed, 70, NormPoint{0.46f, 0.57f}}};
    const auto one = compute_callout_layout(in);
    CHECK(one.mode == CalloutMode::OneSide);
    CHECK(one.image.w == l.image.w);
    CHECK(one.image.x == l.image.x);
}

TEST_CASE("the shrink makes room for a pointless chip wider than every lined one",
          "[printer_image][callout_layout]") {
    auto in = wide();
    // The widest chip has no point to draw to; it still rides in the side band.
    in.budget.push_back({CalloutKind::Fan, 100, std::nullopt});
    in.active = in.budget;
    const int col = 100 + in.gap + in.min_line;
    in.area_w = 186 + col - 14; // the column fits once the image gives up 14px
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::OneSide);
    CHECK(l.image.w == 186 - 14);
}

TEST_CASE("one side may take more of the image than both sides",
          "[printer_image][callout_layout]") {
    auto in = wide();
    const int col = 70 + in.gap + in.min_line;
    in.area_w = 186 + col - 65; // the column fits once the image gives up 65px: 35%
    REQUIRE(65 * 100 / 186 > helix::callout_detail::kMaxImageShrinkPctBothSides);
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::OneSide);
    CHECK(l.image.w == 186 - 65);
}

TEST_CASE("a column needing more than the shrink limit staggers at full size",
          "[printer_image][callout_layout]") {
    auto in = wide();
    // One pixel more than the image may give up.
    in.area_w = 186 + (70 + in.gap + in.min_line) -
                (186 * helix::callout_detail::kMaxImageShrinkPctOneSide / 100 + 1);
    const auto l = compute_callout_layout(in);
    CHECK(l.mode == CalloutMode::Staggered);
    CHECK(l.image.w == 186);
}

TEST_CASE("staggered chips stand compact at the area edges beside the full-size picture",
          "[printer_image][callout_layout]") {
    auto in = wide();
    // Past the shrink limit for one 70px column: no line mode fits, nothing shrinks.
    in.area_w = 186 + (70 + in.gap + in.min_line) -
                (186 * helix::callout_detail::kMaxImageShrinkPctOneSide / 100 + 1);
    // The chamber's chip reaches past its point (x 60: the 186px picture at x 3).
    for (auto* v : {&in.budget, &in.active})
        for (auto& c : *v)
            c.w_compact = c.kind == CalloutKind::Chamber ? 80 : 40;
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Staggered);
    CHECK(l.image.w == fit_image(in.area_w, in.area_h, in.image_w, in.image_h).w);
    REQUIRE(l.chips.size() == in.active.size());
    for (const auto& c : l.chips) {
        CHECK(c.rect.w == (c.kind == CalloutKind::Chamber ? 80 : 40));
        CHECK((c.rect.x == in.gap || c.rect.x + c.rect.w == in.area_w - in.gap));
        // A leader runs only from a point the chip's inner edge clears.
        if (c.has_line)
            CHECK((c.line_x0 >= c.rect.x + c.rect.w + in.min_line ||
                   c.line_x0 <= c.rect.x - in.min_line));
    }
    // Over its own point, the chamber chip needs no leader.
    const auto* chamber = find(l, CalloutKind::Chamber);
    REQUIRE(chamber);
    CHECK(chamber->rect.x == in.gap);
    CHECK_FALSE(chamber->has_line);
    const auto* bed = find(l, CalloutKind::Bed);
    REQUIRE(bed);
    CHECK(bed->has_line);

    // A chip wider than the area has no edge to stand against: it pins.
    for (auto* v : {&in.budget, &in.active})
        v->front().w_compact = in.area_w;
    CHECK(compute_callout_layout(in).mode == CalloutMode::Pinned);
}

TEST_CASE("a small image shrink unlocks both sides when one side cannot stack",
          "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_h = 80; // three chips stack, four do not; fitted image 106x80
    const int col = 70 + in.gap + in.min_line;
    in.area_w = 106 + 2 * col - 7;
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::BothSides);
    CHECK(l.image.w == 106 - 7);
    CHECK(l.image.x == (in.area_w - l.image.w) / 2);
    for (const auto& c : l.chips) {
        const bool left = c.rect.x + c.rect.w <= l.image.x;
        const bool right = c.rect.x >= l.image.x + l.image.w;
        CHECK((left || right));
    }
}

TEST_CASE("column that cannot stack its chips falls through to pinned",
          "[printer_image][callout_layout]") {
    auto in = wide();
    for (auto& c : in.budget)
        c.anchor->x = 0.3f; // every point left of centre: one side must hold all four
    in.active = in.budget;
    in.area_w = 400;
    in.area_h = 4 * in.chip_h + 3 * in.gap + 2 * in.gap - 1; // one pixel short for four stacked
    CHECK(compute_callout_layout(in).mode == CalloutMode::Pinned);
    in.area_h += 1;
    CHECK(compute_callout_layout(in).mode != CalloutMode::Pinned);
}

TEST_CASE("mode comes from the budget, so the image does not move when chips change",
          "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 186 + 2 * (70 + in.gap + in.min_line) - 2;
    const auto full = compute_callout_layout(in);
    in.active = {{CalloutKind::Bed, 70, NormPoint{0.46f, 0.57f}}};
    const auto one = compute_callout_layout(in);
    CHECK(one.mode == full.mode);
    CHECK(one.image.x == full.image.x);
    in.active.clear();
    CHECK(compute_callout_layout(in).image.x == full.image.x);
}

TEST_CASE("an empty budget never draws lines", "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 186 + 2 * (70 + in.gap + in.min_line); // room for both sides
    REQUIRE(compute_callout_layout(in).mode == CalloutMode::BothSides);
    in.budget.clear();
    const auto l = compute_callout_layout(in);
    CHECK(l.mode == CalloutMode::Pinned);
    for (const auto& c : l.chips)
        CHECK_FALSE(c.has_line);
}

TEST_CASE("leader line runs from the tagged point to the chip's inner edge",
          "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 186 + 2 * (70 + in.gap + in.min_line);
    in.active = {{CalloutKind::Bed, 70, NormPoint{0.46f, 0.57f}}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.chips.size() == 1);
    const auto& c = l.chips[0];
    CHECK(c.line_x0 == l.image.x + int(0.46f * l.image.w));
    CHECK(c.line_y0 == l.image.y + int(0.57f * l.image.h));
    const bool at_inner_edge = c.line_x1 == c.rect.x || c.line_x1 == c.rect.x + c.rect.w;
    CHECK(at_inner_edge);
    CHECK(c.line_y1 == c.rect.y + c.rect.h / 2);
}

TEST_CASE("tall widget uses bands above and below", "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 240;
    in.area_h = 480; // image 240x180 centred: 150px above and below
    const auto l = compute_callout_layout(in);
    REQUIRE((l.mode == CalloutMode::BothSides || l.mode == CalloutMode::OneSide));
    for (const auto& c : l.chips) {
        const bool above = c.rect.y + c.rect.h <= l.image.y;
        const bool below = c.rect.y >= l.image.y + l.image.h;
        CHECK((above || below));
    }
}

TEST_CASE("a budget chip with no tagged point docks beside the lined chips, not at the origin",
          "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 186 + 2 * (70 + in.gap + in.min_line); // otherwise fits both sides
    for (auto* v : {&in.budget, &in.active})
        for (auto& c : *v)
            if (c.kind == CalloutKind::Chamber)
                c.anchor = std::nullopt;
    const auto l = compute_callout_layout(in);
    REQUIRE((l.mode == CalloutMode::BothSides || l.mode == CalloutMode::OneSide));
    const auto* chamber = find(l, CalloutKind::Chamber);
    REQUIRE(chamber);
    CHECK_FALSE(chamber->has_line);
    CHECK(inside(chamber->rect, in.area_w, in.area_h));
    CHECK_FALSE(overlaps(chamber->rect, {l.image.x, l.image.y, l.image.w, l.image.h}));
    for (const auto& c : l.chips)
        if (&c != chamber)
            CHECK(c.has_line);
}

// ---------------------------------------------------------------------------
// Elbow leader lines: 45 degrees toward the chip, then straight into its edge
// ---------------------------------------------------------------------------

using callout_detail::leader_elbow;

TEST_CASE("leader elbow: room for 45 degrees runs 45 degrees, then level into the chip",
          "[printer_image][callout_layout]") {
    const CalloutPoint a{100, 50}, c{200, 80};
    const auto e = leader_elbow(a, c, 8, true);
    CHECK(std::abs(e.x - a.x) == std::abs(e.y - a.y));
    CHECK(e.y == c.y);
    CHECK(e.x == 130);
    // Upward too.
    const auto up = leader_elbow(a, CalloutPoint{200, 20}, 8, true);
    CHECK(up.x == 130);
    CHECK(up.y == 20);
}

TEST_CASE("leader elbow: too steep for 45 degrees keeps a min_line level stub",
          "[printer_image][callout_layout]") {
    const CalloutPoint a{100, 50}, c{120, 100};
    const auto e = leader_elbow(a, c, 8, true);
    CHECK(e.x == c.x - 8);
    CHECK(e.y == c.y);
    // Exactly one pixel short of the 45-degree run plus its stub.
    CHECK(leader_elbow(a, CalloutPoint{120, 63}, 8, true).x == 112);
    CHECK(leader_elbow(a, CalloutPoint{120, 62}, 8, true).x == 112);
    CHECK(leader_elbow(a, CalloutPoint{120, 61}, 8, true).x == 111);
}

TEST_CASE("leader elbow: a chip nearer than min_line never runs the line backwards",
          "[printer_image][callout_layout]") {
    const CalloutPoint a{100, 50}, c{105, 90};
    const auto e = leader_elbow(a, c, 8, true);
    CHECK(e.x == a.x);
    CHECK(e.y == c.y);
}

TEST_CASE("leader elbow: a level chip puts the elbow on the straight line",
          "[printer_image][callout_layout]") {
    const CalloutPoint a{100, 50}, c{200, 50};
    const auto e = leader_elbow(a, c, 8, true);
    CHECK(e.y == 50);
    CHECK(e.x >= a.x);
    CHECK(e.x <= c.x);
}

TEST_CASE("leader elbow: a left-hand chip mirrors the run", "[printer_image][callout_layout]") {
    const CalloutPoint a{100, 50};
    const auto e = leader_elbow(a, CalloutPoint{0, 80}, 8, true);
    CHECK(e.x == 70);
    CHECK(e.y == 80);
    const auto steep = leader_elbow(a, CalloutPoint{80, 100}, 8, true);
    CHECK(steep.x == 80 + 8);
    CHECK(steep.y == 100);
}

TEST_CASE("leader elbow: bands above and below run 45 degrees, then vertical into the chip",
          "[printer_image][callout_layout]") {
    const CalloutPoint a{100, 50};
    const auto e = leader_elbow(a, CalloutPoint{130, 200}, 8, false);
    CHECK(std::abs(e.x - a.x) == std::abs(e.y - a.y));
    CHECK(e.x == 130);
    CHECK(e.y == 80);
    const auto steep = leader_elbow(a, CalloutPoint{160, 70}, 8, false);
    CHECK(steep.x == 160);
    CHECK(steep.y == 70 - 8);
    const auto above = leader_elbow(a, CalloutPoint{70, 0}, 8, false);
    CHECK(above.x == 70);
    CHECK(above.y == 20);
}

TEST_CASE("leader lines from the layout end level into side chips, vertical into top chips",
          "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 186 + 2 * (70 + in.gap + in.min_line);
    auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::BothSides);
    for (const auto& c : l.chips) {
        INFO(int(c.kind));
        const auto e =
            leader_elbow({c.line_x0, c.line_y0}, {c.line_x1, c.line_y1}, in.min_line, true);
        CHECK(c.line_xm == e.x);
        CHECK(c.line_ym == e.y);
        CHECK(c.line_ym == c.line_y1);
    }
    in.area_w = 240;
    in.area_h = 480;
    l = compute_callout_layout(in);
    REQUIRE((l.mode == CalloutMode::BothSides || l.mode == CalloutMode::OneSide));
    for (const auto& c : l.chips) {
        INFO(int(c.kind));
        const auto e =
            leader_elbow({c.line_x0, c.line_y0}, {c.line_x1, c.line_y1}, in.min_line, false);
        CHECK(c.line_xm == e.x);
        CHECK(c.line_ym == e.y);
        CHECK(c.line_xm == c.line_x1);
    }
}

// ---------------------------------------------------------------------------
// Pinned chips slide apart instead of landing on each other
// ---------------------------------------------------------------------------

namespace {

void check_apart_and_inside(const CalloutLayout& l, const CalloutLayoutInput& in) {
    for (size_t i = 0; i < l.chips.size(); ++i) {
        INFO("chip " << int(l.chips[i].kind));
        CHECK(inside(l.chips[i].rect, in.area_w, in.area_h));
        for (size_t j = i + 1; j < l.chips.size(); ++j) {
            INFO("vs chip " << int(l.chips[j].kind));
            CHECK_FALSE(overlaps(l.chips[i].rect, l.chips[j].rect));
        }
    }
}

// A K1C 2x2 tile: the merged toolhead chip on the nozzle, the light chip on
// the light, which sits just above and left of it.
CalloutLayoutInput k1c_2x2() {
    auto in = base();
    in.area_w = 240;
    in.area_h = 200;
    in.chip_h = 30;
    in.active = {{CalloutKind::Nozzle, 100, NormPoint{0.513f, 0.279f}},
                 {CalloutKind::Fan, 50, NormPoint{0.488f, 0.206f}},
                 {CalloutKind::Light, 40, NormPoint{0.321f, 0.164f}}};
    in.toolhead = CalloutChipIn{CalloutKind::Toolhead, 184, NormPoint{0.513f, 0.279f}};
    return in;
}

} // namespace

TEST_CASE("pinned: the toolhead chip and the light chip slide apart (K1C 2x2)",
          "[printer_image][callout_layout]") {
    const auto in = k1c_2x2();
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Pinned);
    REQUIRE(l.toolhead_merged);
    const auto* th = find(l, CalloutKind::Toolhead);
    const auto* light = find(l, CalloutKind::Light);
    REQUIRE(th);
    REQUIRE(light);
    // Placed on their points, the two would collide.
    const int nx = l.image.x + int(0.513f * l.image.w), ny = l.image.y + int(0.279f * l.image.h);
    const int lx = l.image.x + int(0.321f * l.image.w), ly = l.image.y + int(0.164f * l.image.h);
    REQUIRE(overlaps({nx - 92, ny - 15, 184, 30}, {lx - 20, ly - 15, 40, 30}));
    check_apart_and_inside(l, in);
    // Only vertical moves: each chip stays centred on its point's x.
    CHECK(th->rect.x == nx - 92);
    CHECK(light->rect.x == lx - 20);
}

TEST_CASE("pinned: chamber and nozzle chips slide apart (Q2-like)",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.area_w = 240;
    in.area_h = 200;
    in.image_w = 1000;
    in.image_h = 1000;
    in.chip_h = 30;
    in.active = {{CalloutKind::Chamber, 70, NormPoint{0.368f, 0.531f}},
                 {CalloutKind::Nozzle, 70, NormPoint{0.574f, 0.398f}}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Pinned);
    REQUIRE(l.chips.size() == 2);
    // Placed on their points, the two would collide.
    const auto on_point = [&](const CalloutChipIn& c) {
        const int x = l.image.x + int(c.anchor->x * l.image.w);
        const int y = l.image.y + int(c.anchor->y * l.image.h);
        return CalloutRect{x - c.w / 2, y - in.chip_h / 2, c.w, in.chip_h};
    };
    REQUIRE(overlaps(on_point(in.active[0]), on_point(in.active[1])));
    check_apart_and_inside(l, in);
}

TEST_CASE("pinned: a column too tall for a short tile docks its lowest-priority chips",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.area_w = 240;
    in.area_h = 100; // four stacked chips need 132
    in.chip_h = 30;
    in.active = {{CalloutKind::Nozzle, 150, NormPoint{0.513f, 0.279f}},
                 {CalloutKind::Bed, 100, NormPoint{0.46f, 0.57f}},
                 {CalloutKind::Chamber, 90, NormPoint{0.31f, 0.38f}},
                 {CalloutKind::Light, 28, NormPoint{0.32f, 0.16f}}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Pinned);
    REQUIRE(l.chips.size() == 4);
    check_apart_and_inside(l, in);
    // The nozzle keeps its point; the light gives way first, to the bottom row.
    const auto* nozzle = find(l, CalloutKind::Nozzle);
    REQUIRE(nozzle);
    CHECK(nozzle->rect.x == l.image.x + int(0.513f * l.image.w) - 75);
    const auto* light = find(l, CalloutKind::Light);
    REQUIRE(light);
    CHECK(light->rect.y + light->rect.h == in.area_h - in.gap);
}

TEST_CASE("pinned: chips that do not collide stay centred on their points",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.area_w = 240;
    in.area_h = 200;
    in.chip_h = 30;
    // Same column, far apart vertically; and one level with the nozzle, off to the side.
    in.active = {{CalloutKind::Nozzle, 60, NormPoint{0.5f, 0.25f}},
                 {CalloutKind::Bed, 60, NormPoint{0.5f, 0.75f}},
                 {CalloutKind::Light, 30, NormPoint{0.1f, 0.25f}}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Pinned);
    for (const auto& a : in.active) {
        const auto* c = find(l, a.kind);
        REQUIRE(c);
        const int px = l.image.x + int(a.anchor->x * l.image.w);
        const int py = l.image.y + int(a.anchor->y * l.image.h);
        CHECK(c->rect.x == px - a.w / 2);
        CHECK(c->rect.y == py - in.chip_h / 2);
    }
}

TEST_CASE("pinned: a chip on a low point stays clear of the docked row",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.active = {{CalloutKind::Bed, 60, NormPoint{0.5f, 0.9f}},
                 {CalloutKind::Chamber, 50, std::nullopt}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Pinned);
    const auto* chamber = find(l, CalloutKind::Chamber);
    REQUIRE(chamber);
    REQUIRE(chamber->rect.y + chamber->rect.h == in.area_h - in.gap); // docked row
    // On its point the bed chip would sit on the docked chamber chip.
    const int bx = l.image.x + int(0.5f * l.image.w), by = l.image.y + int(0.9f * l.image.h);
    REQUIRE(overlaps({bx - 30, by - in.chip_h / 2, 60, in.chip_h}, chamber->rect));
    check_apart_and_inside(l, in);
}

namespace {
// The Trident's tagged points, 645x908.
std::optional<NormPoint> tri(float x, float y) {
    return NormPoint{x, y};
}
} // namespace

TEST_CASE("untagged image docks every chip", "[printer_image][callout_layout]") {
    auto in = base();
    in.tagged = false;
    in.area_w = 240;
    in.area_h = 200;
    in.active = {{CalloutKind::Nozzle, 60, tri(0.5f, 0.3f)},
                 {CalloutKind::Bed, 60, tri(0.5f, 0.6f)},
                 {CalloutKind::Chamber, 60, tri(0.4f, 0.5f)},
                 {CalloutKind::Light, 40, tri(0.8f, 0.1f)},
                 {CalloutKind::Fan, 40, tri(0.4f, 0.3f)}};
    const auto l = compute_callout_layout(in);
    CHECK(l.mode == CalloutMode::Docked);
    REQUIRE(l.chips.size() == 5);
    for (const auto& c : l.chips)
        CHECK_FALSE(c.has_line);
}

TEST_CASE("missing anchor docks only that chip", "[printer_image][callout_layout]") {
    CalloutLayoutInput in;
    in.area_w = 342;
    in.area_h = 270;
    in.image_w = 645;
    in.image_h = 908;
    in.tagged = true;
    in.chip_h = 22;
    in.gap = 4;
    in.min_line = 8;
    in.active = {{CalloutKind::Nozzle, 70, tri(0.441f, 0.36f)},
                 {CalloutKind::Bed, 70, tri(0.346f, 0.382f)},
                 {CalloutKind::Light, 50, std::nullopt},
                 {CalloutKind::SpeedFlow, 60, std::nullopt}};
    in.budget = in.active;
    const auto l = compute_callout_layout(in);
    REQUIRE((l.mode == CalloutMode::BothSides || l.mode == CalloutMode::OneSide));
    REQUIRE(l.chips.size() == 4);
    CHECK(find(l, CalloutKind::Nozzle)->has_line);
    CHECK(find(l, CalloutKind::Bed)->has_line);
    for (const auto k : {CalloutKind::Light, CalloutKind::SpeedFlow}) {
        const auto* c = find(l, k);
        REQUIRE(c);
        CHECK_FALSE(c->has_line);
        CHECK(inside(c->rect, in.area_w, in.area_h));
        CHECK_FALSE(overlaps(c->rect, {l.image.x, l.image.y, l.image.w, l.image.h}));
    }
    CHECK_FALSE(overlaps(find(l, CalloutKind::Light)->rect, find(l, CalloutKind::SpeedFlow)->rect));
}

TEST_CASE("expendable chips drop before the image goes pinned", "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 286; // room for one 70px column beside the image, not a 150px one
    // Two wide expendable fans widen the budget's column past what the band
    // holds even with the image shrunk.
    in.budget = {{CalloutKind::Nozzle, 70, NormPoint{0.51f, 0.28f}},
                 {CalloutKind::Bed, 70, NormPoint{0.46f, 0.57f}}};
    in.budget.push_back({CalloutKind::HotendFan, 150, NormPoint{0.6f, 0.30f}, true});
    in.budget.push_back({CalloutKind::AuxFan, 150, NormPoint{0.6f, 0.34f}, true});
    in.active = in.budget;
    // Same input without the expendable chips must line; with them it cannot.
    auto lean = in;
    for (auto* v : {&lean.budget, &lean.active})
        v->erase(std::remove_if(v->begin(), v->end(), [](auto& c) { return c.expendable; }),
                 v->end());
    REQUIRE(compute_callout_layout(lean).mode != CalloutMode::Pinned);
    const auto l = compute_callout_layout(in);
    CHECK(l.mode != CalloutMode::Pinned);
    CHECK(l.dropped.size() == 2);
    CHECK(std::count(l.dropped.begin(), l.dropped.end(), CalloutKind::HotendFan) == 1);
    CHECK(std::count(l.dropped.begin(), l.dropped.end(), CalloutKind::AuxFan) == 1);
    CHECK(find(l, CalloutKind::HotendFan) == nullptr);
}

TEST_CASE("pinned drops expendable before docking", "[printer_image][callout_layout]") {
    auto in = base();
    in.area_w = 150;
    in.area_h = 190;
    in.chip_h = 30; // seven stacked chips need 234, five need 166
    in.active = {{CalloutKind::Nozzle, 60, NormPoint{0.5f, 0.30f}},
                 {CalloutKind::Bed, 60, NormPoint{0.5f, 0.32f}},
                 {CalloutKind::Chamber, 60, NormPoint{0.5f, 0.34f}},
                 {CalloutKind::Light, 40, NormPoint{0.5f, 0.36f}},
                 {CalloutKind::Fan, 40, NormPoint{0.5f, 0.38f}},
                 {CalloutKind::HotendFan, 40, NormPoint{0.5f, 0.40f}, true},
                 {CalloutKind::AuxFan, 40, NormPoint{0.5f, 0.42f}, true}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Pinned);
    REQUIRE_FALSE(l.dropped.empty());
    for (const auto k : l.dropped)
        CHECK((k == CalloutKind::HotendFan || k == CalloutKind::AuxFan));
    // Both fans give way, so the five others stay pinned on their points' column.
    CHECK(l.dropped.size() == 2);
    CHECK(l.chips.size() == 5);
}

TEST_CASE("pinned chip draws a leader only past leader_min_distance",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.area_w = 240;
    in.area_h = 200;
    in.chip_h = 22;
    in.leader_min_distance = 12;
    in.active = {{CalloutKind::Nozzle, 60, NormPoint{0.5f, 0.5f}},
                 {CalloutKind::Bed, 60, NormPoint{0.5f, 0.5f}}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Pinned);
    int lined = 0, plain = 0;
    for (const auto& c : l.chips) {
        if (c.has_line) {
            ++lined;
            const int cx = c.rect.x + c.rect.w / 2, cy = c.rect.y + c.rect.h / 2;
            CHECK(((cx - c.line_x0) * (cx - c.line_x0) + (cy - c.line_y0) * (cy - c.line_y0)) >
                  12 * 12);
            CHECK(c.line_x0 == l.image.x + l.image.w / 2);
        } else {
            ++plain;
        }
    }
    CHECK(lined == 1);
    CHECK(plain == 1);

    // The slid chip is 26px from its point: a larger threshold leaves it line-free.
    in.leader_min_distance = 40;
    for (const auto& c : compute_callout_layout(in).chips)
        CHECK_FALSE(c.has_line);
}

TEST_CASE("leader_min_distance 0 keeps pinned line-free", "[printer_image][callout_layout]") {
    auto in = base();
    in.area_w = 240;
    in.area_h = 200;
    in.chip_h = 22;
    in.leader_min_distance = 0;
    in.active = {{CalloutKind::Nozzle, 60, NormPoint{0.5f, 0.5f}},
                 {CalloutKind::Bed, 60, NormPoint{0.5f, 0.5f}}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Pinned);
    for (const auto& c : l.chips)
        CHECK_FALSE(c.has_line);
}

TEST_CASE("pinned drops expendable docked chips when the docked row overflows",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.area_w = 150;
    in.area_h = 190;
    in.chip_h = 22;
    // Two anchorless chips: 70 + 4 + 80 = 154 overflow a 142px row, either alone fits.
    in.active = {{CalloutKind::Nozzle, 50, NormPoint{0.5f, 0.3f}},
                 {CalloutKind::Bed, 50, NormPoint{0.5f, 0.6f}},
                 {CalloutKind::SpeedFlow, 80, std::nullopt, false},
                 {CalloutKind::AuxFan, 70, std::nullopt, true}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Pinned);
    CHECK(l.dropped == std::vector<CalloutKind>{CalloutKind::AuxFan});
    CHECK(find(l, CalloutKind::AuxFan) == nullptr);
    CHECK(find(l, CalloutKind::SpeedFlow) != nullptr);

    // With room for the row, nothing expected is dropped.
    in.area_w = 240;
    const auto roomy = compute_callout_layout(in);
    REQUIRE(roomy.mode == CalloutMode::Pinned);
    CHECK(roomy.dropped.empty());
    CHECK(find(roomy, CalloutKind::AuxFan) != nullptr);
}

// ---------------------------------------------------------------------------
// Band layout: chips beside the part they name, the toolhead as one block
// ---------------------------------------------------------------------------

namespace {

CalloutLayoutInput bands() {
    CalloutLayoutInput in;
    in.area_w = 342;
    in.area_h = 270;
    in.image_w = 645;
    in.image_h = 908;
    in.tagged = true;
    in.chip_h = 22;
    in.gap = 4;
    in.min_line = 8;
    return in;
}

int centre_y(const CalloutChipOut& c) {
    return c.rect.y + c.rect.h / 2;
}

bool left_of_image(const CalloutLayout& l, const CalloutChipOut& c) {
    return c.rect.x + c.rect.w <= l.image.x;
}

} // namespace

TEST_CASE("band chips sit on their part's side", "[printer_image][callout_layout]") {
    auto in = bands();
    in.budget = {{CalloutKind::Nozzle, 60, NormPoint{0.441f, 0.36f}},
                 {CalloutKind::Chamber, 60, NormPoint{0.2f, 0.554f}},
                 {CalloutKind::Light, 60, NormPoint{0.811f, 0.14f}},
                 {CalloutKind::Bed, 60, NormPoint{0.346f, 0.382f}},
                 {CalloutKind::SpeedFlow, 60, std::nullopt}};
    in.active = in.budget;
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::BothSides);
    CHECK(left_of_image(l, *find(l, CalloutKind::Nozzle)));
    CHECK(left_of_image(l, *find(l, CalloutKind::Chamber)));
    CHECK(left_of_image(l, *find(l, CalloutKind::Bed)));
    CHECK_FALSE(left_of_image(l, *find(l, CalloutKind::Light)));
    // Three chips left, one right: the chip with no point takes the emptier band.
    CHECK_FALSE(left_of_image(l, *find(l, CalloutKind::SpeedFlow)));
}

TEST_CASE("toolhead group stacks contiguously in the nozzle band",
          "[printer_image][callout_layout]") {
    auto in = bands();
    in.budget = {{CalloutKind::Nozzle, 60, NormPoint{0.441f, 0.36f}},
                 {CalloutKind::Fan, 60, NormPoint{0.441f, 0.318f}},
                 {CalloutKind::HotendFan, 60, NormPoint{0.441f, 0.36f}},
                 {CalloutKind::Light, 60, NormPoint{0.811f, 0.14f}}};
    in.active = in.budget;
    const auto l = compute_callout_layout(in);
    REQUIRE((l.mode == CalloutMode::BothSides || l.mode == CalloutMode::OneSide));
    const auto *nozzle = find(l, CalloutKind::Nozzle), *fan = find(l, CalloutKind::Fan),
               *hot = find(l, CalloutKind::HotendFan);
    REQUIRE((nozzle && fan && hot));
    CHECK(left_of_image(l, *fan) == left_of_image(l, *nozzle));
    CHECK(left_of_image(l, *hot) == left_of_image(l, *nozzle));
    // One block: the normal gap between neighbours, nothing spread inside it.
    CHECK(hot->rect.y == fan->rect.y + in.chip_h + in.gap);
    CHECK(nozzle->rect.y == hot->rect.y + in.chip_h + in.gap);
}

TEST_CASE("toolhead group orders by anchor, fan above temp on a shared point",
          "[printer_image][callout_layout]") {
    auto in = bands();
    // The part fan's point is BELOW the nozzle here, so it sits below the temp;
    // the hotend fan shares the nozzle's point and sits above the temp.
    in.budget = {{CalloutKind::Nozzle, 60, NormPoint{0.441f, 0.30f}},
                 {CalloutKind::Fan, 60, NormPoint{0.441f, 0.40f}},
                 {CalloutKind::HotendFan, 60, NormPoint{0.441f, 0.30f}}};
    in.active = in.budget;
    const auto l = compute_callout_layout(in);
    REQUIRE((l.mode == CalloutMode::BothSides || l.mode == CalloutMode::OneSide));
    const auto *nozzle = find(l, CalloutKind::Nozzle), *fan = find(l, CalloutKind::Fan),
               *hot = find(l, CalloutKind::HotendFan);
    REQUIRE((nozzle && fan && hot));
    CHECK(hot->rect.y < nozzle->rect.y);
    CHECK(nozzle->rect.y < fan->rect.y);
}

TEST_CASE("band units spread over the full height", "[printer_image][callout_layout]") {
    auto in = bands();
    in.budget = {{CalloutKind::Nozzle, 60, NormPoint{0.3f, 0.30f}},
                 {CalloutKind::Bed, 60, NormPoint{0.3f, 0.34f}},
                 {CalloutKind::Chamber, 60, NormPoint{0.3f, 0.38f}}};
    in.active = in.budget;
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::BothSides);
    REQUIRE(l.chips.size() == 3);
    int top = in.area_h, bottom = 0;
    for (const auto& c : l.chips) {
        top = std::min(top, c.rect.y);
        bottom = std::max(bottom, c.rect.y + c.rect.h);
    }
    CHECK(top == in.gap);
    CHECK(bottom == in.area_h - in.gap);
    // Equal gaps between the three.
    std::vector<int> ys;
    for (const auto& c : l.chips)
        ys.push_back(c.rect.y);
    std::sort(ys.begin(), ys.end());
    CHECK((ys[1] - ys[0] - in.chip_h) / 2 == (ys[2] - ys[1] - in.chip_h) / 2);
    // A band with one unit sits level with its point.
    auto one = bands();
    one.budget = {{CalloutKind::Light, 60, NormPoint{0.811f, 0.5f}}};
    one.active = one.budget;
    const auto l1 = compute_callout_layout(one);
    REQUIRE(l1.chips.size() == 1);
    CHECK(std::abs(centre_y(l1.chips[0]) - (l1.image.y + l1.image.h / 2)) <= 1);
}

TEST_CASE("untagged chips take the slots that keep tagged chips nearest",
          "[printer_image][callout_layout]") {
    auto in = bands();
    in.image_w = 756;
    in.image_h = 939;
    in.budget = {{CalloutKind::Nozzle, 60, NormPoint{0.51f, 0.405f}},
                 {CalloutKind::Fan, 60, NormPoint{0.483f, 0.282f}},
                 {CalloutKind::HotendFan, 60, NormPoint{0.51f, 0.405f}},
                 {CalloutKind::Bed, 60, NormPoint{0.44f, 0.835f}},
                 {CalloutKind::Light, 60, std::nullopt},
                 {CalloutKind::Chamber, 60, std::nullopt}};
    in.active = in.budget;
    const auto l = compute_callout_layout(in);
    REQUIRE((l.mode == CalloutMode::BothSides || l.mode == CalloutMode::OneSide));
    const auto* bed = find(l, CalloutKind::Bed);
    REQUIRE(bed);
    CHECK(centre_y(*bed) > in.area_h / 2);
    CHECK_FALSE(find(l, CalloutKind::Light)->has_line);
    CHECK_FALSE(find(l, CalloutKind::Chamber)->has_line);
}

TEST_CASE("an untagged chip takes the middle slot between two tagged ones",
          "[printer_image][callout_layout]") {
    auto in = bands();
    in.budget = {{CalloutKind::Nozzle, 60, NormPoint{0.3f, 0.3f}},
                 {CalloutKind::Chamber, 60, NormPoint{0.3f, 0.5f}},
                 {CalloutKind::Bed, 60, NormPoint{0.3f, 0.7f}},
                 {CalloutKind::Light, 60, NormPoint{0.8f, 0.15f}},
                 {CalloutKind::AuxFan, 60, NormPoint{0.8f, 0.85f}},
                 {CalloutKind::SpeedFlow, 60, std::nullopt}};
    in.active = in.budget;
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::BothSides);
    const auto *light = find(l, CalloutKind::Light), *aux = find(l, CalloutKind::AuxFan),
               *speed = find(l, CalloutKind::SpeedFlow);
    REQUIRE((light && aux && speed));
    CHECK(light->rect.y < speed->rect.y);
    CHECK(speed->rect.y < aux->rect.y);
}

namespace {

long cross(CalloutPoint o, CalloutPoint a, CalloutPoint b) {
    return long(a.x - o.x) * (b.y - o.y) - long(a.y - o.y) * (b.x - o.x);
}

bool segments_cross(CalloutPoint a, CalloutPoint b, CalloutPoint c, CalloutPoint d) {
    const long d1 = cross(a, b, c), d2 = cross(a, b, d), d3 = cross(c, d, a), d4 = cross(c, d, b);
    return ((d1 > 0) != (d2 > 0)) && d1 != 0 && d2 != 0 && ((d3 > 0) != (d4 > 0)) && d3 != 0 &&
           d4 != 0;
}

} // namespace

TEST_CASE("no two leaders cross", "[printer_image][callout_layout]") {
    std::ifstream f("assets/images/printers/regions.json");
    REQUIRE(f.good());
    const auto all = parse_image_regions(std::string((std::istreambuf_iterator<char>(f)), {}));
    int lined_layouts = 0;
    for (const char* name : {"voron-v2", "flashforge-adventurer-5m", "creality-k2-plus-cfs",
                             "snapmaker-u1", "elegoo-centauri-carbon", "qidi-q2"}) {
        const auto it = all.find(name);
        REQUIRE(it != all.end());
        const auto& r = it->second;
        auto in = bands();
        in.image_w = r.src_w;
        in.image_h = r.src_h;
        for (const auto k : {CalloutKind::Nozzle, CalloutKind::Fan, CalloutKind::HotendFan,
                             CalloutKind::Bed, CalloutKind::Chamber, CalloutKind::Light})
            if (const auto a = region_anchor(r, k))
                in.budget.push_back({k, 60, a});
        in.budget.push_back({CalloutKind::AuxFan, 60, std::nullopt});
        in.budget.push_back({CalloutKind::SpeedFlow, 60, std::nullopt});
        in.active = in.budget;
        const auto l = compute_callout_layout(in);
        INFO(name << " mode " << int(l.mode));
        if (l.mode != CalloutMode::BothSides && l.mode != CalloutMode::OneSide)
            continue;
        ++lined_layouts;
        for (size_t i = 0; i < l.chips.size(); ++i)
            for (size_t j = i + 1; j < l.chips.size(); ++j) {
                const auto &a = l.chips[i], &b = l.chips[j];
                if (!a.has_line || !b.has_line || left_of_image(l, a) != left_of_image(l, b))
                    continue;
                const CalloutPoint pa[] = {
                    {a.line_x0, a.line_y0}, {a.line_xm, a.line_ym}, {a.line_x1, a.line_y1}};
                const CalloutPoint pb[] = {
                    {b.line_x0, b.line_y0}, {b.line_xm, b.line_ym}, {b.line_x1, b.line_y1}};
                for (int s = 0; s < 2; ++s)
                    for (int t = 0; t < 2; ++t)
                        CHECK_FALSE(segments_cross(pa[s], pa[s + 1], pb[t], pb[t + 1]));
            }
    }
    CHECK(lined_layouts >= 1);
}

TEST_CASE("hotend fan drops first when chips do not fit", "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 286; // one 70px column beside the image; a 150px chip overflows it
    // Dropping the hotend fan alone brings the widest chip down to 70px.
    in.budget = {{CalloutKind::Nozzle, 70, NormPoint{0.51f, 0.28f}},
                 {CalloutKind::Bed, 70, NormPoint{0.46f, 0.57f}},
                 {CalloutKind::Light, 50, NormPoint{0.8f, 0.2f}, true},
                 {CalloutKind::AuxFan, 50, std::nullopt, true},
                 {CalloutKind::HotendFan, 150, NormPoint{0.51f, 0.28f}, true}};
    in.active = in.budget;
    const auto l = compute_callout_layout(in);
    CHECK(l.mode != CalloutMode::Pinned);
    REQUIRE(l.dropped.size() == 1);
    CHECK(l.dropped[0] == CalloutKind::HotendFan);
    CHECK(find(l, CalloutKind::Light));
    CHECK(find(l, CalloutKind::AuxFan));

    // Pinned columns give up the hotend fan before the light and the aux fan.
    auto tall = base();
    tall.area_w = 150;
    tall.area_h = 190;
    tall.chip_h = 30;
    tall.active = {{CalloutKind::Nozzle, 60, NormPoint{0.5f, 0.30f}},
                   {CalloutKind::Bed, 60, NormPoint{0.5f, 0.32f}},
                   {CalloutKind::Light, 40, NormPoint{0.5f, 0.34f}, true},
                   {CalloutKind::AuxFan, 40, NormPoint{0.5f, 0.36f}, true},
                   {CalloutKind::HotendFan, 40, NormPoint{0.5f, 0.38f}, true},
                   {CalloutKind::Fan, 40, NormPoint{0.5f, 0.40f}}};
    const auto p = compute_callout_layout(tall);
    REQUIRE(p.mode == CalloutMode::Pinned);
    REQUIRE(p.dropped.size() == 1);
    CHECK(p.dropped[0] == CalloutKind::HotendFan);
}

TEST_CASE("a lined tile keeps its labels and drops an idle chip instead",
          "[printer_image][callout_layout]") {
    auto in = wide();
    // Room for one 110px column beside the image, at most. The labeled part
    // fan fits it; a labeled idle aux fan (170px) does not even with the image
    // shrunk, though both would
    // unlabeled (45px).
    in.area_w = 186 + 110 + in.gap + in.min_line;
    in.budget = {{CalloutKind::Nozzle, 70, NormPoint{0.51f, 0.28f}},
                 {CalloutKind::Bed, 70, NormPoint{0.46f, 0.57f}},
                 {CalloutKind::Fan, 110, NormPoint{0.49f, 0.21f}, false, 45},
                 {CalloutKind::AuxFan, 170, NormPoint{0.7f, 0.40f}, true, 45}};
    in.active = in.budget;
    const auto l = compute_callout_layout(in);
    CHECK(l.mode == CalloutMode::OneSide);
    CHECK_FALSE(l.unlabeled);
    REQUIRE(l.dropped.size() == 1);
    CHECK(l.dropped[0] == CalloutKind::AuxFan);
    const auto* fan = find(l, CalloutKind::Fan);
    REQUIRE(fan != nullptr);
    CHECK(fan->rect.w == 110);

    // A tile too small for lines staggers compact chips, which carry no labels.
    in.area_w = 150;
    const auto compact = compute_callout_layout(in);
    CHECK(compact.mode == CalloutMode::Staggered);
    CHECK(compact.unlabeled);
}

// ---------------------------------------------------------------------------
// A chip taller than one text line (the two-line speed/flow chip)
// ---------------------------------------------------------------------------

namespace {

// wide() with one column beside the full-size image, plus a pointless chip
// `h` tall riding in that column.
CalloutLayoutInput one_side_with_tall_chip(int h) {
    auto in = wide();
    in.area_w = 186 + 70 + in.gap + in.min_line;
    CalloutChipIn tall{CalloutKind::SpeedFlow, 60, std::nullopt};
    tall.h = h;
    in.budget.push_back(tall);
    in.active = in.budget;
    return in;
}

} // namespace

TEST_CASE("a tall chip stacks in its column at its own height and clear of the others",
          "[printer_image][callout_layout]") {
    // Four 18px chips and a 40px one with 4px gaps: 128 of the 132px the column has.
    const auto in = one_side_with_tall_chip(40);
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::OneSide);
    const auto* tall = find(l, CalloutKind::SpeedFlow);
    REQUIRE(tall);
    CHECK(tall->rect.h == 40);
    CHECK(tall->rect.y + tall->rect.h <= in.area_h - in.gap);
    for (const auto& c : l.chips)
        if (&c != tall)
            CHECK(c.rect.h == in.chip_h);
    check_apart_and_inside(l, in);
}

TEST_CASE("the column's stack-fit counts a tall chip's real height",
          "[printer_image][callout_layout]") {
    // One text line tall the chip fits the column; 50px tall the five need 138 of 132,
    // so they split over both edges instead.
    CHECK(compute_callout_layout(one_side_with_tall_chip(0)).mode == CalloutMode::OneSide);
    const auto in = one_side_with_tall_chip(50);
    const auto l = compute_callout_layout(in);
    CHECK(l.mode == CalloutMode::Staggered);
    check_apart_and_inside(l, in);
}

TEST_CASE("a leader meets a tall chip at its centre", "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 186 + 70 + in.gap + in.min_line;
    for (auto* v : {&in.budget, &in.active})
        for (auto& c : *v)
            if (c.kind == CalloutKind::Bed)
                c.h = 40;
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::OneSide);
    const auto* bed = find(l, CalloutKind::Bed);
    REQUIRE(bed);
    REQUIRE(bed->has_line);
    CHECK(bed->rect.h == 40);
    CHECK(bed->line_y1 == bed->rect.y + 20);
    CHECK(bed->line_ym == bed->line_y1);
    check_apart_and_inside(l, in);
}

TEST_CASE("a tall docked chip sits inside the area on the row's bottom edge",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.tagged = false;
    in.area_w = 300;
    in.active = {{CalloutKind::Nozzle, 60, std::nullopt}, {CalloutKind::Bed, 60, std::nullopt}};
    CalloutChipIn tall{CalloutKind::SpeedFlow, 60, std::nullopt};
    tall.h = 40;
    in.active.push_back(tall);
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Docked);
    const auto* t = find(l, CalloutKind::SpeedFlow);
    const auto* n = find(l, CalloutKind::Nozzle);
    REQUIRE(t);
    REQUIRE(n);
    CHECK(t->rect.h == 40);
    CHECK(t->rect.y + t->rect.h == n->rect.y + n->rect.h);
    check_apart_and_inside(l, in);
}

TEST_CASE("a tall chip docks in the free space beside bands above and below",
          "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 240;
    in.area_h = 480; // image 240x180 centred: 150px above and below
    CalloutChipIn tall{CalloutKind::SpeedFlow, 60, std::nullopt};
    tall.h = 40;
    in.budget.push_back(tall);
    in.active = in.budget;
    const auto l = compute_callout_layout(in);
    REQUIRE((l.mode == CalloutMode::BothSides || l.mode == CalloutMode::OneSide));
    const auto* t = find(l, CalloutKind::SpeedFlow);
    REQUIRE(t);
    CHECK(t->rect.h == 40);
    CHECK_FALSE(overlaps(t->rect, l.image));
    check_apart_and_inside(l, in);
}

TEST_CASE("a tall pinned chip is centred on its point at its own height",
          "[printer_image][callout_layout]") {
    auto in = base();
    CalloutChipIn tall{CalloutKind::Bed, 40, NormPoint{0.5f, 0.5f}};
    tall.h = 30;
    in.active = {tall};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Pinned);
    REQUIRE(l.chips.size() == 1);
    const auto& r = l.chips[0].rect;
    CHECK(r.h == 30);
    CHECK(r.y + r.h / 2 == l.image.y + l.image.h / 2);
    check_apart_and_inside(l, in);
}

namespace {

// Every chip the print status tile can show at once, on a U1-shaped picture,
// at widths and heights measured from the live chip style at that screen size.
// `two_line_h` is the speed/flow chip's height.
CalloutLayoutInput eight_chips(int area_w, int area_h, int chip_h, int two_line_h, int gap,
                               int min_line, int heater_w, int fan_w, int speed_w) {
    CalloutLayoutInput in;
    in.area_w = area_w;
    in.area_h = area_h;
    in.image_w = 158;
    in.image_h = 215;
    in.tagged = true;
    in.chip_h = chip_h;
    in.gap = gap;
    in.min_line = min_line;
    CalloutChipIn speed{CalloutKind::SpeedFlow, speed_w, std::nullopt, true};
    speed.h = two_line_h;
    in.budget = {{CalloutKind::Nozzle, heater_w, NormPoint{0.5f, 0.35f}},
                 {CalloutKind::Bed, heater_w, NormPoint{0.5f, 0.7f}},
                 {CalloutKind::Chamber, heater_w, NormPoint{0.5f, 0.2f}},
                 {CalloutKind::Fan, fan_w, NormPoint{0.55f, 0.4f}},
                 {CalloutKind::HotendFan, fan_w, NormPoint{0.5f, 0.35f}, true},
                 {CalloutKind::AuxFan, fan_w, std::nullopt, true},
                 {CalloutKind::Light, chip_h, NormPoint{0.2f, 0.1f}},
                 speed};
    in.active = in.budget;
    return in;
}

void check_eight(const CalloutLayoutInput& in) {
    const auto l = compute_callout_layout(in);
    CHECK(l.mode != CalloutMode::ImageOnly);
    CHECK_FALSE(l.dropped.empty()); // eight do not all fit: expendable chips give way
    for (const CalloutKind k : l.dropped)
        CHECK((k == CalloutKind::HotendFan || k == CalloutKind::AuxFan ||
               k == CalloutKind::SpeedFlow));
    if (const auto* speed = find(l, CalloutKind::SpeedFlow))
        CHECK(speed->rect.h == in.budget.back().h);
    check_apart_and_inside(l, in);
}

} // namespace

TEST_CASE("every chip at once in an 800x480 tile: none overlaps or leaves the area",
          "[printer_image][callout_layout]") {
    check_eight(eight_chips(325, 265, 30, 52, 5, 10, 152, 109, 127));
}

TEST_CASE("every chip at once in a 480x272 tile: none overlaps or leaves the area",
          "[printer_image][callout_layout]") {
    check_eight(eight_chips(198, 134, 22, 34, 2, 6, 109, 74, 86));
}

TEST_CASE("a tall chip in an untagged side column stacks at its own height",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.tagged = false;
    in.area_w = 300; // image 186x140 centred: a 57px band right of it
    CalloutChipIn tall{CalloutKind::SpeedFlow, 40, std::nullopt};
    tall.h = 40;
    in.active = {
        {CalloutKind::Nozzle, 40, std::nullopt}, tall, {CalloutKind::Bed, 40, std::nullopt}};
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::Docked);
    REQUIRE(l.chips.size() == 3);
    for (const auto& c : l.chips)
        CHECK(c.rect.x >= l.image.x + l.image.w); // the side column, not the bottom row
    CHECK(l.chips[1].rect.h == 40);
    CHECK(l.chips[2].rect.y >= l.chips[1].rect.y + 40 + in.gap);
    check_apart_and_inside(l, in);
}

TEST_CASE("bands above and below are as deep as their tallest chip",
          "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 240;
    in.area_h = 480; // image 240x180 centred: 150px above and below
    for (auto* v : {&in.budget, &in.active})
        for (auto& c : *v)
            if (c.kind == CalloutKind::Fan)
                c.h = 148; // with gap and min_line, deeper than either band
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode != CalloutMode::ImageOnly);
    for (const auto& c : l.chips) {
        CAPTURE(int(c.kind));
        CHECK_FALSE(overlaps(c.rect, l.image));
    }
    check_apart_and_inside(l, in);
}

TEST_CASE("a tall chip in a band below the image ends on the far edge at its own height",
          "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 240;
    in.area_h = 480; // image 240x180 centred: 150px above and below
    for (auto* v : {&in.budget, &in.active})
        for (auto& c : *v)
            if (c.kind == CalloutKind::Bed)
                c.h = 40; // its point is below the image's middle: the bottom band
    const auto l = compute_callout_layout(in);
    REQUIRE((l.mode == CalloutMode::BothSides || l.mode == CalloutMode::OneSide));
    const auto* bed = find(l, CalloutKind::Bed);
    REQUIRE(bed);
    REQUIRE(bed->has_line);
    CHECK(bed->rect.h == 40);
    CHECK(bed->rect.y + bed->rect.h == in.area_h - in.gap);
    CHECK(bed->line_y1 == bed->rect.y); // the leader enters its near (top) edge
    check_apart_and_inside(l, in);
}

TEST_CASE("a tall chip in the toolhead group pushes the chips below it down",
          "[printer_image][callout_layout]") {
    auto in = wide();
    in.area_w = 186 + 70 + in.gap + in.min_line; // one column beside the full-size image
    // The fan and a 60px nozzle stack as one block, 82px with the gap; with the
    // chamber and bed, 126 of the 132px the column has.
    for (auto* v : {&in.budget, &in.active})
        for (auto& c : *v)
            if (c.kind == CalloutKind::Nozzle)
                c.h = 60;
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::OneSide);
    REQUIRE(find(l, CalloutKind::Nozzle)->rect.h == 60);
    check_apart_and_inside(l, in);
}

TEST_CASE("a loose chip finds the only free spot, off the gap grid",
          "[printer_image][callout_layout]") {
    auto in = base();
    in.area_h = 200;
    const CalloutChipIn chip{CalloutKind::AuxFan, 40, std::nullopt};
    std::vector<CalloutChipOut> out;
    CalloutRect img;
    CalloutRect want;
    SECTION("the bottom-left corner, at both strips' far ends") {
        in.area_w = 203; // neither strip's span is a whole number of gaps
        img = {0, 0, 203, 178};
        out = {{CalloutKind::Light, {48, 150, 155, 50}}};
        want = {in.gap, 200 - in.gap - in.chip_h, 40, in.chip_h};
    }
    SECTION("a slot exactly as wide as the chip between two placed chips") {
        in.area_w = 200;
        img = {0, 0, 200, 170};
        out = {{CalloutKind::Light, {0, 160, 49, 40}}, {CalloutKind::Bed, {97, 160, 103, 40}}};
        want = {53, 200 - in.gap - in.chip_h, 40, in.chip_h};
    }
    REQUIRE(callout_detail::dock_free(in, img, {chip}, out));
    const CalloutRect& got = out.back().rect;
    INFO("docked at " << got.x << "," << got.y);
    CHECK(got.x == want.x);
    CHECK(got.y == want.y);
}
