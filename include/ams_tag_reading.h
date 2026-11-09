// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "lane_observation.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace helix::ams {

/// The fields a spool's tag (an RFID chip, or an inline colour scanner) can
/// state. Color covers a multi-colour spool's extra colours too; NozzleTemp is
/// the min/max pair.
enum class TagField : std::uint8_t {
    Material,
    Brand,
    Product,
    Color,
    NozzleTemp,
    BedTemp,
    Weight,
};

/// What a slot's tag reader reported for the spool in it, as it reported it.
/// An unset field is one the tag did not state.
struct TagReading {
    std::optional<std::string> material;
    std::optional<std::string> brand;
    std::optional<std::string> product_name;
    std::optional<uint32_t> color_rgb;
    /// A multi-colour spool's colours in SlotInfo::multi_color_hexes form
    /// ("#RRGGBB,#RRGGBB"). Empty for a single-colour spool.
    std::string multi_color_hexes;
    std::optional<int> nozzle_temp_min;
    std::optional<int> nozzle_temp_max;
    std::optional<int> bed_temp;
    std::optional<float> total_weight_g;
    /// The Spoolman spool the tag names. Set only for an id the tag actually
    /// declares: each backend's decoder applies its own tag format's rules for
    /// a value that names no spool (a factory default, a blank), so a set id
    /// is always one to look up. A binding rather than a property the slot
    /// shows, so it is no TagField and provenance and reset_to_tag() ignore
    /// it. Nothing writes a tag, so fold_tag_frame() leaves it to the caller.
    std::optional<int> spoolman_id;

    [[nodiscard]] bool states(TagField field) const;
    /// True when the reading states no field and names no spool.
    [[nodiscard]] bool empty() const;

    bool operator==(const TagReading& o) const {
        return material == o.material && brand == o.brand && product_name == o.product_name &&
               color_rgb == o.color_rgb && multi_color_hexes == o.multi_color_hexes &&
               nozzle_temp_min == o.nozzle_temp_min && nozzle_temp_max == o.nozzle_temp_max &&
               bed_temp == o.bed_temp && total_weight_g == o.total_weight_g &&
               spoolman_id == o.spoolman_id;
    }
};

/// Per field, whether the value a slot shows is its tag's.
class TagProvenance {
  public:
    [[nodiscard]] bool from_tag(TagField field) const {
        return (bits_ & bit(field)) != 0;
    }
    [[nodiscard]] bool any() const {
        return bits_ != 0;
    }
    void set(TagField field) {
        bits_ = static_cast<std::uint8_t>(bits_ | bit(field));
    }

  private:
    static constexpr std::uint8_t bit(TagField field) {
        return static_cast<std::uint8_t>(1U << static_cast<unsigned>(field));
    }
    std::uint8_t bits_ = 0;
};

/// The material, brand, product, colour and total weight a vendor-cache
/// observation states, as a tag reading. Temperatures are not part of an
/// Observation, so a backend adds them itself.
[[nodiscard]] TagReading tag_reading_from(const Observation& cache);

/// Which of @p shown's fields hold @p tag's value. A field the tag does not
/// state is never from the tag. Text compares case-insensitively; colour needs
/// color_rgb and multi_color_hexes both to match; NozzleTemp needs whichever of
/// min and max the tag states to match.
[[nodiscard]] TagProvenance tag_provenance(const TagReading& tag, const SlotInfo& shown);

/// @p slot with every field @p tag states put back to the tag's value, and
/// every other field untouched: what "Reset to tag" saves.
[[nodiscard]] SlotInfo reset_to_tag(const SlotInfo& slot, const TagReading& tag);

/// Keep what one frame of a slot's tag reader states as that slot's reading,
/// for a box whose record states the tag through a firmware observation.
/// @p nozzle_min / @p nozzle_max are the range the backend's tables give the
/// record's material, so they stand or fall with it. The slot is erased when
/// the frame states nothing.
void keep_tag_reading(std::unordered_map<int, TagReading>& kept, int slot_index,
                      const Observation& stated, std::optional<int> nozzle_min,
                      std::optional<int> nozzle_max);

} // namespace helix::ams
