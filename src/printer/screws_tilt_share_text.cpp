// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screws_tilt_share_text.h"

#include "core/text_io.h"

#include <cstdio>

namespace helix {

std::string format_screw_share_z(const ScrewTiltResult& screw) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(screw.z_height));
    return buf;
}

std::string format_screw_share_adjustment(const ScrewTiltResult& screw, const char* base_label) {
    if (screw.is_reference) {
        return base_label ? base_label : kScrewsTiltShareBaseLabel;
    }
    std::string adjustment(helix::text_io::trim(screw.adjustment));
    if (adjustment.empty()) {
        return kScrewsTiltShareNoAdjustment;
    }
    return adjustment;
}

std::string build_screws_tilt_share_text(const std::vector<ScrewTiltResult>& results) {
    std::string out = kScrewsTiltShareHeader;

    if (results.empty()) {
        out += "\n(no results)";
        return out;
    }

    for (const auto& screw : results) {
        out += '\n';
        out += screw.display_name();
        out += ": z=";
        out += format_screw_share_z(screw);
        out += ' ';
        out += format_screw_share_adjustment(screw);
    }

    return out;
}

} // namespace helix
