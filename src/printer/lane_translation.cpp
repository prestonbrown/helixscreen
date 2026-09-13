// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_translation.h"

#include "ams_types.h"
#include "color_utils.h"
#include "json_utils.h"

#include <cctype>
#include <cmath>
#include <tuple>
#include <type_traits>
#include <utility>

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

/// What kind of value a field holds, which is what decides whether a given
/// value counts as a declaration. The two translations ask different questions
/// of the same field - an edit has a before-value to compare against and a
/// stored record has none - so the kind names the field's shape and each
/// translation supplies its own rule for that shape.
enum class FieldKind {
    Untranslated, ///< Neither translation carries it.
    Color,        ///< Carries the AMS_DEFAULT_SLOT_COLOR "no reading" sentinel.
    Text,
    PositiveId, ///< Zero is "unset", so a stored zero declares nothing.
    Weight,     ///< Negative is the "unknown" sentinel.
};

/// One field, named once for every translation that carries it. A nullptr
/// member says that translation does not claim the field, with the reason on
/// the row.
template <FieldKind K, typename SlotMember, typename RecordMember, typename ObsMember>
struct FieldRow {
    static constexpr FieldKind kind = K;
    SlotMember slot;     ///< SlotInfo member the edit path reads
    RecordMember record; ///< FilamentSlotOverride member the record path reads
    ObsMember obs;       ///< where both file the value
};

template <FieldKind K, typename S, typename R, typename O>
constexpr auto field(S slot, R record, O obs) {
    return FieldRow<K, S, R, O>{slot, record, obs};
}

/// Every Observation field, once. This is the field list both translations
/// walk, so a field reaches or is refused by each of them here rather than in
/// two places that agree only by convention.
constexpr auto FIELD_ROSTER = std::make_tuple(
    // Presence is sensed, never declared, so neither translation carries it.
    field<FieldKind::Untranslated>(nullptr, nullptr, &Observation::present),
    field<FieldKind::Color>(&SlotInfo::color_rgb, &FilamentSlotOverride::color_rgb,
                            &Observation::color_rgb),
    field<FieldKind::Text>(&SlotInfo::color_name, &FilamentSlotOverride::color_name,
                           &Observation::color_name),
    field<FieldKind::Text>(&SlotInfo::material, &FilamentSlotOverride::material,
                           &Observation::material),
    field<FieldKind::Text>(&SlotInfo::brand, &FilamentSlotOverride::brand, &Observation::brand),
    field<FieldKind::Text>(&SlotInfo::spool_name, &FilamentSlotOverride::spool_name,
                           &Observation::spool_name),
    // The edit path refuses catalog_id and product_name by the rule
    // AmsEditOverlay::is_dirty() applies to them: the spool-edit view
    // auto-highlights a product and Save copies whatever is highlighted, so
    // both arrive on commits no person touched them in.
    field<FieldKind::Text>(nullptr, &FilamentSlotOverride::catalog_id, &Observation::catalog_id),
    field<FieldKind::Text>(nullptr, &FilamentSlotOverride::product_name,
                           &Observation::product_name),
    // A binding change is a whole statement the edit path answers before it
    // walks any field, so spoolman_id is not one of the fields it walks.
    field<FieldKind::PositiveId>(nullptr, &FilamentSlotOverride::spoolman_id,
                                 &Observation::spoolman_id),
    field<FieldKind::PositiveId>(&SlotInfo::spoolman_vendor_id,
                                 &FilamentSlotOverride::spoolman_vendor_id,
                                 &Observation::spoolman_vendor_id),
    field<FieldKind::Weight>(&SlotInfo::remaining_weight_g,
                             &FilamentSlotOverride::remaining_weight_g,
                             &Observation::remaining_weight_g),
    field<FieldKind::Weight>(&SlotInfo::total_weight_g, &FilamentSlotOverride::total_weight_g,
                             &Observation::total_weight_g));

static_assert(std::tuple_size_v<decltype(FIELD_ROSTER)> ==
                  std::tuple_size_v<decltype(std::declval<Observation&>().fields())>,
              "every Observation field needs a row above: a field with no row is dropped by "
              "both translations while the store amends it, with nothing red");

template <typename Fn> void for_each_field(Fn&& fn) {
    std::apply([&fn](const auto&... rows) { (fn(rows), ...); }, FIELD_ROSTER);
}

/// True when this row's translation does not carry the field.
template <typename Member> constexpr bool skipped = std::is_null_pointer_v<Member>;

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

    // The user's statement is what moved. A field that reads the same as the
    // editor opened on is not theirs to claim, whatever it holds.
    for_each_field([&](const auto& f) {
        using Row = std::decay_t<decltype(f)>;
        if constexpr (!skipped<decltype(f.slot)>) {
            const auto& before = original.*(f.slot);
            const auto& after = edited.*(f.slot);
            if constexpr (Row::kind == FieldKind::Weight) {
                if (is_declarable_weight(after) && weight_changed(before, after))
                    obs.*(f.obs) = after;
            } else if constexpr (Row::kind == FieldKind::Color) {
                if (after != before && is_declarable_color(after))
                    obs.*(f.obs) = after;
            } else {
                // A cleared text field and a zeroed id are both moves a person
                // made, so an empty value here is a declaration, not an absence.
                if (after != before)
                    obs.*(f.obs) = after;
            }
        }
    });
    return obs;
}

ObservationSource classify_declaration(const FilamentSlotOverride& record,
                                       const nlohmann::json& wire) {
    if (record.spoolman_id > 0) {
        return ObservationSource::Spoolman;
    }
    // A lock counts only when the key is actually present on the wire: the
    // parsed struct defaults a missing key from color_set / material presence
    // (from_lane_data_record's legacy-preservation rule), which is not a
    // declaration. safe_bool supplies the truthiness rule the parser itself
    // uses, so a non-boolean lock value classifies the same way here as it
    // did on load, rather than disagreeing with the parser on the same key.
    const auto locked = [&wire](const char* key) {
        return wire.contains(key) && helix::json_util::safe_bool(wire, key, false);
    };
    return (locked("helix_locked_color") || locked("helix_locked_material"))
               ? ObservationSource::LocalUser
               : ObservationSource::VendorCache;
}

Observation declared_from_record(const FilamentSlotOverride& record, const nlohmann::json& wire) {
    Observation obs(classify_declaration(record, wire));

    // A stored record has no before-value, so what it carries is what it
    // declares. Every field defaults to something value-shaped (empty string,
    // 0, -1.0f), and only a value past that default is a statement.
    for_each_field([&](const auto& f) {
        using Row = std::decay_t<decltype(f)>;
        if constexpr (!skipped<decltype(f.record)>) {
            const auto& value = record.*(f.record);
            if constexpr (Row::kind == FieldKind::Color) {
                // color_set is the record's own "a colour is present" flag and
                // has no Observation field to occupy a row, so the one colour
                // row reads it directly. color_rgb is undefined while it is false.
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
    return rgb != AMS_DEFAULT_SLOT_COLOR;
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
