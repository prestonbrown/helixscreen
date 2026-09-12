// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_translation.h"

#include "ams_types.h"

#include <cmath>

namespace helix::ams {

namespace {

/// The tolerance the spool editor itself uses to decide a weight was edited
/// (AmsEditOverlay::is_dirty). Anything finer is a consumption tick or float
/// noise, not a number a person typed into a gram field.
constexpr float WEIGHT_EPSILON_G = 0.1f;

/// True when a weight the editor committed is a number a person could have
/// entered. Negative is SlotInfo's "unknown" sentinel, and the editor refuses
/// a negative entry, so a negative can only have come from a clear.
bool is_declarable_weight(float grams) {
    return grams >= 0.0f;
}

bool weight_changed(float original, float edited) {
    return std::fabs(edited - original) > WEIGHT_EPSILON_G;
}

} // namespace

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

    // AMS_DEFAULT_SLOT_COLOR means "no colour reading", not a grey a person
    // picked, and clearing a slot writes it. Filing it would leave a user
    // colour outranking every server value with a sentinel. The cost is that
    // this one grey cannot be declared, which is the trade the sentinel
    // already imposes everywhere else it is read.
    if (edited.color_rgb != original.color_rgb && edited.color_rgb != AMS_DEFAULT_SLOT_COLOR)
        obs.color_rgb = edited.color_rgb;
    if (edited.color_name != original.color_name)
        obs.color_name = edited.color_name;
    if (edited.material != original.material)
        obs.material = edited.material;
    if (edited.brand != original.brand)
        obs.brand = edited.brand;
    if (edited.spool_name != original.spool_name)
        obs.spool_name = edited.spool_name;
    if (edited.spoolman_vendor_id != original.spoolman_vendor_id)
        obs.spoolman_vendor_id = edited.spoolman_vendor_id;

    // catalog_id and product_name are absent by the same rule
    // AmsEditOverlay::is_dirty() applies to them: the spool-edit view
    // auto-highlights a product and Save copies whatever is highlighted, so
    // these two arrive on a commit no person touched them in. Keep the two
    // field lists in agreement.

    if (is_declarable_weight(edited.remaining_weight_g) &&
        weight_changed(original.remaining_weight_g, edited.remaining_weight_g))
        obs.remaining_weight_g = edited.remaining_weight_g;
    if (is_declarable_weight(edited.total_weight_g) &&
        weight_changed(original.total_weight_g, edited.total_weight_g))
        obs.total_weight_g = edited.total_weight_g;
    return obs;
}

ObservationSource classify_declaration(const FilamentSlotOverride& record,
                                       const nlohmann::json& wire) {
    if (record.spoolman_id > 0) {
        return ObservationSource::Spoolman;
    }
    const bool locked_color = wire.contains("helix_locked_color") &&
                              wire["helix_locked_color"].is_boolean() &&
                              wire["helix_locked_color"].get<bool>();
    const bool locked_material = wire.contains("helix_locked_material") &&
                                 wire["helix_locked_material"].is_boolean() &&
                                 wire["helix_locked_material"].get<bool>();
    return (locked_color || locked_material) ? ObservationSource::LocalUser
                                             : ObservationSource::VendorCache;
}

Observation declared_from_record(const FilamentSlotOverride& record, const nlohmann::json& wire) {
    Observation obs(classify_declaration(record, wire));

    // AMS_DEFAULT_SLOT_COLOR means "no colour reading", not a grey a person
    // or a legacy writer chose, and it can ride in on the wire the same way a
    // cleared SlotInfo carries it (see user_edit_observation above). Filing it
    // would hand every uncoloured legacy lane a declared grey.
    if (record.color_set && record.color_rgb != AMS_DEFAULT_SLOT_COLOR)
        obs.color_rgb = record.color_rgb;
    if (!record.color_name.empty())
        obs.color_name = record.color_name;
    if (!record.material.empty())
        obs.material = record.material;
    if (!record.brand.empty())
        obs.brand = record.brand;
    if (!record.spool_name.empty())
        obs.spool_name = record.spool_name;
    if (!record.catalog_id.empty())
        obs.catalog_id = record.catalog_id;
    if (!record.product_name.empty())
        obs.product_name = record.product_name;
    if (record.spoolman_id > 0)
        obs.spoolman_id = record.spoolman_id;
    if (record.spoolman_vendor_id > 0)
        obs.spoolman_vendor_id = record.spoolman_vendor_id;
    if (record.remaining_weight_g >= 0.0F)
        obs.remaining_weight_g = record.remaining_weight_g;
    if (record.total_weight_g >= 0.0F)
        obs.total_weight_g = record.total_weight_g;
    return obs;
}

} // namespace helix::ams
