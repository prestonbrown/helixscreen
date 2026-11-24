// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "makeid_protocol.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace helix::label {

uint8_t makeid_checksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return static_cast<uint8_t>(-sum);
}

std::vector<uint8_t> makeid_build_handshake(MakeIdHandshakeState state) {
    // Frame: [0x66, LEN_LO, LEN_HI, CMD, STATE, CHECKSUM]
    // Total length = 6
    std::vector<uint8_t> frame;
    frame.reserve(6);

    frame.push_back(0x66); // magic
    frame.push_back(0x06); // length low (6 bytes total)
    frame.push_back(0x00); // length high
    frame.push_back(static_cast<uint8_t>(MakeIdCmd::Handshake));
    frame.push_back(static_cast<uint8_t>(state));

    // Checksum over bytes [0..4]
    uint8_t chk = makeid_checksum(frame.data(), frame.size());
    frame.push_back(chk);

    return frame;
}

MakeIdResponse makeid_parse_response(const uint8_t* data, size_t len) {
    MakeIdResponse resp;

    if (len < 36) {
        // Short response — check for null marker
        if (len >= 6 && data[3] == 0x11) {
            resp.status = MakeIdResponseStatus::Null;
            return resp;
        }
        resp.status = MakeIdResponseStatus::Resend;
        return resp;
    }

    // Extract error code from bits 0-5 of byte 4
    uint8_t error_code = data[4] & 0x3F;
    resp.error_code = error_code;

    // Determine status with priority: Error > Wait > Resend > Pause > Exit > Success
    // Error code 23 means "no error" in the Wewin protocol (same as 0)
    bool has_error = (error_code != 0 && error_code != 23);
    bool has_wait = (data[4] & 0x80) != 0;
    bool has_resend = (data[4] & 0x40) != 0;

    // Byte 35: bits 5-6 encode state, bit 7 = isPrinting
    uint8_t state_bits = (data[35] >> 5) & 0x03;
    resp.is_printing = (data[35] & 0x80) != 0;

    if (has_error) {
        resp.status = MakeIdResponseStatus::Error;
    } else if (has_wait) {
        resp.status = MakeIdResponseStatus::Wait;
    } else if (has_resend) {
        resp.status = MakeIdResponseStatus::Resend;
    } else if (state_bits == 1) {
        resp.status = MakeIdResponseStatus::Pause;
    } else if (state_bits == 3) {
        resp.status = MakeIdResponseStatus::Exit;
    } else {
        resp.status = MakeIdResponseStatus::Success;
    }

    return resp;
}

std::vector<uint8_t> makeid_build_print_frame(const std::vector<uint8_t>& compressed_data,
                                              const MakeIdPrintParams& params) {
    // Header: 17 bytes + compressed_data + 1 checksum byte
    // Total = 18 + compressed_data.size()
    size_t total_len = 17 + compressed_data.size() + 1;

    std::vector<uint8_t> frame;
    frame.reserve(total_len);

    // Byte 0: magic
    frame.push_back(0x66);

    // Bytes 1-2: total frame length (little-endian)
    frame.push_back(static_cast<uint8_t>(total_len & 0xFF));
    frame.push_back(static_cast<uint8_t>((total_len >> 8) & 0xFF));

    // Byte 3: command
    frame.push_back(static_cast<uint8_t>(MakeIdCmd::PrintData));

    // Byte 4: darkness (bits 0-4) | label_type (bits 5-7)
    frame.push_back((params.darkness & 0x1F) | static_cast<uint8_t>(params.label_type));

    // Byte 5: cut_type (bits 0-2) | save_type (bits 3-4)
    frame.push_back((params.cut_type & 0x07) | ((params.save_type & 0x03) << 3));

    // Bytes 6-7: total copies (little-endian)
    frame.push_back(static_cast<uint8_t>(params.total_copies & 0xFF));
    frame.push_back(static_cast<uint8_t>((params.total_copies >> 8) & 0xFF));

    // Bytes 8-9: current copy (little-endian)
    frame.push_back(static_cast<uint8_t>(params.current_copy & 0xFF));
    frame.push_back(static_cast<uint8_t>((params.current_copy >> 8) & 0xFF));

    // Byte 10: 0x01 (observed in APK captures; may indicate LZO-compressed payload)
    frame.push_back(0x01);

    // Bytes 11-12: width in pixels (little-endian)
    frame.push_back(static_cast<uint8_t>(params.width_px & 0xFF));
    frame.push_back(static_cast<uint8_t>((params.width_px >> 8) & 0xFF));

    // Bytes 13-14: chunk height in rows (little-endian)
    frame.push_back(static_cast<uint8_t>(params.chunk_height & 0xFF));
    frame.push_back(static_cast<uint8_t>((params.chunk_height >> 8) & 0xFF));

    // Byte 15: remaining chunks
    frame.push_back(params.remaining_chunks);

    // Byte 16: reserved
    frame.push_back(0x00);

    // Bitmap payload
    frame.insert(frame.end(), compressed_data.begin(), compressed_data.end());

    // Checksum over all preceding bytes
    uint8_t chk = makeid_checksum(frame.data(), frame.size());
    frame.push_back(chk);

    return frame;
}

std::vector<uint8_t> makeid_lzo_literals_only(const uint8_t* data, size_t len) {
    // Encode as a single LZO1X first-literal run — no match/copy instructions.
    // Standard miniLZO lzo1x_1_compress produces match encodings that crash the
    // MakeID E1 firmware. This literals-only encoding is universally safe.
    //
    // LZO1X first literal encoding:
    //   len <= 238: prefix byte = len + 17, then len literal bytes
    //   len > 238:  prefix byte = 0x00, then (len-18)/255 zero bytes,
    //               then (len-18)%255 as one byte, then len literal bytes
    std::vector<uint8_t> out;
    out.reserve(len + 10);

    if (len <= 238) {
        out.push_back(static_cast<uint8_t>(len + 17));
    } else {
        out.push_back(0x00);
        size_t remaining = len - 18;
        while (remaining > 255) {
            out.push_back(0x00);
            remaining -= 255;
        }
        out.push_back(static_cast<uint8_t>(remaining));
    }

    out.insert(out.end(), data, data + len);

    // LZO end-of-stream marker
    out.push_back(0x11);
    out.push_back(0x00);
    out.push_back(0x00);
    return out;
}

/// Encode bitmap to column-major format for MakeID printers.
/// Our LabelBitmap is row-major: width=print_head_px, height=label_length_px.
/// MakeID column-major: each "column" = one position along label length.
/// Each bitmap row maps directly to one column (same byte layout).
std::vector<uint8_t> makeid_encode_bitmap_raw(const LabelBitmap& bitmap, int printer_width_bytes) {
    int total_cols = bitmap.height(); // columns = label length
    int bpc = printer_width_bytes;

    std::vector<uint8_t> result(static_cast<size_t>(total_cols) * bpc, 0x00);

    for (int col = 0; col < total_cols; col++) {
        uint8_t* dst = result.data() + static_cast<size_t>(col) * bpc;
        const uint8_t* src = bitmap.row_data(col);
        int copy_bytes = std::min(bitmap.row_byte_width(), bpc);
        std::memcpy(dst, src, copy_bytes);
    }

    return result;
}

std::vector<MakeIdBitmapChunk>
makeid_encode_bitmap(const LabelBitmap& bitmap, int printer_width_bytes, int max_cols_per_chunk) {
    auto full_data = makeid_encode_bitmap_raw(bitmap, printer_width_bytes);
    int bpc = printer_width_bytes;
    int total_cols = bitmap.height();

    std::vector<MakeIdBitmapChunk> chunks;

    for (int start_col = 0; start_col < total_cols; start_col += max_cols_per_chunk) {
        int chunk_cols = std::min(max_cols_per_chunk, total_cols - start_col);
        size_t chunk_bytes = static_cast<size_t>(chunk_cols) * bpc;

        const uint8_t* chunk_ptr = full_data.data() + static_cast<size_t>(start_col) * bpc;

        MakeIdBitmapChunk chunk;
        chunk.height = chunk_cols;
        chunk.data = makeid_lzo_literals_only(chunk_ptr, chunk_bytes);
        chunks.push_back(std::move(chunk));
    }

    return chunks;
}

MakeIdPrintJob makeid_build_print_job(const LabelBitmap& bitmap, const LabelSize& size,
                                      const MakeIdPrintJobConfig& config) {
    MakeIdPrintJob job;
    job.total_rows = bitmap.height();

    // Encode bitmap into chunks
    auto chunks =
        makeid_encode_bitmap(bitmap, config.printer_width_bytes, config.max_rows_per_chunk);

    int total_chunks = static_cast<int>(chunks.size());

    spdlog::info("MakeID job: bitmap {}x{}, {} chunks, darkness={}", bitmap.width(),
                 bitmap.height(), total_chunks, config.darkness);

    for (int i = 0; i < total_chunks; i++) {
        MakeIdPrintParams params;
        params.darkness = config.darkness;
        params.label_type = config.label_type;
        params.cut_type = config.cut_type;
        params.total_copies = 1;
        params.current_copy = 1;
        // width = total columns (bitmap height in row-major = label length in px)
        params.width_px = static_cast<uint16_t>(bitmap.height());
        params.chunk_height = static_cast<uint16_t>(chunks[i].height);
        params.remaining_chunks = static_cast<uint8_t>(total_chunks - 1 - i);

        auto frame = makeid_build_print_frame(chunks[i].data, params);
        job.chunks.push_back(std::move(frame));
    }

    return job;
}

std::vector<LabelSize> makeid_e1_sizes() {
    // MakeID E1: 203 DPI tape printer
    // 9mm  -> ~72px  (9 * 203 / 25.4 ≈ 71.9)
    // 12mm -> ~96px  (12 * 203 / 25.4 ≈ 95.9)
    // 16mm tape: head is still 96px on E1 (wider tape, same head)
    return {
        {"12x40mm", 96, 307, 203, 0x01, 12, 40}, {"12x30mm", 96, 231, 203, 0x01, 12, 30},
        {"12x50mm", 96, 384, 203, 0x01, 12, 50}, {"9x40mm", 72, 307, 203, 0x01, 9, 40},
        {"9x30mm", 72, 231, 203, 0x01, 9, 30},   {"16x30mm", 96, 231, 203, 0x01, 16, 30},
        {"16x40mm", 96, 307, 203, 0x01, 16, 40}, {"16x50mm", 96, 384, 203, 0x01, 16, 50},
    };
}

std::vector<LabelSize> makeid_default_sizes() {
    return makeid_e1_sizes();
}

} // namespace helix::label

#endif // HELIX_HAS_LABEL_PRINTER
