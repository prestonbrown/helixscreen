// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "brother_pt_protocol.h"

#include "lvgl.h"

#include <algorithm>
#include <array>

namespace helix::label {

static constexpr std::array<BrotherPTTapeInfo, 6> kTapeTable = {{
    {4, 24, 52},  // 3.5mm (status byte = 4)
    {6, 32, 48},  // 6mm
    {9, 50, 39},  // 9mm
    {12, 70, 29}, // 12mm
    {18, 112, 8}, // 18mm
    {24, 128, 0}, // 24mm
}};

const BrotherPTTapeInfo* brother_pt_get_tape_info(int width_mm) {
    for (const auto& t : kTapeTable) {
        if (t.width_mm == width_mm)
            return &t;
    }
    return nullptr;
}

std::optional<LabelSize> brother_pt_label_size_for_tape(int width_mm) {
    const auto* info = brother_pt_get_tape_info(width_mm);
    if (!info)
        return std::nullopt;
    std::string name = (width_mm == 4) ? "3.5mm" : std::to_string(width_mm) + "mm";
    return LabelSize{name,
                     info->printable_pins,
                     0,
                     180,
                     0x01, // media_type: laminated TZe (default)
                     static_cast<uint8_t>(width_mm),
                     0};
}

std::vector<uint8_t> brother_pt_build_status_request() {
    std::vector<uint8_t> cmd;
    // 1. Invalidate — 100 bytes of 0x00 (PT uses fewer than QL's 200)
    cmd.insert(cmd.end(), 100, 0x00);
    // 2. Initialize — ESC @
    cmd.push_back(0x1B);
    cmd.push_back(0x40);
    // 3. Request status — ESC i S
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x53);
    return cmd;
}

BrotherPTMedia brother_pt_parse_status(const uint8_t* data, size_t len) {
    BrotherPTMedia media{};
    if (!data || len < 32)
        return media;
    if (data[0] != 0x80)
        return media;
    media.error_info_1 = data[8];
    media.error_info_2 = data[9];
    media.width_mm = data[10];
    media.media_type = data[11];
    media.status_type = data[18];
    media.valid = true;
    return media;
}

std::string brother_pt_error_string(const BrotherPTMedia& media) {
    if (media.error_info_1 & 0x01)
        return lv_tr("No media installed");
    if (media.error_info_1 & 0x04)
        return lv_tr("Cutter jam");
    if (media.error_info_1 & 0x08)
        return lv_tr("Weak battery");
    if (media.error_info_2 & 0x01)
        return lv_tr("Wrong media");
    if (media.error_info_2 & 0x10)
        return lv_tr("Cover open");
    if (media.error_info_2 & 0x20)
        return lv_tr("Overheating");
    return "";
}

std::vector<uint8_t> brother_pt_packbits_compress(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < len) {
        // Count repeated bytes
        size_t run = 1;
        while (i + run < len && run < 128 && data[i + run] == data[i])
            run++;

        if (run >= 2) {
            // Repeat run: control = -(run-1), then single byte
            out.push_back(static_cast<uint8_t>(-(static_cast<int>(run) - 1)));
            out.push_back(data[i]);
            i += run;
        } else {
            // Literal run: collect non-repeating bytes
            size_t lit_start = i;
            size_t lit_len = 1;
            i++;
            while (i < len && lit_len < 128) {
                if (i + 1 < len && data[i] == data[i + 1])
                    break;
                lit_len++;
                i++;
            }
            out.push_back(static_cast<uint8_t>(lit_len - 1));
            out.insert(out.end(), data + lit_start, data + lit_start + lit_len);
        }
    }
    return out;
}

std::vector<uint8_t> brother_pt_build_raster(const LabelBitmap& bitmap, int tape_width_mm) {
    const auto* tape = brother_pt_get_tape_info(tape_width_mm);
    if (!tape)
        return {};

    // No rotation needed — the renderer produces a bitmap with width = tape printable
    // width and height = label length. Each row of the bitmap maps to one raster line
    // across the tape, and the number of rows determines the label length.
    int bmp_w = bitmap.width();
    int bmp_h = bitmap.height();

    std::vector<uint8_t> cmd;
    cmd.reserve(256 + static_cast<size_t>(bmp_h) * (3 + kBrotherPtRasterRowBytes) + 1);

    // 1. Invalidate — 100 bytes of 0x00
    cmd.insert(cmd.end(), 100, 0x00);

    // 2. Initialize — ESC @
    cmd.push_back(0x1B);
    cmd.push_back(0x40);

    // 3. Enter raster mode — ESC i a 01
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x61);
    cmd.push_back(0x01);

    // 4. Print info — ESC i z
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x7A);
    cmd.push_back(0x86); // valid flags: media type + width + quality
    cmd.push_back(0x01); // media type: laminated TZe
    cmd.push_back(static_cast<uint8_t>(tape_width_mm));
    cmd.push_back(0x00); // length (continuous)
    // Raster line count (LE 32-bit)
    cmd.push_back(static_cast<uint8_t>(bmp_h & 0xFF));
    cmd.push_back(static_cast<uint8_t>((bmp_h >> 8) & 0xFF));
    cmd.push_back(static_cast<uint8_t>((bmp_h >> 16) & 0xFF));
    cmd.push_back(static_cast<uint8_t>((bmp_h >> 24) & 0xFF));
    cmd.push_back(0x00); // page number
    cmd.push_back(0x00); // reserved

    // 5. Auto-cut — ESC i M (0x00 = no auto-cut, PT-P300BT has manual cut only)
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x4D);
    cmd.push_back(0x00);

    // 6. Advanced mode — ESC i K (no chain printing)
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x4B);
    cmd.push_back(0x08);

    // 7. Margin — ESC i d (50 dots ≈ 7mm at 180dpi)
    // Trailing feed to ensure content clears the print head and reaches the
    // manual cutter. Too little = content clipped, too much = tape waste.
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x64);
    cmd.push_back(50);
    cmd.push_back(0x00);

    // 8. Compression — M 02 (TIFF PackBits)
    cmd.push_back(0x4D);
    cmd.push_back(0x02);

    // 9. Raster rows — pin-precise margin centering
    // Each bitmap row (70 pixels for 12mm) is centered in the 128-pin print head
    std::vector<uint8_t> padded_row(kBrotherPtRasterRowBytes, 0x00);
    int margin_pins = tape->left_margin_pins;

    for (int y = 0; y < bmp_h; y++) {
        const uint8_t* row = bitmap.row_data(y);
        int row_bytes = bitmap.row_byte_width();

        std::fill(padded_row.begin(), padded_row.end(), 0x00);

        // Horizontal flip the printable portion with pin-precise margin offset
        for (int x = 0; x < tape->printable_pins && x < bmp_w; x++) {
            int src_byte = x / 8;
            int src_bit = 7 - (x % 8);
            if (src_byte < row_bytes && (row[src_byte] & (1 << src_bit))) {
                int dst_x = tape->printable_pins - 1 - x;
                int dst_pin = margin_pins + dst_x;
                int dst_byte = dst_pin / 8;
                int dst_bit = 7 - (dst_pin % 8);
                if (dst_byte < kBrotherPtRasterRowBytes)
                    padded_row[dst_byte] |= (1 << dst_bit);
            }
        }

        // Check if all zeros
        bool all_white = true;
        for (int b = 0; b < kBrotherPtRasterRowBytes; b++) {
            if (padded_row[b] != 0x00) {
                all_white = false;
                break;
            }
        }

        if (all_white) {
            cmd.push_back(0x5A);
        } else {
            auto compressed =
                brother_pt_packbits_compress(padded_row.data(), kBrotherPtRasterRowBytes);
            cmd.push_back(0x47);
            cmd.push_back(static_cast<uint8_t>(compressed.size() & 0xFF));
            cmd.push_back(static_cast<uint8_t>((compressed.size() >> 8) & 0xFF));
            cmd.insert(cmd.end(), compressed.begin(), compressed.end());
        }
    }

    // 10. Print with minimal feed
    // 0x1A = print + full feed (wastes tape), 0x0C = print page (minimal feed)
    cmd.push_back(0x0C);

    return cmd;
}

} // namespace helix::label

#endif // HELIX_HAS_LABEL_PRINTER
