// SPDX-License-Identifier: GPL-3.0-or-later
#include "callout_layout.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr int kVoronW = 710, kVoronH = 923;       // a picture taller than wide
constexpr int kCorexyW = 829, kCorexyH = 871;     // a squarer one
constexpr int kChipH = 22, kRowGap = 6, kGap = 4; // the print status tile's chips at micro

CalloutChipIn heater(CalloutKind k) {
    CalloutChipIn c{k, 100, NormPoint{0.5f, 0.5f}};
    c.w_row = 72;       // icon, reading, status glyph
    c.w_row_plain = 52; // icon and reading
    return c;
}

CalloutChipIn fan(CalloutKind k, bool expendable) {
    CalloutChipIn c{k, 80, NormPoint{0.6f, 0.4f}, expendable, 59};
    return c;
}

/// The print status printer tile in portrait, as measured at 272x480 (row 256)
/// and 320x480 (row 304), with the row offered and hidden.
CalloutLayoutInput portrait(int area_w, int area_h, int img_w, int img_h, int row_w) {
    CalloutLayoutInput in;
    in.area_w = area_w;
    in.area_h = area_h;
    in.image_w = img_w;
    in.image_h = img_h;
    in.tagged = true;
    in.chip_h = kChipH;
    in.compact_chip_h = 21;
    in.gap = kGap;
    in.min_line = 8;
    in.row = {row_w, kRowGap, false};
    in.budget = {heater(CalloutKind::Nozzle), heater(CalloutKind::Bed),
                 heater(CalloutKind::Chamber), fan(CalloutKind::Fan, false)};
    in.active = in.budget;
    return in;
}

bool overlaps(const CalloutRect& a, const CalloutRect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

void check_in_row(const CalloutLayout& l, int row_w) {
    CHECK(l.row_h == kChipH);
    for (size_t i = 0; i < l.chips.size(); ++i) {
        const CalloutRect& r = l.chips[i].rect;
        CHECK((r.x >= 0 && r.y >= 0 && r.x + r.w <= row_w && r.y + r.h <= l.row_h));
        CHECK_FALSE(l.chips[i].has_line);
        for (size_t j = i + 1; j < l.chips.size(); ++j)
            CHECK_FALSE(overlaps(r, l.chips[j].rect));
    }
}

} // namespace

TEST_CASE("row below: portrait micro puts the chips under both tiles at the picture's full size",
          "[callout_layout][print_status]") {
    auto in = portrait(115, 182, kVoronW, kVoronH, 256);
    const CalloutRect full = fit_image(115, 182, kVoronW, kVoronH);

    const auto hidden = compute_callout_layout(in);
    REQUIRE(hidden.mode == CalloutMode::RowBelow);
    CHECK(hidden.chips.size() == 4);
    CHECK(hidden.dropped.empty());
    // Three heaters with glyphs and the fan need 287px; the 256px row takes them plain.
    CHECK(hidden.row_plain);
    check_in_row(hidden, 256);

    // Shown, the row has taken its chip and gap from the area: the decision and
    // the picture's size stand.
    in.area_h -= kChipH + kRowGap;
    in.row.taken = kChipH + kRowGap;
    const auto shown = compute_callout_layout(in);
    REQUIRE(shown.mode == CalloutMode::RowBelow);
    CHECK(shown.image.w == full.w);
    CHECK(shown.image.h == full.h);
    CHECK(shown.chips.size() == 4);
    check_in_row(shown, 256);
}

TEST_CASE("row below: the row keeps the heaters' status glyphs where they fit",
          "[callout_layout][print_status]") {
    const auto l = compute_callout_layout(portrait(139, 202, kCorexyW, kCorexyH, 304));
    REQUIRE(l.mode == CalloutMode::RowBelow);
    CHECK_FALSE(l.row_plain);
    CHECK(l.chips.size() == 4);
    check_in_row(l, 304);
    // Heaters first, in reading order, the fan last.
    CHECK(l.chips.front().kind == CalloutKind::Nozzle);
    CHECK(l.chips.back().kind == CalloutKind::Fan);
}

TEST_CASE("row below: expected values go before the status glyphs",
          "[callout_layout][print_status]") {
    auto in = portrait(139, 202, kCorexyW, kCorexyH, 304);
    in.budget.push_back(fan(CalloutKind::HotendFan, true));
    in.budget.push_back({CalloutKind::Light, 26, std::nullopt, true});
    in.active = in.budget;
    const auto l = compute_callout_layout(in);
    REQUIRE(l.mode == CalloutMode::RowBelow);
    CHECK_FALSE(l.row_plain);
    CHECK(l.chips.size() == 4);
    CHECK(std::find(l.dropped.begin(), l.dropped.end(), CalloutKind::HotendFan) != l.dropped.end());
    CHECK(std::find(l.dropped.begin(), l.dropped.end(), CalloutKind::Light) != l.dropped.end());
}

TEST_CASE("row below: a row that would shrink the picture is not used",
          "[callout_layout][print_status]") {
    // 320x480 with a lane strip: the tall picture already fills the area's height.
    auto in = portrait(139, 176, kVoronW, kVoronH, 304);
    const auto l = compute_callout_layout(in);
    CHECK(l.mode != CalloutMode::RowBelow);
    in.row = {};
    CHECK(l.mode == compute_callout_layout(in).mode);
}

TEST_CASE("row below: chips the row cannot hold even plain leave the layout as it was",
          "[callout_layout][print_status]") {
    auto in = portrait(115, 182, kVoronW, kVoronH, 256);
    in.budget.push_back({CalloutKind::OtherTools, 117, std::nullopt, false});
    in.active = in.budget;
    const auto l = compute_callout_layout(in);
    CHECK(l.mode != CalloutMode::RowBelow);
    in.row = {};
    CHECK(l.mode == compute_callout_layout(in).mode);
}

TEST_CASE("row below: a strip that takes more than its budget does not toggle the mode",
          "[callout_layout][print_status]") {
    auto in = portrait(115, 182, kVoronW, kVoronH, 256);
    REQUIRE(compute_callout_layout(in).mode == CalloutMode::RowBelow);
    // Shown, the strip took 40px rather than its 28: the area is judged with
    // those 40 given back, so the second pass decides as the first did.
    in.area_h = 182 - 40;
    in.row.taken = 40;
    CHECK(compute_callout_layout(in).mode == CalloutMode::RowBelow);
}
