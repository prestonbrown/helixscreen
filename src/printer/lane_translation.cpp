// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_translation.h"

#include "ams_types.h"

namespace helix::ams {

Observation user_edit_observation(const SlotInfo& original, const SlotInfo& edited) {
    Observation obs(ObservationSource::LocalUser);

    // A binding change is a statement about the binding, not about the fields
    // that rode in with it. Linking a spool carries the spool's colour, brand
    // and material into the same commit, and unlinking clears them; in neither
    // direction did a person choose those values, so neither direction may
    // file them as the person's own declaration.
    if (edited.spoolman_id != original.spoolman_id) {
        obs.spoolman_id = edited.spoolman_id;
        return obs;
    }

    if (edited.color_rgb != original.color_rgb)
        obs.color_rgb = edited.color_rgb;
    if (edited.color_name != original.color_name)
        obs.color_name = edited.color_name;
    if (edited.material != original.material)
        obs.material = edited.material;
    if (edited.brand != original.brand)
        obs.brand = edited.brand;
    if (edited.spool_name != original.spool_name)
        obs.spool_name = edited.spool_name;
    if (edited.catalog_id != original.catalog_id)
        obs.catalog_id = edited.catalog_id;
    if (edited.product_name != original.product_name)
        obs.product_name = edited.product_name;
    if (edited.spoolman_vendor_id != original.spoolman_vendor_id)
        obs.spoolman_vendor_id = edited.spoolman_vendor_id;
    if (edited.remaining_weight_g != original.remaining_weight_g)
        obs.remaining_weight_g = edited.remaining_weight_g;
    if (edited.total_weight_g != original.total_weight_g)
        obs.total_weight_g = edited.total_weight_g;
    return obs;
}

} // namespace helix::ams
