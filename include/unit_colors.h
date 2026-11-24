// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file unit_colors.h
 * @brief The colors a printed filament unit was printed in: three regions, presets, and the
 * region layers a unit's drawing tints with them.
 *
 * A printed (DIY) unit's drawing may bake a white tint mask and a shade layer per color region,
 * at the back of the drawing (behind the spools) and at its front. The app fills each mask with
 * the user's color for that region. A region the user left unset, or one the drawing has no mask
 * for, keeps the drawing's baked color.
 */

#include "json_fwd.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix {

enum class UnitColorRegion : uint8_t { Main, Accent, Panels };

inline constexpr std::array<UnitColorRegion, 3> kUnitColorRegions{
    UnitColorRegion::Main, UnitColorRegion::Accent, UnitColorRegion::Panels};

/// The region's name in settings and in baked layer names: "main", "accent" or "panels".
const char* unit_color_region_name(UnitColorRegion region);

/// A unit's chosen colors, 0xRRGGBB; an unset region keeps the drawing's own color.
struct UnitColors {
    std::optional<uint32_t> main;
    std::optional<uint32_t> accent;
    std::optional<uint32_t> panels;

    std::optional<uint32_t> get(UnitColorRegion region) const;
    void set(UnitColorRegion region, std::optional<uint32_t> rgb);
    bool empty() const {
        return !main && !accent && !panels;
    }
    bool operator==(const UnitColors& other) const {
        return main == other.main && accent == other.accent && panels == other.panels;
    }
    bool operator!=(const UnitColors& other) const {
        return !(*this == other);
    }
};

/// A named set of all three colors, offered beside the free picker.
struct UnitColorPreset {
    const char* id;   ///< stable key
    const char* name; ///< shown to the user, translated where shown
    uint32_t main;
    uint32_t accent;
    uint32_t panels;

    UnitColors colors() const {
        return UnitColors{main, accent, panels};
    }
};

/// Every preset, in the order a picker lists them.
const std::vector<UnitColorPreset>& unit_color_presets();

/// The preset with id @p id, or nullptr.
const UnitColorPreset* find_unit_color_preset(std::string_view id);

/// The colors a drawing shows: each region @p picked sets, else @p baked's.
UnitColors unit_colors_shown(const UnitColors& picked, const UnitColors& baked);

/// The Colors menu, by option: 0 keeps the drawing's own colors, 1 to N are
/// unit_color_presets() in order, and N + 1 is Custom.
int unit_color_option_count();

/// The option @p colors select: 0 when none is set, a preset's when all three match it,
/// else Custom.
int unit_color_option(const UnitColors& colors);

/// The colors choosing @p option saves: none for the drawing's own, or the preset's. nullopt
/// for Custom, which saves nothing until a swatch changes, and past the menu.
std::optional<UnitColors> unit_colors_for_option(int option);

/// The colors as saved: {"main": "#RRGGBB", ...}, an unset region left out.
nlohmann::json unit_colors_to_json(const UnitColors& colors);

/// Colors read back from unit_colors_to_json's form; a missing or unreadable region is unset.
UnitColors unit_colors_from_json(const nlohmann::json& value);

/// Where in the drawing's depth a region mask stands.
enum class UnitArtDepth : uint8_t { Back, Front };

/// The bit a drawing sets in UnitShell::color_regions when it bakes @p region's mask at
/// @p depth.
constexpr uint8_t unit_color_mask_bit(UnitArtDepth depth, UnitColorRegion region) {
    return static_cast<uint8_t>(1u << (static_cast<unsigned>(depth) * kUnitColorRegions.size() +
                                       static_cast<unsigned>(region)));
}

/// One region layer the drawing tints: `<base>_<stem>.png` filled with @p color, then
/// `<base>_<stem>_shade.png` over it.
struct UnitColorLayer {
    UnitArtDepth depth;
    UnitColorRegion region;
    uint32_t color;
    std::string stem; ///< "back_main", "front_accent", ...
};

/// The region layers a drawing that bakes @p baked_masks (UnitShell::color_regions) shows for
/// @p colors, back layers first, each depth in region order. Empty when the drawing bakes no
/// masks or the user set no color for any it bakes, so the drawing stays as baked.
std::vector<UnitColorLayer> unit_color_layers(uint8_t baked_masks, const UnitColors& colors);

} // namespace helix
