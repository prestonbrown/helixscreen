// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "label_renderer.h"

#include "core/text_io.h"
#include "spoolman_types.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace helix {

// ---------------------------------------------------------------------------
// Built-in 5x7 bitmap font for label rendering (public domain)
// Each character: 7 rows, 5 bits per row stored in upper bits of uint8_t.
// Bit layout per row byte: [b7 b6 b5 b4 b3 _ _ _] where b7=leftmost pixel.
// Covers ASCII 32 (' ') through 126 ('~').
// ---------------------------------------------------------------------------

// clang-format off
static const uint8_t kFont5x7[95][7] = {
    // 32 ' '
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 33 '!'
    {0x20,0x20,0x20,0x20,0x20,0x00,0x20},
    // 34 '"'
    {0x50,0x50,0x00,0x00,0x00,0x00,0x00},
    // 35 '#'
    {0x50,0xF8,0x50,0x50,0xF8,0x50,0x00},
    // 36 '$'
    {0x20,0x78,0xA0,0x70,0x28,0xF0,0x20},
    // 37 '%'
    {0xC8,0xC8,0x10,0x20,0x40,0x98,0x98},
    // 38 '&'
    {0x40,0xA0,0xA0,0x40,0xA8,0x90,0x68},
    // 39 '''
    {0x20,0x20,0x00,0x00,0x00,0x00,0x00},
    // 40 '('
    {0x10,0x20,0x40,0x40,0x40,0x20,0x10},
    // 41 ')'
    {0x40,0x20,0x10,0x10,0x10,0x20,0x40},
    // 42 '*'
    {0x00,0x20,0xA8,0x70,0xA8,0x20,0x00},
    // 43 '+'
    {0x00,0x20,0x20,0xF8,0x20,0x20,0x00},
    // 44 ','
    {0x00,0x00,0x00,0x00,0x00,0x20,0x40},
    // 45 '-'
    {0x00,0x00,0x00,0xF8,0x00,0x00,0x00},
    // 46 '.'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x20},
    // 47 '/'
    {0x00,0x08,0x10,0x20,0x40,0x80,0x00},
    // 48 '0'
    {0x70,0x88,0x98,0xA8,0xC8,0x88,0x70},
    // 49 '1'
    {0x20,0x60,0x20,0x20,0x20,0x20,0x70},
    // 50 '2'
    {0x70,0x88,0x08,0x10,0x20,0x40,0xF8},
    // 51 '3'
    {0x70,0x88,0x08,0x30,0x08,0x88,0x70},
    // 52 '4'
    {0x10,0x30,0x50,0x90,0xF8,0x10,0x10},
    // 53 '5'
    {0xF8,0x80,0xF0,0x08,0x08,0x88,0x70},
    // 54 '6'
    {0x30,0x40,0x80,0xF0,0x88,0x88,0x70},
    // 55 '7'
    {0xF8,0x08,0x10,0x20,0x40,0x40,0x40},
    // 56 '8'
    {0x70,0x88,0x88,0x70,0x88,0x88,0x70},
    // 57 '9'
    {0x70,0x88,0x88,0x78,0x08,0x10,0x60},
    // 58 ':'
    {0x00,0x00,0x20,0x00,0x00,0x20,0x00},
    // 59 ';'
    {0x00,0x00,0x20,0x00,0x00,0x20,0x40},
    // 60 '<'
    {0x08,0x10,0x20,0x40,0x20,0x10,0x08},
    // 61 '='
    {0x00,0x00,0xF8,0x00,0xF8,0x00,0x00},
    // 62 '>'
    {0x80,0x40,0x20,0x10,0x20,0x40,0x80},
    // 63 '?'
    {0x70,0x88,0x08,0x10,0x20,0x00,0x20},
    // 64 '@'
    {0x70,0x88,0xB8,0xB8,0x80,0x88,0x70},
    // 65 'A'
    {0x70,0x88,0x88,0xF8,0x88,0x88,0x88},
    // 66 'B'
    {0xF0,0x88,0x88,0xF0,0x88,0x88,0xF0},
    // 67 'C'
    {0x70,0x88,0x80,0x80,0x80,0x88,0x70},
    // 68 'D'
    {0xF0,0x88,0x88,0x88,0x88,0x88,0xF0},
    // 69 'E'
    {0xF8,0x80,0x80,0xF0,0x80,0x80,0xF8},
    // 70 'F'
    {0xF8,0x80,0x80,0xF0,0x80,0x80,0x80},
    // 71 'G'
    {0x70,0x88,0x80,0xB8,0x88,0x88,0x70},
    // 72 'H'
    {0x88,0x88,0x88,0xF8,0x88,0x88,0x88},
    // 73 'I'
    {0x70,0x20,0x20,0x20,0x20,0x20,0x70},
    // 74 'J'
    {0x08,0x08,0x08,0x08,0x08,0x88,0x70},
    // 75 'K'
    {0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88},
    // 76 'L'
    {0x80,0x80,0x80,0x80,0x80,0x80,0xF8},
    // 77 'M'
    {0x88,0xD8,0xA8,0xA8,0x88,0x88,0x88},
    // 78 'N'
    {0x88,0xC8,0xA8,0x98,0x88,0x88,0x88},
    // 79 'O'
    {0x70,0x88,0x88,0x88,0x88,0x88,0x70},
    // 80 'P'
    {0xF0,0x88,0x88,0xF0,0x80,0x80,0x80},
    // 81 'Q'
    {0x70,0x88,0x88,0x88,0xA8,0x90,0x68},
    // 82 'R'
    {0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88},
    // 83 'S'
    {0x70,0x88,0x80,0x70,0x08,0x88,0x70},
    // 84 'T'
    {0xF8,0x20,0x20,0x20,0x20,0x20,0x20},
    // 85 'U'
    {0x88,0x88,0x88,0x88,0x88,0x88,0x70},
    // 86 'V'
    {0x88,0x88,0x88,0x88,0x50,0x50,0x20},
    // 87 'W'
    {0x88,0x88,0x88,0xA8,0xA8,0xD8,0x88},
    // 88 'X'
    {0x88,0x88,0x50,0x20,0x50,0x88,0x88},
    // 89 'Y'
    {0x88,0x88,0x50,0x20,0x20,0x20,0x20},
    // 90 'Z'
    {0xF8,0x08,0x10,0x20,0x40,0x80,0xF8},
    // 91 '['
    {0x70,0x40,0x40,0x40,0x40,0x40,0x70},
    // 92 '\'
    {0x00,0x80,0x40,0x20,0x10,0x08,0x00},
    // 93 ']'
    {0x70,0x10,0x10,0x10,0x10,0x10,0x70},
    // 94 '^'
    {0x20,0x50,0x88,0x00,0x00,0x00,0x00},
    // 95 '_'
    {0x00,0x00,0x00,0x00,0x00,0x00,0xF8},
    // 96 '`'
    {0x40,0x20,0x00,0x00,0x00,0x00,0x00},
    // 97 'a'
    {0x00,0x00,0x70,0x08,0x78,0x88,0x78},
    // 98 'b'
    {0x80,0x80,0xF0,0x88,0x88,0x88,0xF0},
    // 99 'c'
    {0x00,0x00,0x70,0x80,0x80,0x88,0x70},
    // 100 'd'
    {0x08,0x08,0x78,0x88,0x88,0x88,0x78},
    // 101 'e'
    {0x00,0x00,0x70,0x88,0xF8,0x80,0x70},
    // 102 'f'
    {0x30,0x48,0x40,0xF0,0x40,0x40,0x40},
    // 103 'g'
    {0x00,0x00,0x78,0x88,0x78,0x08,0x70},
    // 104 'h'
    {0x80,0x80,0xF0,0x88,0x88,0x88,0x88},
    // 105 'i'
    {0x20,0x00,0x60,0x20,0x20,0x20,0x70},
    // 106 'j'
    {0x08,0x00,0x18,0x08,0x08,0x88,0x70},
    // 107 'k'
    {0x80,0x80,0x90,0xA0,0xC0,0xA0,0x90},
    // 108 'l'
    {0x60,0x20,0x20,0x20,0x20,0x20,0x70},
    // 109 'm'
    {0x00,0x00,0xD0,0xA8,0xA8,0xA8,0x88},
    // 110 'n'
    {0x00,0x00,0xF0,0x88,0x88,0x88,0x88},
    // 111 'o'
    {0x00,0x00,0x70,0x88,0x88,0x88,0x70},
    // 112 'p'
    {0x00,0x00,0xF0,0x88,0xF0,0x80,0x80},
    // 113 'q'
    {0x00,0x00,0x78,0x88,0x78,0x08,0x08},
    // 114 'r'
    {0x00,0x00,0xB0,0xC8,0x80,0x80,0x80},
    // 115 's'
    {0x00,0x00,0x78,0x80,0x70,0x08,0xF0},
    // 116 't'
    {0x40,0x40,0xF0,0x40,0x40,0x48,0x30},
    // 117 'u'
    {0x00,0x00,0x88,0x88,0x88,0x88,0x78},
    // 118 'v'
    {0x00,0x00,0x88,0x88,0x88,0x50,0x20},
    // 119 'w'
    {0x00,0x00,0x88,0xA8,0xA8,0xA8,0x50},
    // 120 'x'
    {0x00,0x00,0x88,0x50,0x20,0x50,0x88},
    // 121 'y'
    {0x00,0x00,0x88,0x88,0x78,0x08,0x70},
    // 122 'z'
    {0x00,0x00,0xF8,0x10,0x20,0x40,0xF8},
    // 123 '{'
    {0x10,0x20,0x20,0x40,0x20,0x20,0x10},
    // 124 '|'
    {0x20,0x20,0x20,0x20,0x20,0x20,0x20},
    // 125 '}'
    {0x40,0x20,0x20,0x10,0x20,0x20,0x40},
    // 126 '~'
    {0x00,0x40,0xA8,0x10,0x00,0x00,0x00},
};
// clang-format on

static constexpr int kFontW = 5;
static constexpr int kFontH = 7;
static constexpr int kFontFirst = 32;
static constexpr int kFontLast = 126;

/// Draw a single character at (x, y) scaled by `scale`
static void draw_char(LabelBitmap& bmp, char c, int x, int y, int scale) {
    int idx = static_cast<int>(static_cast<unsigned char>(c)) - kFontFirst;
    if (idx < 0 || idx > (kFontLast - kFontFirst))
        idx = 0; // fallback to space

    const uint8_t* glyph = kFont5x7[idx];
    for (int row = 0; row < kFontH; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < kFontW; col++) {
            if (bits & (0x80 >> col)) {
                // Draw a scale×scale block
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        bmp.set_pixel(x + col * scale + sx, y + row * scale + sy, true);
            }
        }
    }
}

/// Draw a string at (x, y) with given scale (char pitch = 6*scale)
static void draw_text(LabelBitmap& bmp, const std::string& text, int x, int y, int scale) {
    int cursor_x = x;
    int char_pitch = (kFontW + 1) * scale; // 1px gap between chars
    for (char c : text) {
        draw_char(bmp, c, cursor_x, y, scale);
        cursor_x += char_pitch;
    }
}

/// Truncate text to fit within max_width pixels at given scale
static std::string truncate_to_fit(const std::string& text, int max_width, int scale) {
    int char_pitch = (kFontW + 1) * scale;
    int max_chars = max_width / char_pitch;
    if (max_chars <= 0)
        return {};
    if (static_cast<int>(text.length()) <= max_chars)
        return text;
    if (max_chars <= 2)
        return text.substr(0, max_chars);
    return text.substr(0, max_chars - 2) + "..";
}

/// Pixel width of `text` drawn at `scale` (last glyph has no trailing gap)
static int text_width(const std::string& text, int scale) {
    if (text.empty())
        return 0;
    return static_cast<int>(text.length()) * (kFontW + 1) * scale - scale;
}

/// Human-readable spool identifier. It is what a person reads off the label
/// when picking a spool from a rack, so every layout prints it at least as
/// large as the vendor line and never lets another field truncate it.
/// Tracked spools print "#n"; the negative-id preview label prints "TEST";
/// untracked spools (id == 0) have nothing to print and get an empty string.
static std::string spool_id_text(const SpoolInfo& spool) {
    if (spool.id < 0)
        return "TEST";
    if (spool.id == 0)
        return {};
    return "#" + std::to_string(spool.id);
}

// Spool-number size bounds for the MINIMAL layout, in font scale units.
// The floor keeps the number legible; the cap stops a short number from
// dwarfing the QR on a wide label.
static constexpr int kIdMinScale = 3;
static constexpr int kIdMaxScale = 12;

/// Largest scale at which `text` fits inside a w x h box, capped at kIdMaxScale
static int fit_scale(const std::string& text, int w, int h) {
    int by_w = w / (static_cast<int>(text.length()) * (kFontW + 1));
    int by_h = h / kFontH;
    return std::min({by_w, by_h, kIdMaxScale});
}

/// MINIMAL layout on a canvas_w x canvas_h canvas (canvas_h == 0: continuous,
/// the height follows the content). The QR is paired with the spool number so
/// a person can identify the spool without scanning it; id_text is never
/// empty here because an untracked spool falls back to COMPACT first. The
/// number goes beside the QR unless placing it below yields a larger number;
/// on a die-cut label too small for either, the QR shrinks to make room below.
static LabelBitmap render_minimal(const std::string& qr_data, const std::string& id_text,
                                  int canvas_w, int canvas_h, int qr_margin, int qr_max, int gap,
                                  int spool_id) {
    auto qr = generate_qr_bitmap(qr_data, qr_max);
    if (qr.empty()) {
        spdlog::warn("label_renderer: QR generation failed for spool {}", spool_id);
        return LabelBitmap::create(canvas_w, qr_margin * 2);
    }

    const int inner_w = canvas_w - 2 * qr_margin;
    const int inner_h = canvas_h > 0 ? canvas_h - 2 * qr_margin : 0;

    int scale_beside = fit_scale(id_text, inner_w - qr.width() - gap, qr.height());
    int scale_below = fit_scale(id_text, inner_w,
                                canvas_h > 0 ? inner_h - qr.height() - gap : kIdMaxScale * kFontH);
    bool beside = scale_beside >= kIdMinScale && scale_beside >= scale_below;

    if (!beside && scale_below < kIdMinScale) {
        // Die-cut label with no room for the number: give up QR pixels, not
        // the number. Continuous tape never lands here (its height is free).
        int shrunk = inner_h - gap - kFontH * kIdMinScale;
        auto smaller = generate_qr_bitmap(qr_data, std::max(shrunk, 1));
        if (!smaller.empty())
            qr = std::move(smaller);
        scale_below = fit_scale(id_text, inner_w, inner_h - qr.height() - gap);
        scale_below = std::max(scale_below, 1);
    }

    int scale = beside ? scale_beside : scale_below;
    int id_w = text_width(id_text, scale);
    int id_h = kFontH * scale;

    if (beside) {
        int height = canvas_h > 0 ? canvas_h : qr.height() + 2 * qr_margin;
        auto label = LabelBitmap::create(canvas_w, height);
        int group_w = qr.width() + gap + id_w;
        int x0 = (canvas_w - group_w) / 2;
        int qr_y = (height - qr.height()) / 2;
        label.blit(qr, x0, qr_y);
        draw_text(label, id_text, x0 + qr.width() + gap, qr_y + (qr.height() - id_h) / 2, scale);
        return label;
    }

    int group_h = qr.height() + gap + id_h;
    int height = canvas_h > 0 ? canvas_h : group_h + 2 * qr_margin;
    auto label = LabelBitmap::create(canvas_w, height);
    int y0 = (height - group_h) / 2;
    label.blit(qr, (canvas_w - qr.width()) / 2, y0);
    draw_text(label, id_text, (canvas_w - id_w) / 2, y0 + qr.height() + gap, scale);
    return label;
}

LabelBitmap LabelRenderer::render(const SpoolInfo& spool, LabelPreset preset,
                                  const LabelSize& size) {
    int margin = 20;
    int label_width = size.width_px;

    // Untracked spools carry id == 0: there is no Spoolman record to link to,
    // so both the QR code and the "#n" text line are omitted and the remaining
    // content reflows into the freed space. Vendor/material/color/weight are
    // still meaningful and render unchanged. Negative IDs (preview/test labels)
    // keep their QR.
    const bool has_id = spool.id != 0;

    // QR data: Spoolman URI. Negative IDs are reserved for preview/test
    // labels — emit a payload the decoder rejects so an accidental scan of
    // the printed test label doesn't swap the active spool.
    std::string qr_data = (spool.id < 0) ? std::string{"web+spoolman:test"}
                                         : "web+spoolman:s-" + std::to_string(spool.id);

    // MINIMAL is QR + spool number and nothing else — without an ID it would
    // print a blank label, so fall back to COMPACT, which still carries the
    // identifying text.
    if (preset == LabelPreset::MINIMAL && !has_id) {
        spdlog::debug("label_renderer: untracked spool has no QR payload — rendering COMPACT "
                      "instead of MINIMAL");
        preset = LabelPreset::COMPACT;
    }

    const std::string id_text = spool_id_text(spool);

    // --- MINIMAL: QR plus the spool number, nothing else ---
    if (preset == LabelPreset::MINIMAL) {
        // No extra margin for die-cut labels (QR spec includes its own quiet zone).
        // Use small margin for continuous tape to avoid printing to the edge.
        int qr_margin = (size.height_px > 0) ? 0 : std::min(margin, label_width / 20);

        // Cap QR size to fill most of the label
        int qr_max = std::min(label_width, 250);
        if (size.height_px > 0)
            qr_max = std::min(qr_max, size.height_px - 2 * qr_margin);

        // Narrow labels (< 150px wide) have the printhead along the short edge:
        // compose in landscape (QR left, number right) and rotate 90° CW, the
        // same way the text layouts below handle them.
        if (label_width < 150) {
            int lw = size.height_px > 0 ? size.height_px : 307;
            auto landscape =
                render_minimal(qr_data, id_text, lw, label_width, qr_margin, qr_max, 8, spool.id);
            return landscape.rotate_90_cw();
        }
        return render_minimal(qr_data, id_text, label_width, size.height_px, qr_margin, qr_max, 16,
                              spool.id);
    }

    // Build text content
    std::string vendor = helix::text_io::to_upper(spool.vendor.empty() ? "UNKNOWN" : spool.vendor);
    std::string material =
        helix::text_io::to_upper(spool.material.empty() ? "FILAMENT" : spool.material);
    // Behaviour preserved across the color_name -> filament_name rename: this
    // line has always rendered Spoolman's filament.name, which is the most
    // specific string on the record and what a user scans the label for.
    std::string color = helix::text_io::to_upper(spool.filament_name);

    // --- NARROW labels (< 150px wide): render landscape, rotate 90° CW ---
    // Narrow labels like Niimbot D110 (96px wide × 307px tall) have the
    // printhead along the short edge. Render content in landscape orientation
    // (height_px wide × width_px tall) with QR left + text right, then rotate
    // 90° CW so the final bitmap is width_px × height_px for the printer.
    if (label_width < 150) {
        int m = 6; // margin
        int gap = 8;

        // Landscape dimensions: long edge is width, short edge is height
        int lw = size.height_px > 0 ? size.height_px : 307; // landscape width
        int lh = label_width;                               // landscape height (96px for D110)

        // QR: fit to landscape height minus margins. Untracked spools skip it
        // and the text block claims the whole landscape width.
        int qr_target = lh - 2 * m;
        LabelBitmap qr;
        if (has_id) {
            qr = generate_qr_bitmap(qr_data, qr_target);
            if (qr.empty()) {
                spdlog::warn("label_renderer: QR generation failed for spool {}", spool.id);
                return LabelBitmap::create(label_width, lw);
            }
        }

        // Font scale: 96px height gives room for scale 2 (14px line height)
        int scale = 2;
        int line_h = kFontH * scale + 2;
        int text_x = has_id ? m + qr.width() + gap : m;
        int text_area_width = lw - text_x - m;

        auto label = LabelBitmap::create(lw, lh);

        // QR: left side, vertically centered
        if (has_id) {
            int qr_x = m;
            int qr_y = (lh - qr.height()) / 2;
            label.blit(qr, qr_x, qr_y);
        }

        // Text: right side of QR, slightly lower for visual balance
        int text_y = m + 4;

        // Line 1: Spool number (omitted for untracked spools)
        if (!id_text.empty()) {
            draw_text(label, truncate_to_fit(id_text, text_area_width, scale), text_x, text_y,
                      scale);
            text_y += line_h;
        }

        // Line 2: Vendor
        draw_text(label, truncate_to_fit(vendor, text_area_width, scale), text_x, text_y, scale);
        text_y += line_h;

        // Line 3: Material + Color
        std::string line2 = color.empty() ? material : material + " " + color;
        draw_text(label, truncate_to_fit(line2, text_area_width, scale), text_x, text_y, scale);
        text_y += line_h;

        // Line 4: Weight
        std::string weight_str = std::to_string(static_cast<int>(spool.remaining_weight_g)) + "G";
        draw_text(label, truncate_to_fit(weight_str, text_area_width, scale), text_x, text_y,
                  scale);
        text_y += line_h;

        // Line 5: Temps (if fits)
        if (spool.nozzle_temp_recommended > 0 && text_y + line_h <= lh - m) {
            std::string temps = std::to_string(spool.nozzle_temp_recommended) + "C";
            if (spool.bed_temp_recommended > 0)
                temps += "/" + std::to_string(spool.bed_temp_recommended) + "C";
            draw_text(label, truncate_to_fit(temps, text_area_width, scale), text_x, text_y, scale);
        }

        // Rotate 90° CW: landscape (lw × lh) → portrait (lh × lw) for printer
        return label.rotate_90_cw();
    }

    // --- STANDARD / COMPACT: QR left, text right (wide labels) ---
    // Narrow labels (≤62mm tape, ≤500px) get tighter margins and a smaller
    // QR to maximize text area — applies to both die-cut AND narrow continuous
    // tape (29mm/38mm Brother QL), where horizontal width is the tight axis.
    bool narrow = label_width < 500;
    if (narrow) {
        margin = 12;
    }

    int qr_pct = (preset == LabelPreset::STANDARD) ? 40 : 30;
    int qr_max = (preset == LabelPreset::STANDARD) ? 250 : 200;
    if (narrow) {
        qr_pct -= 8; // shrink QR on narrow labels (die-cut or continuous)
        qr_max = std::min(qr_max, 180);
    }
    int qr_target = std::min(label_width * qr_pct / 100, qr_max);
    LabelBitmap qr;
    if (has_id) {
        qr = generate_qr_bitmap(qr_data, qr_target);
        if (qr.empty()) {
            spdlog::warn("label_renderer: QR generation failed for spool {}", spool.id);
            return LabelBitmap::create(label_width, margin * 2);
        }
    }
    const int qr_h = has_id ? qr.height() : 0;

    int gap = narrow ? 10 : 16;
    int text_x = has_id ? margin + qr.width() + gap : margin;
    int text_area_width = label_width - text_x - margin;

    // Scale selection based on label width AND height to fill available space.
    // Continuous tape has no fixed length — use a tighter target than die-cut so
    // fonts stay smaller and more characters fit horizontally. 260px @ 300dpi
    // gives scale_lg=5/md=4/sm=3 for STANDARD continuous (was 6/5/4 with the
    // 300px default — too tall and wasted horizontal width on 62mm tape).
    int avail_h = size.height_px > 0 ? size.height_px : 260;
    int scale_lg, scale_md, scale_sm;
    if (preset == LabelPreset::COMPACT) {
        // Compact: 3 lines. Scale to fill ~80% of height.
        // 3 lines at (kFontH * scale + 4) each → scale = (avail_h * 0.8 / 3 - 4) / kFontH
        int target_scale = (avail_h * 8 / 10 / 3 - 4) / kFontH;
        scale_lg = std::clamp(target_scale, 3, 8);
        scale_md = std::clamp(target_scale - 1, 2, 7);
        scale_sm = std::clamp(target_scale - 2, 2, 6);
    } else {
        // Standard: 3-6 lines. Scale to fill ~70% of height with 4 lines.
        int target_scale = (avail_h * 7 / 10 / 4 - 4) / kFontH;
        scale_lg = std::clamp(target_scale, 3, 7);
        scale_md = std::clamp(target_scale - 1, 2, 6);
        scale_sm = std::clamp(target_scale - 2, 2, 5);
    }
    // Also cap by text area width (don't let chars overflow horizontally).
    // Floor of 8 chars: vendor/material strings ("PRUSAMENT", "POLYMAKER") are
    // typically 7-9 chars — anything less truncates them to garble on narrow
    // tape. Was 4, which left 29mm continuous showing only "PRUS".
    int max_scale_by_width = text_area_width / ((kFontW + 1) * 8); // fit ≥8 chars
    if (max_scale_by_width < 2)
        max_scale_by_width = 2;
    scale_lg = std::min(scale_lg, max_scale_by_width);
    scale_md = std::min(scale_md, max_scale_by_width);
    scale_sm = std::min(scale_sm, max_scale_by_width);

    int line_h_lg = kFontH * scale_lg + 4;
    int line_h_md = kFontH * scale_md + 4;
    int line_h_sm = kFontH * scale_sm + 4;

    // Calculate text block height. The spool number leads at the vendor
    // scale; untracked spools drop that line entirely so the remaining lines
    // stay centered rather than leaving a blank first line.
    const int line_h_id = has_id ? line_h_lg : 0;
    int text_height;
    if (preset == LabelPreset::STANDARD) {
        text_height = line_h_id + line_h_lg + line_h_md + line_h_sm;
        // Extra lines (temps, lot, comment) if data is present and, on a
        // fixed-height label, if there is room. Counting a line that cannot
        // be drawn would push the block off the top edge.
        auto extra_fits = [&](int h) {
            return size.height_px <= 0 || h + line_h_sm <= size.height_px - 2 * margin;
        };
        if (spool.nozzle_temp_recommended > 0 && extra_fits(text_height))
            text_height += line_h_sm;
        if (!spool.lot_nr.empty() && extra_fits(text_height))
            text_height += line_h_sm;
        if (!spool.comment.empty() && extra_fits(text_height))
            text_height += line_h_sm;
    } else {
        // COMPACT: spool number + vendor + material/color.
        text_height = line_h_id + line_h_lg + line_h_md;
    }

    int content_height = std::max(qr_h, text_height);
    int height = size.height_px > 0 ? size.height_px : content_height + 2 * margin;

    auto label = LabelBitmap::create(label_width, height);

    // QR: vertically centered
    if (has_id) {
        label.blit(qr, margin, (height - qr.height()) / 2);
    }

    // Text: vertically centered
    int text_y = (height - text_height) / 2;

    // Line 1 (both layouts): Spool number, at the vendor scale so it reads
    // from across the room. Omitted for untracked spools.
    if (has_id) {
        draw_text(label, truncate_to_fit(id_text, text_area_width, scale_lg), text_x, text_y,
                  scale_lg);
        text_y += line_h_lg;
    }

    if (preset == LabelPreset::STANDARD) {
        // Line 2: Vendor (large)
        draw_text(label, truncate_to_fit(vendor, text_area_width, scale_lg), text_x, text_y,
                  scale_lg);
        text_y += line_h_lg;

        // Line 3: Material + Color (medium)
        std::string line2 = color.empty() ? material : material + " " + color;
        draw_text(label, truncate_to_fit(line2, text_area_width, scale_md), text_x, text_y,
                  scale_md);
        text_y += line_h_md;

        // Line 4: Weight + Length (small). On its own line so a narrow column
        // truncates the weight, never the spool number.
        std::string weight_str = std::to_string(static_cast<int>(spool.remaining_weight_g)) + "G";
        if (spool.remaining_length_m > 0) {
            weight_str += " / " + std::to_string(static_cast<int>(spool.remaining_length_m)) + "M";
        }
        draw_text(label, truncate_to_fit(weight_str, text_area_width, scale_sm), text_x, text_y,
                  scale_sm);
        text_y += line_h_sm;

        // Line 5: Temps (small) — if present and space permits
        if (spool.nozzle_temp_recommended > 0 && text_y + line_h_sm <= height - margin) {
            std::string temps = std::to_string(spool.nozzle_temp_recommended) + "C";
            if (spool.bed_temp_recommended > 0) {
                temps += " / " + std::to_string(spool.bed_temp_recommended) + "C BED";
            }
            draw_text(label, truncate_to_fit(temps, text_area_width, scale_sm), text_x, text_y,
                      scale_sm);
            text_y += line_h_sm;
        }

        // Line 6: Lot number (small) — if present and space permits
        if (!spool.lot_nr.empty() && text_y + line_h_sm <= height - margin) {
            draw_text(
                label,
                truncate_to_fit(helix::text_io::to_upper(spool.lot_nr), text_area_width, scale_sm),
                text_x, text_y, scale_sm);
            text_y += line_h_sm;
        }

        // Line 7: Comment/notes (small) — if present and space permits
        if (!spool.comment.empty() && text_y + line_h_sm <= height - margin) {
            draw_text(
                label,
                truncate_to_fit(helix::text_io::to_upper(spool.comment), text_area_width, scale_sm),
                text_x, text_y, scale_sm);
        }
    } else {
        // COMPACT: Line 2 = Vendor (large)
        draw_text(label, truncate_to_fit(vendor, text_area_width, scale_lg), text_x, text_y,
                  scale_lg);
        text_y += line_h_lg;

        // Line 3: Material + Color (medium)
        std::string line2 = color.empty() ? material : material + " " + color;
        draw_text(label, truncate_to_fit(line2, text_area_width, scale_md), text_x, text_y,
                  scale_md);
    }

    spdlog::debug("label_renderer: rendered {}x{} label for spool {} ({})", label.width(),
                  label.height(), spool.id, label_preset_name(preset));
    return label;
}

} // namespace helix

#endif // HELIX_HAS_LABEL_PRINTER
