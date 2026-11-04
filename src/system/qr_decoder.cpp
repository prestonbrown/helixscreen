// SPDX-License-Identifier: GPL-3.0-or-later
#include "qr_decoder.h"

#include "core/text_io.h"
#include "quirc/lib/quirc.h"

#include <spdlog/spdlog.h>

#include <climits>
#include <cstdlib>
#include <cstring>

namespace helix {

QrDecoder::QrDecoder() {
    qr_ = quirc_new();
    if (!qr_) {
        spdlog::error("QrDecoder: failed to allocate quirc context");
    }
}

QrDecoder::~QrDecoder() {
    if (qr_) {
        quirc_destroy(qr_);
    }
}

QrDecodeResult QrDecoder::decode(const uint8_t* gray_data, int width, int height) {
    QrDecodeResult result;

    if (!qr_ || !gray_data || width <= 0 || height <= 0) {
        return result;
    }

    if (quirc_resize(qr_, width, height) < 0) {
        spdlog::error("QrDecoder: failed to resize quirc to {}x{}", width, height);
        return result;
    }

    // Copy grayscale data into quirc buffer
    int buf_w = 0;
    int buf_h = 0;
    uint8_t* buf = quirc_begin(qr_, &buf_w, &buf_h);
    if (!buf) {
        spdlog::error("QrDecoder: quirc_begin returned null");
        return result;
    }

    // Copy row by row (quirc buffer stride may differ from width)
    int copy_w = (width < buf_w) ? width : buf_w;
    int copy_h = (height < buf_h) ? height : buf_h;
    for (int y = 0; y < copy_h; ++y) {
        std::memcpy(buf + y * buf_w, gray_data + y * width, copy_w);
    }

    quirc_end(qr_);

    int count = quirc_count(qr_);
    spdlog::info("QrDecoder: found {} QR code candidates in {}x{} image", count, width, height);

    for (int i = 0; i < count; ++i) {
        struct quirc_code code;
        struct quirc_data data;

        quirc_extract(qr_, i, &code);
        quirc_decode_error_t err = quirc_decode(&code, &data);

        if (err != QUIRC_SUCCESS) {
            spdlog::info("QrDecoder: code {} decode error: {}", i, quirc_strerror(err));
            continue;
        }

        result.success = true;
        result.text = std::string(reinterpret_cast<const char*>(data.payload), data.payload_len);
        result.spool_id = parse_spoolman_id(result.text);

        spdlog::info("QrDecoder: decoded QR text='{}' spool_id={}", result.text, result.spool_id);
        break;
    }

    return result;
}

// Safe string-to-int conversion. Returns -1 on overflow or invalid input.
static int safe_parse_int(const std::string& s) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) {
        return -1;
    }
    char* end = nullptr;
    errno = 0;
    long val = std::strtol(s.c_str(), &end, 10);
    if (errno == ERANGE || val < 0 || val > INT_MAX || end != s.c_str() + s.size()) {
        return -1;
    }
    return static_cast<int>(val);
}

int QrDecoder::parse_spoolman_id(const std::string& text) {
    if (text.empty()) {
        return -1;
    }

    // Normalize text for scanners that can't type shifted characters.
    // Some USB HID barcode scanners only use Caps Lock (not Shift), so
    // '+' arrives as '=' and ':' arrives as ';'.
    std::string normalized = text;
    for (auto& c : normalized) {
        if (c == '=')
            c = '+';
        if (c == ';')
            c = ':';
    }

    if (normalized != text) {
        spdlog::debug("QrDecoder: normalized '{}' -> '{}'", text, normalized);
    }

    // Format: "web+spoolman:s-<id>". URI schemes are case-insensitive, and
    // Spoolman's own labels encode it uppercase for QR alphanumeric mode.
    const std::string prefix_ws = "web+spoolman:s-";
    if (normalized.size() > prefix_ws.size() &&
        text_io::to_lower(std::string_view(normalized).substr(0, prefix_ws.size())) == prefix_ws) {
        return safe_parse_int(normalized.substr(prefix_ws.size()));
    }

    // Format: "SM:SPOOL=<id>" (note: '=' is intentional here, not normalized)
    const std::string prefix_sm = "SM:SPOOL=";
    // Check original text first (camera QR reads correctly)
    if (text.size() > prefix_sm.size() && text.compare(0, prefix_sm.size(), prefix_sm) == 0) {
        return safe_parse_int(text.substr(prefix_sm.size()));
    }
    // Also check normalized (scanner may send "SM:SPOOL+<id>")
    const std::string prefix_sm_norm = "SM:SPOOL+";
    if (normalized.size() > prefix_sm_norm.size() &&
        normalized.compare(0, prefix_sm_norm.size(), prefix_sm_norm) == 0) {
        return safe_parse_int(normalized.substr(prefix_sm_norm.size()));
    }

    // URL formats:
    //   ".../spool/<id>"       — e.g. https://spoolman.example.com/spool/42
    //   ".../spool/<id>/"      — trailing slash variant
    //   ".../spool/show/<id>"  — Spoolman web UI URL (issue #660)
    //   ".../spool/show/<id>/" — trailing slash variant
    for (const auto& spool_path : {"/spool/show/", "/spool/"}) {
        auto pos = text.rfind(spool_path);
        if (pos == std::string::npos)
            continue;
        auto id_start = pos + std::strlen(spool_path);
        if (id_start >= text.size())
            continue;
        auto id_end = text.find('/', id_start);
        std::string id_str = (id_end != std::string::npos)
                                 ? text.substr(id_start, id_end - id_start)
                                 : text.substr(id_start);
        int result = safe_parse_int(id_str);
        if (result >= 0)
            return result;
    }

    spdlog::debug("QrDecoder: no Spoolman pattern matched for '{}' "
                  "(tried web+spoolman:, SM:SPOOL=, /spool/ URL)",
                  text);
    return -1;
}

} // namespace helix
