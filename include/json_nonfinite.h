// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace helix::json_util {

/// Rewrite the bare tokens NaN, Infinity and -Infinity outside string literals
/// as null.
///
/// Python's json module writes a non-finite float that way, and Moonraker falls
/// back to it on hosts without orjson, so a sensor reading NaN turns every status
/// frame that carries it into text a strict JSON parser rejects whole. JSON has
/// no other bare token those spellings can collide with, so outside a string they
/// can only be a non-finite number.
///
/// @return true when anything was replaced; @p out then holds the rewritten text.
///         false leaves @p out unspecified.
inline bool nonfinite_literals_to_null(std::string_view text, std::string& out) {
    static constexpr std::string_view kTokens[] = {"-Infinity", "Infinity", "NaN"};
    out.clear();
    out.reserve(text.size());
    bool replaced = false;
    bool in_string = false;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (in_string) {
            out.push_back(c);
            if (c == '\\' && i + 1 < text.size()) {
                out.push_back(text[i + 1]);
                i += 2;
                continue;
            }
            if (c == '"') {
                in_string = false;
            }
            ++i;
            continue;
        }
        if (c == '"') {
            in_string = true;
            out.push_back(c);
            ++i;
            continue;
        }
        bool matched = false;
        for (std::string_view tok : kTokens) {
            if (text.compare(i, tok.size(), tok) == 0) {
                out += "null";
                i += tok.size();
                replaced = true;
                matched = true;
                break;
            }
        }
        if (!matched) {
            out.push_back(c);
            ++i;
        }
    }
    return replaced;
}

} // namespace helix::json_util
