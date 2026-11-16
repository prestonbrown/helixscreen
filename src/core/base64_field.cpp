// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/base64_field.h"

#include "hv/base64.h"

namespace helix {

// The first-party netd client's validate=True semantics. hv provides the
// codec; this only guards what it will accept.
std::string decode_b64_field(const std::string& encoded) {
    const size_t len = encoded.size();
    if (len == 0)
        return {};
    if (len % 4 != 0)
        return {};
    for (size_t i = 0; i < len; ++i) {
        const char c = encoded[i];
        const bool in_alphabet = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                 (c >= '0' && c <= '9') || c == '+' || c == '/';
        // '=' pads only the final quantum (last one or two positions).
        const bool is_trailing_pad = c == '=' && i + 2 >= len;
        if (!in_alphabet && !is_trailing_pad)
            return {};
    }
    return hv::Base64Decode(encoded.c_str(), static_cast<unsigned int>(len));
}

std::string encode_b64_field(const std::string& raw) {
    // hv_base64_encode dereferences its input even at length 0, so the empty
    // encoding, which is the empty string, is answered here.
    if (raw.empty())
        return {};
    return hv::Base64Encode(reinterpret_cast<const unsigned char*>(raw.data()),
                            static_cast<unsigned int>(raw.size()));
}

} // namespace helix
