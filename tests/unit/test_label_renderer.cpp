// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "label_renderer.h"
#include "spoolman_types.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

static SpoolInfo make_test_spool() {
    SpoolInfo spool;
    spool.id = 42;
    spool.vendor = "Hatchbox";
    spool.material = "PLA";
    spool.filament_name = "Red";
    spool.remaining_weight_g = 800;
    spool.initial_weight_g = 1000;
    spool.lot_nr = "LOT-2026-001";
    spool.comment = "Great filament";
    spool.spool_weight_g = 200;
    return spool;
}

static helix::LabelSize continuous_62mm() {
    return {"62mm", 696, 0, 300, 0x0A, 62, 0};
}

static helix::LabelSize continuous_29mm() {
    return {"29mm", 306, 0, 300, 0x0A, 29, 0};
}

static helix::LabelSize diecut_62x29() {
    return {"62x29mm", 696, 271, 300, 0x0B, 62, 29};
}

/// Check if bitmap has any black pixels
static bool has_black_pixels(const helix::LabelBitmap& bmp) {
    for (int y = 0; y < bmp.height(); y++)
        for (int x = 0; x < bmp.width(); x++)
            if (bmp.get_pixel(x, y))
                return true;
    return false;
}

TEST_CASE("LabelRenderer STANDARD preset produces valid bitmap", "[label]") {
    auto spool = make_test_spool();
    auto label =
        helix::LabelRenderer::render(spool, helix::LabelPreset::STANDARD, continuous_62mm());

    REQUIRE_FALSE(label.empty());
    REQUIRE(label.width() == 696);
    REQUIRE(label.height() > 0);
    REQUIRE(has_black_pixels(label));
}

TEST_CASE("LabelRenderer MINIMAL preset renders", "[label]") {
    auto spool = make_test_spool();
    auto label =
        helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, continuous_62mm());

    REQUIRE_FALSE(label.empty());
    REQUIRE(label.width() == 696);
    REQUIRE(label.height() > 0);
    REQUIRE(has_black_pixels(label));
}

TEST_CASE("LabelRenderer COMPACT preset", "[label]") {
    auto spool = make_test_spool();
    auto label =
        helix::LabelRenderer::render(spool, helix::LabelPreset::COMPACT, continuous_62mm());

    REQUIRE_FALSE(label.empty());
    REQUIRE(label.width() == 696);
    REQUIRE(label.height() > 0);
    REQUIRE(has_black_pixels(label));
}

TEST_CASE("LabelRenderer 29mm label", "[label]") {
    auto spool = make_test_spool();
    auto label =
        helix::LabelRenderer::render(spool, helix::LabelPreset::STANDARD, continuous_29mm());

    REQUIRE_FALSE(label.empty());
    REQUIRE(label.width() == 306);
    REQUIRE(label.height() > 0);
}

TEST_CASE("LabelRenderer die-cut label fits dimensions", "[label]") {
    auto spool = make_test_spool();
    auto size = diecut_62x29();
    auto label = helix::LabelRenderer::render(spool, helix::LabelPreset::STANDARD, size);

    REQUIRE(label.width() == 696);
    REQUIRE(label.height() == 271);
    REQUIRE(has_black_pixels(label));
}

TEST_CASE("LabelRenderer handles empty vendor and color", "[label]") {
    SpoolInfo spool;
    spool.id = 1;
    spool.material = "PETG";
    // vendor and color_name empty

    auto label =
        helix::LabelRenderer::render(spool, helix::LabelPreset::STANDARD, continuous_62mm());
    REQUIRE_FALSE(label.empty());
    REQUIRE(has_black_pixels(label));
}

TEST_CASE("LabelRenderer continuous height adapts to content", "[label]") {
    auto spool = make_test_spool();
    auto minimal =
        helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, continuous_62mm());
    auto standard =
        helix::LabelRenderer::render(spool, helix::LabelPreset::STANDARD, continuous_62mm());

    REQUIRE(minimal.height() > 0);
    REQUIRE(standard.height() > 0);
    // STANDARD has text alongside QR, so may differ in height
}

TEST_CASE("LabelRenderer MINIMAL die-cut centers QR", "[label]") {
    auto spool = make_test_spool();
    auto size = diecut_62x29();
    auto label = helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, size);

    REQUIRE(label.width() == 696);
    REQUIRE(label.height() == 271);

    // QR should not touch the very edges (there should be margin)
    bool top_row_clear = true;
    for (int x = 0; x < label.width(); x++)
        if (label.get_pixel(x, 0))
            top_row_clear = false;
    REQUIRE(top_row_clear);
}

TEST_CASE("LabelRenderer COMPACT wider label produces larger content", "[label]") {
    auto spool = make_test_spool();
    auto compact_62 =
        helix::LabelRenderer::render(spool, helix::LabelPreset::COMPACT, continuous_62mm());
    auto compact_29 =
        helix::LabelRenderer::render(spool, helix::LabelPreset::COMPACT, continuous_29mm());

    REQUIRE_FALSE(compact_62.empty());
    REQUIRE_FALSE(compact_29.empty());
    // 62mm label is wider than 29mm
    REQUIRE(compact_62.width() > compact_29.width());
}

TEST_CASE("LabelRenderer MINIMAL QR code capped size", "[label]") {
    auto spool = make_test_spool();
    auto label =
        helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, continuous_62mm());

    REQUIRE_FALSE(label.empty());
    // Find the bounding box of black pixels to check QR size
    int max_y = 0;
    for (int y = 0; y < label.height(); y++)
        for (int x = 0; x < label.width(); x++)
            if (label.get_pixel(x, y))
                max_y = y;

    // QR code height should be reasonable (capped, not filling entire label width)
    REQUIRE(max_y < label.width()); // QR shouldn't be as tall as the label is wide
    REQUIRE(max_y <= 300);          // QR should be capped around 250px + margin
}

// ============================================================================
// Untracked spools (spoolman_id == 0): no Spoolman record, so the QR code and
// the "#n" ID text must be omitted rather than rendering "web+spoolman:s-0"
// and "#0". Negative IDs stay reserved for preview/test labels and keep a QR.
// ============================================================================

/// A spool with no Spoolman record — the AMS slot editor's untracked case.
static SpoolInfo make_untracked_spool() {
    SpoolInfo spool;
    spool.id = 0; // untracked
    spool.vendor = "Hatchbox";
    spool.material = "PLA";
    spool.filament_name = "Red";
    spool.remaining_weight_g = 800;
    return spool;
}

/// Same fields, but with a real Spoolman ID.
static SpoolInfo make_tracked_spool() {
    SpoolInfo spool = make_untracked_spool();
    spool.id = 42;
    return spool;
}

/// Maximal runs of consecutive rows that contain at least one black pixel.
/// A QR code is a single tall contiguous run; text renders as one run per line.
static std::vector<std::pair<int, int>> row_bands(const helix::LabelBitmap& bmp) {
    std::vector<std::pair<int, int>> bands;
    bool in_band = false;
    int start = 0;
    for (int y = 0; y < bmp.height(); y++) {
        bool black = false;
        for (int x = 0; x < bmp.width() && !black; x++)
            black = bmp.get_pixel(x, y);
        if (black && !in_band) {
            in_band = true;
            start = y;
        } else if (!black && in_band) {
            in_band = false;
            bands.emplace_back(start, y - 1);
        }
    }
    if (in_band)
        bands.emplace_back(start, bmp.height() - 1);
    return bands;
}

/// Leftmost/rightmost black pixel across rows [y0, y1].
static std::pair<int, int> x_extent(const helix::LabelBitmap& bmp, int y0, int y1) {
    int left = bmp.width();
    int right = -1;
    for (int y = y0; y <= y1; y++) {
        for (int x = 0; x < bmp.width(); x++) {
            if (bmp.get_pixel(x, y)) {
                if (x < left)
                    left = x;
                if (x > right)
                    right = x;
            }
        }
    }
    return {left, right};
}

static int count_black_pixels(const helix::LabelBitmap& bmp) {
    int count = 0;
    for (int y = 0; y < bmp.height(); y++)
        for (int x = 0; x < bmp.width(); x++)
            if (bmp.get_pixel(x, y))
                count++;
    return count;
}

/// Height of the tallest contiguous run of inked rows. A QR block is ~200px
/// tall on a 62x29mm label; a single text line is at most ~60px (7px glyph
/// rows scaled by <= 8). This cleanly separates "has a QR" from "text only".
static int tallest_band(const helix::LabelBitmap& bmp) {
    int tallest = 0;
    for (const auto& band : row_bands(bmp))
        tallest = std::max(tallest, band.second - band.first + 1);
    return tallest;
}

TEST_CASE("LabelRenderer untracked spool omits the QR code", "[label][untracked]") {
    auto size = diecut_62x29();

    // A tracked spool puts a tall contiguous QR block down the left side.
    auto tracked =
        helix::LabelRenderer::render(make_tracked_spool(), helix::LabelPreset::COMPACT, size);
    REQUIRE(tallest_band(tracked) > 100);

    // An untracked spool has no QR — nothing taller than one text line remains.
    auto untracked =
        helix::LabelRenderer::render(make_untracked_spool(), helix::LabelPreset::COMPACT, size);
    REQUIRE(tallest_band(untracked) <= 70);
    REQUIRE(count_black_pixels(untracked) < count_black_pixels(tracked));

    // Text still renders: vendor/material/color are unaffected.
    REQUIRE(has_black_pixels(untracked));
}

TEST_CASE("LabelRenderer untracked COMPACT drops the spool-ID line", "[label][untracked]") {
    // COMPACT is vendor / material+color / "#n". Without an ID the third line
    // is omitted entirely rather than printing "#0", leaving exactly 2 lines.
    auto label = helix::LabelRenderer::render(make_untracked_spool(), helix::LabelPreset::COMPACT,
                                              diecut_62x29());
    REQUIRE(row_bands(label).size() == 2);
}

TEST_CASE("LabelRenderer untracked STANDARD weight line carries no ID", "[label][untracked]") {
    // STANDARD line 3 is "<weight>  #<id>". For an untracked spool only the
    // weight remains — measure the line's width in character cells to prove the
    // "  #0" suffix is gone. Font metrics: 5x7 glyphs, 1px inter-char gap,
    // uniformly scaled, so band height == 7 * scale.
    auto label = helix::LabelRenderer::render(make_untracked_spool(), helix::LabelPreset::STANDARD,
                                              diecut_62x29());

    auto bands = row_bands(label);
    // vendor / material+color / weight — no QR, no temps, no lot, no comment.
    REQUIRE(bands.size() == 3);

    const auto& weight_band = bands[2];
    int band_h = weight_band.second - weight_band.first + 1;
    int scale = band_h / 7;
    REQUIRE(scale >= 2);

    auto [left, right] = x_extent(label, weight_band.first, weight_band.second);
    REQUIRE(right >= left);
    int char_pitch = (5 + 1) * scale;
    int cells = (right - left + 1 + scale) / char_pitch;

    // "800G" is 4 cells. With the ID appended ("800G  #0") it would be 8.
    REQUIRE(cells <= 5);
}

TEST_CASE("LabelRenderer test label (negative id) keeps its QR", "[label][untracked]") {
    // Negative IDs are the preview/test path and must be untouched by the
    // untracked handling — they still emit a (decoder-rejected) QR payload.
    SpoolInfo spool = make_untracked_spool();
    spool.id = -1;

    auto label = helix::LabelRenderer::render(spool, helix::LabelPreset::COMPACT, diecut_62x29());
    REQUIRE(tallest_band(label) > 100); // QR block present

    // MINIMAL (QR-only) must also still work for the test label.
    auto minimal = helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, diecut_62x29());
    REQUIRE(tallest_band(minimal) > 100);
}

TEST_CASE("LabelRenderer untracked MINIMAL falls back to text", "[label][untracked]") {
    // MINIMAL is QR-only; with no QR payload it would print a blank label, so
    // untracked spools render the COMPACT text layout instead.
    auto label = helix::LabelRenderer::render(make_untracked_spool(), helix::LabelPreset::MINIMAL,
                                              diecut_62x29());

    REQUIRE_FALSE(label.empty());
    REQUIRE(has_black_pixels(label));
    REQUIRE(row_bands(label).size() == 2); // two text lines, no QR
    REQUIRE(tallest_band(label) <= 70);
}

TEST_CASE("LabelRenderer untracked narrow label omits QR and ID", "[label][untracked]") {
    // Narrow labels (<150px wide, e.g. Niimbot D110) render landscape then
    // rotate. The QR must be gone and the text must claim the freed width.
    helix::LabelSize d110{"D110", 96, 307, 203, 0x00, 12, 40};

    auto untracked =
        helix::LabelRenderer::render(make_untracked_spool(), helix::LabelPreset::STANDARD, d110);
    auto tracked =
        helix::LabelRenderer::render(make_tracked_spool(), helix::LabelPreset::STANDARD, d110);

    REQUIRE(untracked.width() == 96);
    REQUIRE(untracked.height() == 307);
    REQUIRE(has_black_pixels(untracked));
    REQUIRE(count_black_pixels(untracked) < count_black_pixels(tracked));
}

TEST_CASE("LabelRenderer STANDARD richer spool produces more content", "[label]") {
    // Minimal spool (just material)
    SpoolInfo minimal_spool;
    minimal_spool.id = 1;
    minimal_spool.material = "PLA";

    // Rich spool (all fields)
    auto rich_spool = make_test_spool();

    auto minimal_label = helix::LabelRenderer::render(minimal_spool, helix::LabelPreset::STANDARD,
                                                      continuous_62mm());
    auto rich_label =
        helix::LabelRenderer::render(rich_spool, helix::LabelPreset::STANDARD, continuous_62mm());

    REQUIRE_FALSE(minimal_label.empty());
    REQUIRE_FALSE(rich_label.empty());
    // Rich spool should produce taller label (more text content)
    REQUIRE(rich_label.height() >= minimal_label.height());
}

// ============================================================================
// Spool number prominence (prestonbrown/helixscreen#1491). The number is what
// a person reads off a label when picking a spool from a rack, so every layout
// prints it on its own line at least as large as the vendor line, never lets
// the weight/length line truncate it, and MINIMAL prints it beside the QR.
// ============================================================================

/// Row bands (as row_bands) restricted to columns x >= x_min, so the text
/// column beside a QR can be measured line by line.
static std::vector<std::pair<int, int>> row_bands_from(const helix::LabelBitmap& bmp, int x_min) {
    std::vector<std::pair<int, int>> bands;
    bool in_band = false;
    int start = 0;
    for (int y = 0; y < bmp.height(); y++) {
        bool black = false;
        for (int x = x_min; x < bmp.width() && !black; x++)
            black = bmp.get_pixel(x, y);
        if (black && !in_band) {
            in_band = true;
            start = y;
        } else if (!black && in_band) {
            in_band = false;
            bands.emplace_back(start, y - 1);
        }
    }
    if (in_band)
        bands.emplace_back(start, bmp.height() - 1);
    return bands;
}

/// Rightmost column of the QR code, which is always the leftmost ink on a
/// tracked label. A QR's first column is its finder patterns' outer edge:
/// solid from the QR's top row to its bottom row, and the QR is square, so
/// that column's ink height is the QR's width.
static int qr_right_edge(const helix::LabelBitmap& bmp) {
    for (int x = 0; x < bmp.width(); x++) {
        int top = -1, bottom = -1;
        for (int y = 0; y < bmp.height(); y++) {
            if (bmp.get_pixel(x, y)) {
                if (top < 0)
                    top = y;
                bottom = y;
            }
        }
        if (top >= 0)
            return x + (bottom - top + 1) - 1;
    }
    return -1;
}

struct LineMetrics {
    int scale; // font scale: band height / 7 glyph rows
    int cells; // characters drawn, from the band's horizontal extent
};

/// Metrics of one text line. Font: 5x7 glyphs, 1-cell gap, uniformly scaled,
/// so a full-height glyph band is 7 * scale rows and each character advances
/// 6 * scale columns. Digits, '#' and capitals all span 5 columns, so the
/// strings measured here round to their exact length.
static LineMetrics line_metrics(const helix::LabelBitmap& bmp, std::pair<int, int> band,
                                int x_min) {
    int scale = (band.second - band.first + 1) / 7;
    int left = bmp.width(), right = -1;
    for (int y = band.first; y <= band.second; y++) {
        for (int x = x_min; x < bmp.width(); x++) {
            if (bmp.get_pixel(x, y)) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    }
    int cells = scale > 0 ? (right - left + 1 + scale) / (6 * scale) : 0;
    return {scale, cells};
}

/// Undo the 90° CW rotation narrow labels get, back to the composed landscape.
static helix::LabelBitmap unrotate(const helix::LabelBitmap& bmp) {
    return bmp.rotate_90_cw().rotate_90_cw().rotate_90_cw();
}

static helix::LabelSize niimbot_d110() {
    return {"D110", 96, 307, 203, 0x00, 12, 40};
}

/// Text lines to the right of the QR, top to bottom.
static std::vector<LineMetrics> text_lines(const helix::LabelBitmap& bmp) {
    int x_min = qr_right_edge(bmp) + 2;
    std::vector<LineMetrics> lines;
    for (const auto& band : row_bands_from(bmp, x_min))
        lines.push_back(line_metrics(bmp, band, x_min));
    return lines;
}

TEST_CASE("LabelRenderer spool number leads STANDARD and COMPACT at the vendor scale",
          "[label][spool-number]") {
    auto spool = make_tracked_spool(); // id 42 -> "#42" (3 cells); "HATCHBOX" is 8

    for (auto preset : {helix::LabelPreset::STANDARD, helix::LabelPreset::COMPACT}) {
        for (auto size : {diecut_62x29(), continuous_62mm(), continuous_29mm()}) {
            auto label = helix::LabelRenderer::render(spool, preset, size);
            auto lines = text_lines(label);
            // STANDARD: number / vendor / material+color / weight.
            // COMPACT: number / vendor / material+color, and nothing else --
            // the number is not repeated as a small trailing line.
            REQUIRE(lines.size() == (preset == helix::LabelPreset::STANDARD ? 4u : 3u));
            CHECK(lines[0].cells == 3); // "#42" is the first line
            CHECK(lines[1].cells == 8); // vendor comes second
            CHECK(lines[0].scale >= lines[1].scale);
            CHECK(lines[0].scale >= 3);
        }
    }
}

TEST_CASE("LabelRenderer narrow column truncates weight before the spool number",
          "[label][spool-number]") {
    // 29mm tape: the text column fits 9 characters. "800G / 330M" is 11, so
    // it loses characters; "#42" must not lose any.
    auto spool = make_tracked_spool();
    spool.remaining_length_m = 330;

    auto label =
        helix::LabelRenderer::render(spool, helix::LabelPreset::STANDARD, continuous_29mm());
    auto lines = text_lines(label);
    REQUIRE(lines.size() == 4); // number / vendor / material+color / weight
    CHECK(lines[0].cells == 3);
    CHECK(lines[3].cells < 11);
    CHECK(lines[3].cells >= 4); // the weight itself is still there
}

TEST_CASE("LabelRenderer spool number leads the landscape layout", "[label][spool-number]") {
    auto spool = make_tracked_spool();
    for (auto preset : {helix::LabelPreset::STANDARD, helix::LabelPreset::COMPACT}) {
        auto label = helix::LabelRenderer::render(spool, preset, niimbot_d110());
        REQUIRE(label.width() == 96);
        REQUIRE(label.height() == 307);

        // The landscape layout is the same for every preset: number / vendor /
        // material+color / weight (temps only when they fit).
        auto lines = text_lines(unrotate(label));
        REQUIRE(lines.size() == 4);
        CHECK(lines[0].cells == 3);
        CHECK(lines[1].cells == 8);
        CHECK(lines[0].scale >= lines[1].scale);
    }
}

TEST_CASE("LabelRenderer MINIMAL prints the spool number beside or below the QR",
          "[label][spool-number]") {
    auto spool = make_tracked_spool();

    SECTION("wide die-cut: beside, as tall as the vendor line would be") {
        auto label =
            helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, diecut_62x29());
        auto lines = text_lines(label);
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].cells == 3);
        CHECK(lines[0].scale >= 6);
    }

    SECTION("wide continuous: beside, label height unchanged") {
        auto label =
            helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, continuous_62mm());
        auto lines = text_lines(label);
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].cells == 3);
        CHECK(lines[0].scale >= 6);
        CHECK(label.height() <= 300); // 250px QR + margins, as before
    }

    SECTION("29mm continuous: no room beside, so below the QR") {
        auto label =
            helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, continuous_29mm());
        auto bands = row_bands(label);
        REQUIRE(bands.size() == 2); // QR block, then the number
        REQUIRE(bands[1].second - bands[1].first < bands[0].second - bands[0].first);
        auto number = line_metrics(label, bands[1], 0);
        CHECK(number.cells == 3);
        CHECK(number.scale >= 6);
    }

    SECTION("23x23mm die-cut: QR shrinks so the number fits below") {
        helix::LabelSize square{"23x23mm", 202, 202, 300, 0x0B, 23, 23};
        auto label = helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, square);
        REQUIRE(label.width() == 202);
        REQUIRE(label.height() == 202);
        auto bands = row_bands(label);
        REQUIRE(bands.size() == 2);
        auto number = line_metrics(label, bands[1], 0);
        CHECK(number.cells == 3);
        CHECK(number.scale >= 3);
    }

    SECTION("narrow label: landscape, beside the QR") {
        auto label =
            helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, niimbot_d110());
        REQUIRE(label.width() == 96);
        REQUIRE(label.height() == 307);
        auto lines = text_lines(unrotate(label));
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].cells == 3);
        CHECK(lines[0].scale >= 6);
    }
}

TEST_CASE("LabelRenderer test label prints TEST where the number goes", "[label][spool-number]") {
    SpoolInfo spool = make_tracked_spool();
    spool.id = -1; // "TEST" is 4 cells

    for (auto preset :
         {helix::LabelPreset::STANDARD, helix::LabelPreset::COMPACT, helix::LabelPreset::MINIMAL}) {
        auto label = helix::LabelRenderer::render(spool, preset, diecut_62x29());
        auto lines = text_lines(label);
        REQUIRE_FALSE(lines.empty());
        CHECK(lines[0].cells == 4);
    }
}

TEST_CASE("LabelRenderer untracked spool prints no number line", "[label][spool-number]") {
    // id == 0: no QR, no number. STANDARD keeps vendor / material / weight,
    // COMPACT keeps vendor / material, and the first line is the vendor.
    auto spool = make_untracked_spool();
    auto standard =
        helix::LabelRenderer::render(spool, helix::LabelPreset::STANDARD, diecut_62x29());
    auto bands = row_bands(standard);
    REQUIRE(bands.size() == 3);
    CHECK(line_metrics(standard, bands[0], 0).cells == 8);

    auto compact = helix::LabelRenderer::render(spool, helix::LabelPreset::COMPACT, diecut_62x29());
    bands = row_bands(compact);
    REQUIRE(bands.size() == 2);
    CHECK(line_metrics(compact, bands[0], 0).cells == 8);
}
