// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_tag_reading.h"

#include "text_io.h"

#include <cmath>

namespace helix::ams {

namespace {

bool same_weight(float a, float b) {
    return std::fabs(a - b) < 0.5F;
}

} // namespace

bool TagReading::states(TagField field) const {
    switch (field) {
    case TagField::Material:
        return material.has_value();
    case TagField::Brand:
        return brand.has_value();
    case TagField::Product:
        return product_name.has_value();
    case TagField::Color:
        return color_rgb.has_value() || !multi_color_hexes.empty();
    case TagField::NozzleTemp:
        return nozzle_temp_min.has_value() || nozzle_temp_max.has_value();
    case TagField::BedTemp:
        return bed_temp.has_value();
    case TagField::Weight:
        return total_weight_g.has_value();
    }
    return false;
}

bool TagReading::empty() const {
    if (spoolman_id) {
        return false;
    }
    for (const TagField f :
         {TagField::Material, TagField::Brand, TagField::Product, TagField::Color,
          TagField::NozzleTemp, TagField::BedTemp, TagField::Weight}) {
        if (states(f)) {
            return false;
        }
    }
    return true;
}

TagReading tag_reading_from(const Observation& cache) {
    TagReading out;
    out.material = cache.material;
    out.brand = cache.brand;
    out.product_name = cache.product_name;
    out.color_rgb = cache.color_rgb;
    out.total_weight_g = cache.total_weight_g;
    return out;
}

TagProvenance tag_provenance(const TagReading& tag, const SlotInfo& shown) {
    TagProvenance out;
    if (tag.material && helix::text_io::equals_ignore_case(shown.material, *tag.material)) {
        out.set(TagField::Material);
    }
    if (tag.brand && helix::text_io::equals_ignore_case(shown.brand, *tag.brand)) {
        out.set(TagField::Brand);
    }
    if (tag.product_name &&
        helix::text_io::equals_ignore_case(shown.product_name, *tag.product_name)) {
        out.set(TagField::Product);
    }
    if (tag.states(TagField::Color) &&
        (!tag.color_rgb || shown.color_rgb == (*tag.color_rgb & 0xFFFFFFu)) &&
        helix::text_io::equals_ignore_case(shown.multi_color_hexes, tag.multi_color_hexes)) {
        out.set(TagField::Color);
    }
    if (tag.states(TagField::NozzleTemp) &&
        (!tag.nozzle_temp_min || shown.nozzle_temp_min == *tag.nozzle_temp_min) &&
        (!tag.nozzle_temp_max || shown.nozzle_temp_max == *tag.nozzle_temp_max)) {
        out.set(TagField::NozzleTemp);
    }
    if (tag.bed_temp && shown.bed_temp == *tag.bed_temp) {
        out.set(TagField::BedTemp);
    }
    if (tag.total_weight_g && same_weight(shown.total_weight_g, *tag.total_weight_g)) {
        out.set(TagField::Weight);
    }
    return out;
}

SlotInfo reset_to_tag(const SlotInfo& slot, const TagReading& tag) {
    SlotInfo out = slot;
    bool identity_moved = false;
    const auto reset_text = [&identity_moved](std::string& field,
                                              const std::optional<std::string>& value) {
        if (value && field != *value) {
            field = *value;
            identity_moved = true;
        }
    };
    reset_text(out.material, tag.material);
    reset_text(out.brand, tag.brand);
    reset_text(out.product_name, tag.product_name);
    // A catalog pick names one product; once the identity is the tag's it may
    // name another, and the product name above is what still describes it.
    if (identity_moved) {
        out.catalog_id.clear();
    }
    if (tag.states(TagField::Color)) {
        if (tag.color_rgb) {
            out.color_rgb = *tag.color_rgb & 0xFFFFFFu;
        }
        out.multi_color_hexes = tag.multi_color_hexes;
        // A tag carries no colour name, and the user's described another colour.
        out.color_name.clear();
    }
    if (tag.nozzle_temp_min) {
        out.nozzle_temp_min = *tag.nozzle_temp_min;
    }
    if (tag.nozzle_temp_max) {
        out.nozzle_temp_max = *tag.nozzle_temp_max;
    }
    if (tag.bed_temp) {
        out.bed_temp = *tag.bed_temp;
    }
    if (tag.total_weight_g) {
        out.total_weight_g = *tag.total_weight_g;
    }
    return out;
}

void keep_tag_reading(std::unordered_map<int, TagReading>& kept, int slot_index,
                      const Observation& stated, std::optional<int> nozzle_min,
                      std::optional<int> nozzle_max) {
    TagReading reading = tag_reading_from(stated);
    if (reading.material) {
        reading.nozzle_temp_min = nozzle_min;
        reading.nozzle_temp_max = nozzle_max;
    }
    if (reading.empty()) {
        kept.erase(slot_index);
    } else {
        kept[slot_index] = std::move(reading);
    }
}

} // namespace helix::ams
