// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "brother_ql_protocol.h"

#include <algorithm>

namespace helix::label {

std::vector<uint8_t> brother_ql_build_raster(const LabelBitmap& bitmap, const LabelSize& size) {
    std::vector<uint8_t> cmd;
    // Pre-allocate: header ~220 bytes + per-row worst case (3 + 90) * rows + footer
    cmd.reserve(256 + static_cast<size_t>(bitmap.height()) * (3 + kBrotherQlRasterRowBytes) + 1);

    // 1. Invalidate — 200 bytes of 0x00
    cmd.insert(cmd.end(), 200, 0x00);

    // 2. Initialize — ESC @
    cmd.push_back(0x1B);
    cmd.push_back(0x40);

    // 3. Switch to raster mode — ESC i a 01
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x61);
    cmd.push_back(0x01);

    // 4. Set media and quality — ESC i z
    int page_length = bitmap.height();
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x7A);
    // Validity flags: bit 7=PI, bit 2=width, bit 1=media_type, bit 3=length (die-cut only)
    uint8_t valid_flags = 0x80 | 0x04 | 0x02; // PI + width + media type
    if (size.media_type == 0x0B) {
        valid_flags |= 0x08; // Die-cut: mark length field as valid
    }
    cmd.push_back(valid_flags);
    cmd.push_back(size.media_type); // Media type
    cmd.push_back(size.width_mm);   // Width in mm
    cmd.push_back(size.length_mm);  // Length in mm
    // Page length as little-endian 32-bit
    cmd.push_back(static_cast<uint8_t>(page_length & 0xFF));
    cmd.push_back(static_cast<uint8_t>((page_length >> 8) & 0xFF));
    cmd.push_back(static_cast<uint8_t>((page_length >> 16) & 0xFF));
    cmd.push_back(static_cast<uint8_t>((page_length >> 24) & 0xFF));
    cmd.push_back(0x00); // Page number = 0
    cmd.push_back(0x00); // Auto-cut flag (set separately)

    // 5. Set auto-cut on — ESC i M
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x4D);
    cmd.push_back(0x40);

    // 5a. Set cut-every = 1 label — ESC i A
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x41);
    cmd.push_back(0x01);

    // 6. Expanded mode — ESC i K (auto-cut, no 2-color, 300dpi)
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x4B);
    cmd.push_back(0x08); // bit 3 = auto-cut

    // 7. Set margins = 0 (no margins for die-cut)
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x64);
    cmd.push_back(0x00);
    cmd.push_back(0x00);

    // 8. Disable compression — M 0x00
    cmd.push_back(0x4D);
    cmd.push_back(0x00);

    // 9. Raster data rows
    // Brother QL printhead requires horizontally flipped pixel data.
    // Data is left-justified in the 90-byte row. For die-cut labels that are
    // narrower than their backing tape, add an offset to center the data on
    // the physical label (e.g., 23mm label centered on 29mm backing).
    int label_byte_width = (size.width_px + 7) / 8;

    // Calculate offset for die-cut labels narrower than their backing tape.
    // The label's printable area is centered on the backing, offset ~3/4 of
    // the total margin from the guide rail edge (empirically tuned).
    int left_offset = 0;
    if (size.media_type == 0x0B) {
        // Die-cut backing widths: ≤29mm → 29mm (306 dots), ≤62mm → 62mm (696 dots)
        int backing_px = (size.width_mm <= 29) ? 306 : 696;
        if (backing_px > size.width_px) {
            int margin_dots = backing_px - size.width_px;
            left_offset = (margin_dots * 2 / 3 + 4) / 8;
        }
    }

    int right_pad = kBrotherQlRasterRowBytes - label_byte_width - left_offset;
    if (right_pad < 0)
        right_pad = 0;

    // Build a horizontally-flipped copy of each row
    std::vector<uint8_t> flipped_row(label_byte_width, 0x00);

    for (int y = 0; y < bitmap.height(); y++) {
        const uint8_t* row = bitmap.row_data(y);
        int row_bytes = bitmap.row_byte_width();

        // Horizontal flip: reverse pixel order within the row
        std::fill(flipped_row.begin(), flipped_row.end(), 0x00);
        for (int x = 0; x < size.width_px && x < bitmap.width(); x++) {
            int src_byte = x / 8;
            int src_bit = 7 - (x % 8);
            if (src_byte < row_bytes && (row[src_byte] & (1 << src_bit))) {
                int dst_x = size.width_px - 1 - x;
                int dst_byte = dst_x / 8;
                int dst_bit = 7 - (dst_x % 8);
                flipped_row[dst_byte] |= (1 << dst_bit);
            }
        }

        bool all_white = true;
        for (int b = 0; b < label_byte_width; b++) {
            if (flipped_row[b] != 0x00) {
                all_white = false;
                break;
            }
        }

        if (all_white) {
            // Blank line marker
            cmd.push_back(0x5A);
        } else {
            // Raster data: 0x67 0x00 [byte_count] [data...]
            cmd.push_back(0x67);
            cmd.push_back(0x00);
            cmd.push_back(static_cast<uint8_t>(kBrotherQlRasterRowBytes));

            // Left offset (centering for die-cut labels on wider backing)
            if (left_offset > 0) {
                cmd.insert(cmd.end(), left_offset, 0x00);
            }

            // Copy flipped pixel data
            cmd.insert(cmd.end(), flipped_row.begin(), flipped_row.end());

            // Right padding (unused printhead area beyond the label)
            cmd.insert(cmd.end(), right_pad, 0x00);
        }
    }

    // 10. Print command
    cmd.push_back(0x1A);

    return cmd;
}

std::vector<uint8_t> brother_ql_build_status_request() {
    std::vector<uint8_t> cmd;
    // 1. Invalidate — 200 bytes of 0x00
    cmd.insert(cmd.end(), 200, 0x00);
    // 2. Initialize — ESC @
    cmd.push_back(0x1B);
    cmd.push_back(0x40);
    // 3. Request status — ESC i S
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x53);
    return cmd;
}

BrotherQLMedia brother_ql_parse_status(const uint8_t* data, size_t len) {
    BrotherQLMedia media{};
    // Brother QL status response is 32 bytes
    if (!data || len < 32)
        return media;
    // Byte 0: print head mark (0x80)
    if (data[0] != 0x80)
        return media;
    // Byte 11: media type (0x0A continuous, 0x0B die-cut)
    media.media_type = data[11];
    // Byte 10: media width in mm
    media.width_mm = data[10];
    // Byte 17: media length in mm (0 for continuous)
    media.length_mm = data[17];
    media.valid = (media.width_mm > 0);
    return media;
}

const LabelSize* brother_ql_match_media(const BrotherQLMedia& media,
                                        const std::vector<LabelSize>& sizes) {
    if (!media.valid)
        return nullptr;
    for (const auto& s : sizes) {
        if (s.media_type == media.media_type && s.width_mm == media.width_mm &&
            s.length_mm == media.length_mm) {
            return &s;
        }
    }
    // No exact match — try matching just width for continuous tape
    if (media.media_type == 0x0A) {
        for (const auto& s : sizes) {
            if (s.media_type == 0x0A && s.width_mm == media.width_mm)
                return &s;
        }
    }
    return nullptr;
}

} // namespace helix::label

#endif // HELIX_HAS_LABEL_PRINTER
