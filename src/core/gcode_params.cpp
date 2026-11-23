// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/gcode_params.h"

namespace helix::gcode {

namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t';
}

} // namespace

std::optional<std::string> gcode_param_value(std::string_view line, std::string_view key) {
    std::string needle(key);
    needle += '=';
    size_t pos = line.find(needle);
    while (pos != std::string_view::npos && pos > 0 && !is_space(line[pos - 1])) {
        pos = line.find(needle, pos + 1);
    }
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t start = pos + needle.size();
    if (start >= line.size()) {
        return std::nullopt;
    }
    const char quote = line[start];
    if (quote == '"' || quote == '\'') {
        const size_t close = line.find(quote, start + 1);
        if (close == std::string_view::npos || close == start + 1) {
            return std::nullopt;
        }
        return std::string(line.substr(start + 1, close - start - 1));
    }
    const size_t end = line.find_first_of(" \t", start);
    return std::string(
        line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
}

} // namespace helix::gcode
