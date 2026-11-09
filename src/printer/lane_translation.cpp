// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_translation.h"

#include "ams_types.h"
#include "color_utils.h"
#include "core/text_io.h"
#include "json_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace helix::ams {

namespace {

/// The tolerance the spool editor itself uses to decide a weight was edited
/// (AmsEditOverlay::is_dirty). Anything finer is a consumption tick or float
/// noise, not a number a person typed into a gram field.
constexpr float kWeightEpsilonG = 0.1f;

/// True when a weight the editor committed is a number a person could have
/// entered. Negative is SlotInfo's "unknown" sentinel, and the editor refuses
/// a negative entry, so a negative can only have come from a clear.
bool is_declarable_weight(float grams) {
    return grams >= 0.0f;
}

bool weight_changed(float original, float edited) {
    return std::fabs(edited - original) > kWeightEpsilonG;
}

/// What kind of value a field holds, which is what decides whether a given
/// value counts as a declaration. The two translations ask different questions
/// of the same field - an edit has a before-value to compare against and a
/// stored record has none - so the kind names the field's shape and each
/// translation supplies its own rule for that shape.
enum class FieldKind {
    Untranslated, ///< Neither translation carries it.
    Color,        ///< Carries the kAmsDefaultSlotColor "no reading" sentinel.
    Text,
    PositiveId, ///< Zero is "unset", so a stored zero declares nothing.
    Weight,     ///< Negative is the "unknown" sentinel.
};

/// Who owns a field's value on a lane bound to a spool.
enum class Owner {
    /// Whichever source the lane ranks highest, a person's edit included.
    Lane,
    /// The bound spool. An edit that keeps the spool does not move the field:
    /// the value comes from the spool's record and the edit declares nothing
    /// for it.
    SpoolWhenLinked,
};

/// One field, named once for every translation that carries it. A nullptr
/// member says that translation does not claim the field, with the reason on
/// the row.
template <FieldKind K, Owner W, typename SlotMember, typename RecordMember, typename ObsMember>
struct FieldRow {
    static constexpr FieldKind kind = K;
    static constexpr Owner owner = W;
    std::string_view name; ///< the field's name, as an error message spells it
    SlotMember slot;       ///< SlotInfo member the edit path reads
    RecordMember record;   ///< FilamentSlotOverride member the record path reads
    ObsMember obs;         ///< where both file the value
};

template <FieldKind K, Owner W = Owner::Lane, typename S, typename R, typename O>
constexpr auto field(std::string_view name, S slot, R record, O obs) {
    return FieldRow<K, W, S, R, O>{name, slot, record, obs};
}

/// Every Observation field, once. This is the field list both translations
/// walk, so a field reaches or is refused by each of them here rather than in
/// two places that agree only by convention.
constexpr auto kFieldRoster = std::make_tuple(
    // Presence is sensed, never declared, so neither translation carries it.
    field<FieldKind::Untranslated>("present", nullptr, nullptr, &Observation::present),
    // Neither is docking: no editor or stored record can state where a
    // toolhead is parked.
    field<FieldKind::Untranslated>("tool_docked", nullptr, nullptr, &Observation::tool_docked),
    field<FieldKind::Color>("color_rgb", &SlotInfo::color_rgb, &FilamentSlotOverride::color_rgb,
                            &Observation::color_rgb),
    field<FieldKind::Text>("color_name", &SlotInfo::color_name, &FilamentSlotOverride::color_name,
                           &Observation::color_name),
    // Material, brand, spool name and vendor id are what a Spoolman spool states
    // about itself, so on a linked lane the spool owns them.
    field<FieldKind::Text, Owner::SpoolWhenLinked>(
        "material", &SlotInfo::material, &FilamentSlotOverride::material, &Observation::material),
    field<FieldKind::Text, Owner::SpoolWhenLinked>(
        "brand", &SlotInfo::brand, &FilamentSlotOverride::brand, &Observation::brand),
    field<FieldKind::Text, Owner::SpoolWhenLinked>("spool_name", &SlotInfo::spool_name,
                                                   &FilamentSlotOverride::spool_name,
                                                   &Observation::spool_name),
    // The edit path refuses catalog_id and product_name by the rule
    // AmsEditOverlay::is_dirty() applies to them: the spool-edit view
    // auto-highlights a product and Save copies whatever is highlighted, so
    // both arrive on commits no person touched them in.
    field<FieldKind::Text>("catalog_id", nullptr, &FilamentSlotOverride::catalog_id,
                           &Observation::catalog_id),
    field<FieldKind::Text>("product_name", nullptr, &FilamentSlotOverride::product_name,
                           &Observation::product_name),
    // A binding change is a whole statement the edit path answers before it
    // walks any field, so spoolman_id is not one of the fields it walks.
    field<FieldKind::PositiveId>("spoolman_id", nullptr, &FilamentSlotOverride::spoolman_id,
                                 &Observation::spoolman_id),
    // The filament definition behind the binding: the Spoolman fetch states it
    // beside the spool id, and it names nothing once the spool is unlinked, so
    // every routing that drops the binding drops it too.
    field<FieldKind::PositiveId>("spoolman_filament_id", nullptr,
                                 &FilamentSlotOverride::spoolman_filament_id,
                                 &Observation::spoolman_filament_id),
    // The spool's vendor, which the Spoolman fetch states while the spool is
    // linked. No edit declares it: a store that cannot hold it (AFC's and
    // Happy Hare's extras) would never echo it.
    field<FieldKind::PositiveId>("spoolman_vendor_id", nullptr,
                                 &FilamentSlotOverride::spoolman_vendor_id,
                                 &Observation::spoolman_vendor_id),
    // The OrcaSlicer preset id the spool's filament names. Like the definition
    // id, only the Spoolman fetch states it and no editor writes it, and it
    // goes with the binding.
    field<FieldKind::Text>("spoolman_orca_filament_id", nullptr,
                           &FilamentSlotOverride::spoolman_orca_filament_id,
                           &Observation::spoolman_orca_filament_id),
    field<FieldKind::Weight>("remaining_weight_g", &SlotInfo::remaining_weight_g,
                             &FilamentSlotOverride::remaining_weight_g,
                             &Observation::remaining_weight_g),
    field<FieldKind::Weight>("total_weight_g", &SlotInfo::total_weight_g,
                             &FilamentSlotOverride::total_weight_g, &Observation::total_weight_g));

static_assert(std::tuple_size_v<decltype(kFieldRoster)> ==
                  std::tuple_size_v<decltype(std::declval<Observation&>().fields())>,
              "every Observation field needs a row above: a field with no row is dropped by "
              "both translations while the store amends it, with nothing red");

template <typename Fn> void for_each_field(Fn&& fn) {
    std::apply([&fn](const auto&... rows) { (fn(rows), ...); }, kFieldRoster);
}

/// True when this row's translation does not carry the field.
template <typename Member> constexpr bool skipped = std::is_null_pointer_v<Member>;

} // namespace

Observation user_edit_observation(const SlotInfo& original, const SlotInfo& edited) {
    Observation obs(ObservationSource::Pending);
    const bool binding_changed = edited.spoolman_id != original.spoolman_id;
    if (binding_changed) {
        obs.spoolman_id = edited.spoolman_id;
    }
    // On an edit that keeps a linked spool the spool owns what it states
    // about itself, so a move of those fields is not the edit's to write.
    const bool keeps_spool = !binding_changed && edited.spoolman_id > 0;
    for_each_field([&](const auto& f) {
        using Row = std::decay_t<decltype(f)>;
        if constexpr (!skipped<decltype(f.slot)>) {
            if constexpr (Row::owner == Owner::SpoolWhenLinked) {
                if (keeps_spool) {
                    return;
                }
            }
            const auto& before = original.*(f.slot);
            const auto& after = edited.*(f.slot);
            if constexpr (Row::kind == FieldKind::Weight) {
                if (is_declarable_weight(after) && weight_changed(before, after))
                    obs.*(f.obs) = after;
            } else if constexpr (Row::kind == FieldKind::Color) {
                if (after != before && is_declarable_color(after))
                    obs.*(f.obs) = after;
            } else if (after != before) {
                obs.*(f.obs) = after;
            }
        }
    });
    return obs;
}

SlotInfo keep_spool_owned_identity(const SlotInfo& original, const SlotInfo& edited,
                                   const std::optional<Observation>& spool_record) {
    SlotInfo applied = edited;
    if (edited.spoolman_id <= 0 || edited.spoolman_id != original.spoolman_id) {
        return applied;
    }
    // A record naming another spool states nothing about this one.
    const bool describes_spool =
        spool_record.has_value() && spool_record->spoolman_id == edited.spoolman_id;
    for_each_field([&](const auto& f) {
        using Row = std::decay_t<decltype(f)>;
        if constexpr (Row::owner == Owner::SpoolWhenLinked && !skipped<decltype(f.slot)>) {
            using Stated = std::decay_t<decltype(std::declval<const Observation&>().*(f.obs))>;
            const Stated stated = describes_spool ? (*spool_record).*(f.obs) : Stated{};
            applied.*(f.slot) = stated.has_value() ? *stated : original.*(f.slot);
        }
    });
    return applied;
}

bool drop_stale_spool_name(const SlotInfo& original, SlotInfo& edited) {
    if (edited.spool_name.empty() || edited.spool_name != original.spool_name) {
        return false;
    }
    if (original.spoolman_id > 0 || edited.spoolman_id > 0) {
        return false;
    }
    const auto moved = [](const std::string& before, const std::string& after) {
        return text_io::to_lower(before) != text_io::to_lower(after);
    };
    const bool product_moved =
        !original.catalog_id.empty() && original.catalog_id != edited.catalog_id;
    if (!moved(original.brand, edited.brand) && !moved(original.material, edited.material) &&
        !product_moved) {
        return false;
    }
    edited.spool_name.clear();
    return true;
}

std::vector<std::string_view> spool_owned_fields_dropped(const SlotInfo& original,
                                                         const SlotInfo& edited,
                                                         const SlotInfo& applied) {
    std::vector<std::string_view> dropped;
    if (edited.spoolman_id <= 0 || edited.spoolman_id != original.spoolman_id) {
        return dropped;
    }
    for_each_field([&](const auto& f) {
        using Row = std::decay_t<decltype(f)>;
        if constexpr (Row::owner == Owner::SpoolWhenLinked && !skipped<decltype(f.slot)>) {
            if (edited.*(f.slot) != original.*(f.slot) && applied.*(f.slot) != edited.*(f.slot)) {
                dropped.push_back(f.name);
            }
        }
    });
    return dropped;
}

Observation record_observation(const FilamentSlotOverride& record) {
    Observation obs(ObservationSource::Record);
    // Every field defaults to something value-shaped (empty string, 0, -1.0f),
    // and only a value past that default is a statement.
    for_each_field([&](const auto& f) {
        using Row = std::decay_t<decltype(f)>;
        if constexpr (!skipped<decltype(f.record)>) {
            const auto& value = record.*(f.record);
            if constexpr (Row::kind == FieldKind::Color) {
                // color_set is the record's own "a colour is present" flag;
                // color_rgb is undefined while it is false.
                if (record.color_set && is_declarable_color(value))
                    obs.*(f.obs) = value;
            } else if constexpr (Row::kind == FieldKind::Text) {
                if (!value.empty())
                    obs.*(f.obs) = value;
            } else if constexpr (Row::kind == FieldKind::PositiveId) {
                if (value > 0)
                    obs.*(f.obs) = value;
            } else {
                if (is_declarable_weight(value))
                    obs.*(f.obs) = value;
            }
        }
    });
    return obs;
}

bool is_declarable_color(uint32_t rgb) {
    return rgb != kAmsDefaultSlotColor;
}

uint32_t picked_color(uint32_t rgb) {
    return is_declarable_color(rgb) ? rgb : 0x7F7F7F;
}

ColorReading read_lane_color(const std::string& raw) {
    // A value that is only whitespace and a prefix carries no colour to fail
    // to parse, so it is the producer saying the lane has none.
    size_t begin = 0;
    size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1]))) {
        --end;
    }
    if (begin < end && raw[begin] == '#') {
        ++begin;
    }
    if (begin == end) {
        return {ColorReadingKind::Cleared, 0};
    }

    if (const auto rgb = parse_hex_color(raw)) {
        return {ColorReadingKind::Observed, *rgb};
    }
    return {ColorReadingKind::NoReading, 0};
}

} // namespace helix::ams
