// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_resolver.h"

namespace helix::ams {

ResolvedLane resolve(const LaneSources& sources) {
    ResolvedLane out;

    // Presence is sensed. Nothing in the fleet reports identity from hardware,
    // so identity metadata is never evidence that a spool is present: a vendor
    // cache that still remembers the last spool would otherwise resurrect an
    // emptied lane on every poll.
    if (sources.sensed.has_value() && sources.sensed->present.has_value()) {
        out.present = *sources.sensed->present;
    }

    // Identity, highest priority last so each pass overwrites the weaker one.
    // A source that did not observe a field leaves the weaker source's value
    // standing, which is why every field is an optional rather than a sentinel.
    const Observation* identity_ladder[] = {
        sources.vendor_cache.has_value() ? &*sources.vendor_cache : nullptr,
        sources.local_user.has_value() ? &*sources.local_user : nullptr,
        sources.spoolman.has_value() ? &*sources.spoolman : nullptr,
    };

    for (const Observation* obs : identity_ladder) {
        if (obs == nullptr) {
            continue;
        }
        if (obs->color_rgb.has_value())
            out.color_rgb = *obs->color_rgb;
        if (obs->color_name.has_value())
            out.color_name = *obs->color_name;
        if (obs->material.has_value())
            out.material = *obs->material;
        if (obs->brand.has_value())
            out.brand = *obs->brand;
        if (obs->spool_name.has_value())
            out.spool_name = *obs->spool_name;
        if (obs->catalog_id.has_value())
            out.catalog_id = *obs->catalog_id;
        if (obs->product_name.has_value())
            out.product_name = *obs->product_name;
        if (obs->spoolman_id.has_value())
            out.spoolman_id = *obs->spoolman_id;
        if (obs->spoolman_vendor_id.has_value())
            out.spoolman_vendor_id = *obs->spoolman_vendor_id;
    }

    // A colour the user picked for this lane outranks the linked spool's own.
    // The two are different statements: the spool record says what the vendor
    // sells, the user's pick says what is loaded right now. Ranking the spool
    // above it discards a pick made on the printer's own screen. Nothing else
    // in the identity block is overridden this way: brand, name and catalog
    // identity belong to the spool, not to the lane.
    if (sources.local_user.has_value() && sources.local_user->color_rgb.has_value()) {
        out.color_rgb = *sources.local_user->color_rgb;
    }

    return out;
}

} // namespace helix::ams
