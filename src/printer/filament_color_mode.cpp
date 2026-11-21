// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_color_mode.h"

#include <cctype>

namespace helix::color_mode {

namespace {

bool equals_ignore_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string normalize(std::string_view mode) {
    if (equals_ignore_case(mode, kGradient) || equals_ignore_case(mode, "longitudinal")) {
        return std::string(kGradient);
    }
    if (equals_ignore_case(mode, kCoaxial)) {
        return std::string(kCoaxial);
    }
    return {};
}

std::string to_spoolman_direction(std::string_view mode) {
    const std::string m = normalize(mode);
    if (m == kGradient) {
        return "longitudinal";
    }
    return m; // "coaxial" or empty
}

std::string_view to_spec(std::string_view mode) {
    const std::string m = normalize(mode);
    if (m == kGradient) {
        return kGradient;
    }
    if (m == kCoaxial) {
        return kCoaxial;
    }
    return kSolid;
}

} // namespace helix::color_mode
