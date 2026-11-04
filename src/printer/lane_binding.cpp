// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_binding.h"

#include "core/text_io.h"
#include "lane_resolver.h"

#include <string_view>
#include <strings.h>

namespace helix::ams {

namespace {

/// The spool id this lane's sources DECLARE, or 0 when none of them do.
///
/// Resolving a copy with VendorCache removed rather than reading the two
/// records by hand: the ranking between Spoolman and LocalUser then has one
/// definition, resolve()'s, and cannot drift from the ladder the UI paints
/// from.
int declared_spool_id(const LaneSources& sources) {
    LaneSources declared = sources;
    declared.drop(ObservationSource::VendorCache);
    // Remembered goes for the same reason, and for one more: it is our own
    // disk copy rather than anyone's statement. It cannot carry a spool id
    // today, because a record naming one routes wholly to Spoolman, so this
    // changes no answer; it keeps the function's contract true by construction
    // rather than by a detail of another file.
    declared.drop(ObservationSource::Remembered);
    return resolve(declared).spoolman_id.value_or(0);
}

bool same_ignoring_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && strncasecmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace

BindingVerdict classify_binding(const LaneSources& sources, const BindingReading& reading) {
    const int declared = declared_spool_id(sources);

    // An external re-bind. Another well-behaved writer - Mainsail, the AFC
    // plugin, a macro - has put a DIFFERENT spool on this lane. That is a
    // statement rather than a guess, so the declared identity stops standing
    // and firmware's own reading paints. Never gated by the retention setting:
    // an explicit external write is not a preference. Never fires on 0, which
    // is the eject signal below and not a spool.
    if (reading.firmware_spool_id > 0 && declared > 0 && reading.firmware_spool_id != declared &&
        reading.firmware_spool_id != reading.own_write_old_id &&
        reading.firmware_spool_id != reading.own_write_new_id) {
        return BindingVerdict::Rebound;
    }

    // The eject signal. Meaningful only where firmware names an id while a
    // spool is loaded: there 0 is the plugin's own clear. Elsewhere 0 is the
    // everyday reading and clearing on it would empty every lane on every
    // poll. Gated by the setting because retaining identity across an eject is
    // a preference, unlike a re-bind.
    if (reading.printer_reports_spool_ids && reading.firmware_spool_id <= 0 && declared > 0 &&
        !reading.keep_spool_info_on_eject) {
        return BindingVerdict::Ejected;
    }

    return BindingVerdict::Holds;
}

void drop_previous_spool_declarations(LaneId lane) {
    drop_lane_source(lane, ObservationSource::Spoolman);
    drop_lane_source(lane, ObservationSource::LocalUser);
    drop_lane_source(lane, ObservationSource::Remembered);
}

BindingVerdict reconcile_binding(LaneId lane, const BindingReading& reading) {
    const BindingVerdict verdict = classify_binding(lane_sources(lane), reading);
    if (verdict != BindingVerdict::Holds) {
        drop_previous_spool_declarations(lane);
    }
    return verdict;
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
