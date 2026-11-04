// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_tag_reading.h"

#include "core/text_io.h"

#include <cmath>

namespace helix::ams {

namespace {

bool same_weight(float a, float b) {
    return std::fabs(a - b) < 0.5F;
}

/// One field of fold_tag_frame(): stated and filed is a reading, stated and
/// withheld keeps what was kept, unstated is gone.
template <typename T>
std::optional<T> fold_field(const std::optional<T>& kept, const std::optional<T>& raw,
                            const std::optional<T>& filed) {
    if (!raw) {
        return std::nullopt;
    }
    if (filed) {
        return filed;
    }
    return kept;
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

std::optional<TagReading> fold_tag_frame(const std::optional<TagReading>& kept,
                                         const TagReading& raw, const TagReading& filed) {
    const TagReading none;
    const TagReading& k = kept ? *kept : none;
    TagReading out;
    out.material = fold_field(k.material, raw.material, filed.material);
    out.brand = fold_field(k.brand, raw.brand, filed.brand);
    out.product_name = fold_field(k.product_name, raw.product_name, filed.product_name);
    out.color_rgb = fold_field(k.color_rgb, raw.color_rgb, filed.color_rgb);
    out.nozzle_temp_min = fold_field(k.nozzle_temp_min, raw.nozzle_temp_min, filed.nozzle_temp_min);
    out.nozzle_temp_max = fold_field(k.nozzle_temp_max, raw.nozzle_temp_max, filed.nozzle_temp_max);
    out.bed_temp = fold_field(k.bed_temp, raw.bed_temp, filed.bed_temp);
    out.total_weight_g = fold_field(k.total_weight_g, raw.total_weight_g, filed.total_weight_g);
    if (!raw.multi_color_hexes.empty()) {
        out.multi_color_hexes =
            !filed.multi_color_hexes.empty() ? filed.multi_color_hexes : k.multi_color_hexes;
    }
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

void fold_tag_frame_into(std::unordered_map<int, TagReading>& kept, int slot_index,
                         const Observation& stated, const Observation& filed,
                         std::optional<int> nozzle_min, std::optional<int> nozzle_max) {
    const auto with_temps = [&](TagReading reading) {
        if (reading.material) {
            reading.nozzle_temp_min = nozzle_min;
            reading.nozzle_temp_max = nozzle_max;
        }
        return reading;
    };
    std::optional<TagReading> before;
    if (auto it = kept.find(slot_index); it != kept.end()) {
        before = it->second;
    }
    std::optional<TagReading> folded = fold_tag_frame(before, with_temps(tag_reading_from(stated)),
                                                      with_temps(tag_reading_from(filed)));
    if (folded) {
        kept[slot_index] = std::move(*folded);
    } else {
        kept.erase(slot_index);
    }
}

} // namespace helix::ams
