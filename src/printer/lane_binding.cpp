// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_binding.h"

#include "text_io.h"

#include <string_view>
#include <strings.h>

namespace helix::ams {

namespace {

bool same_ignoring_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && strncasecmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace

void drop_previous_spool_declarations(LaneId lane) {
    drop_lane_source(lane, ObservationSource::Pending);
    drop_lane_source(lane, ObservationSource::Spoolman);
}

InsertVerdict classify_insert(const std::optional<SpoolEvidence>& before,
                              const SpoolEvidence& after) {
    if (!before) {
        return InsertVerdict::NoEvidence;
    }
    const auto read_done = [](const SpoolEvidence& e) {
        return e.tag_read_complete || !e.tag_uid.empty();
    };
    if (read_done(*before) && read_done(after) &&
        (!before->tag_uid.empty() || !after.tag_uid.empty())) {
        return before->tag_uid == after.tag_uid ? InsertVerdict::SameSpool
                                                : InsertVerdict::DifferentSpool;
    }

    const std::string_view before_material = helix::text_io::trim(before->material);
    const std::string_view after_material = helix::text_io::trim(after.material);
    const bool material_read = !before_material.empty() && !after_material.empty();
    const bool color_read = before->color_rgb.has_value() && after.color_rgb.has_value();
    // Tags spell one material in more than one case; they never spell two
    // materials alike. Colour is a decoded integer, so it compares exactly.
    if (material_read && !same_ignoring_case(before_material, after_material)) {
        return InsertVerdict::DifferentSpool;
    }
    if (color_read && (*before->color_rgb & 0xFFFFFFu) != (*after.color_rgb & 0xFFFFFFu)) {
        return InsertVerdict::DifferentSpool;
    }
    return material_read && color_read ? InsertVerdict::SameSpool : InsertVerdict::NoEvidence;
}

} // namespace helix::ams
