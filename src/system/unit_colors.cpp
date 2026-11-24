// SPDX-License-Identifier: GPL-3.0-or-later

#include "unit_colors.h"

#include "color_utils.h"
#include "json_utils.h"
#include "translation_loader.h"

#include "hv/json.hpp"

namespace helix {

const char* unit_color_region_name(UnitColorRegion region) {
    switch (region) {
    case UnitColorRegion::Main:
        return "main";
    case UnitColorRegion::Accent:
        return "accent";
    case UnitColorRegion::Panels:
        return "panels";
    }
    return "main";
}

std::optional<uint32_t> UnitColors::get(UnitColorRegion region) const {
    switch (region) {
    case UnitColorRegion::Main:
        return main;
    case UnitColorRegion::Accent:
        return accent;
    case UnitColorRegion::Panels:
        return panels;
    }
    return std::nullopt;
}

void UnitColors::set(UnitColorRegion region, std::optional<uint32_t> rgb) {
    if (rgb)
        rgb = *rgb & 0xFFFFFFu;
    switch (region) {
    case UnitColorRegion::Main:
        main = rgb;
        break;
    case UnitColorRegion::Accent:
        accent = rgb;
        break;
    case UnitColorRegion::Panels:
        panels = rgb;
        break;
    }
}

const std::vector<UnitColorPreset>& unit_color_presets() {
    static const std::vector<UnitColorPreset> kPresets{
        {"voron_black_red", TR_NOOP("Voron Black and Red"), 0x1C1C1C, 0xD7262E, 0x3A3A3A},
        {"turtle_green_gold", TR_NOOP("Box Turtle Green and Gold"), 0x2E7D3A, 0xD4A72C, 0x24302A},
        {"stealth_black", TR_NOOP("Stealth Black"), 0x1C1C1C, 0x3C3C3C, 0x2A2A2A},
        {"arctic_white", TR_NOOP("Arctic White"), 0xE8E8E8, 0x4A90D9, 0xBFC5CC},
    };
    return kPresets;
}

const UnitColorPreset* find_unit_color_preset(std::string_view id) {
    for (const UnitColorPreset& preset : unit_color_presets()) {
        if (id == preset.id)
            return &preset;
    }
    return nullptr;
}

UnitColors unit_colors_shown(const UnitColors& picked, const UnitColors& baked) {
    UnitColors shown;
    for (UnitColorRegion region : kUnitColorRegions)
        shown.set(region, picked.get(region) ? picked.get(region) : baked.get(region));
    return shown;
}

int unit_color_option_count() {
    return static_cast<int>(unit_color_presets().size()) + 2;
}

int unit_color_option(const UnitColors& colors) {
    if (colors.empty())
        return 0;
    const auto& presets = unit_color_presets();
    for (size_t i = 0; i < presets.size(); ++i) {
        if (colors == presets[i].colors())
            return static_cast<int>(i) + 1;
    }
    return unit_color_option_count() - 1;
}

std::optional<UnitColors> unit_colors_for_option(int option, const UnitColors& shown) {
    const auto& presets = unit_color_presets();
    if (option == 0)
        return UnitColors{};
    if (option >= 1 && option <= static_cast<int>(presets.size()))
        return presets[static_cast<size_t>(option - 1)].colors();
    if (option == unit_color_option_count() - 1)
        return shown;
    return std::nullopt;
}

nlohmann::json unit_colors_to_json(const UnitColors& colors) {
    nlohmann::json out = nlohmann::json::object();
    for (UnitColorRegion region : kUnitColorRegions) {
        if (const auto rgb = colors.get(region))
            out[unit_color_region_name(region)] = color_to_hex_string(*rgb);
    }
    return out;
}

UnitColors unit_colors_from_json(const nlohmann::json& value) {
    UnitColors colors;
    if (!value.is_object())
        return colors;
    for (UnitColorRegion region : kUnitColorRegions) {
        const auto it = value.find(unit_color_region_name(region));
        if (it == value.end() || !it->is_string())
            continue;
        uint32_t rgb = 0;
        if (parse_hex_color(json_util::as_string(*it).c_str(), rgb))
            colors.set(region, rgb);
    }
    return colors;
}

std::vector<UnitColorLayer> unit_color_layers(uint8_t baked_masks, const UnitColors& colors) {
    std::vector<UnitColorLayer> layers;
    if (baked_masks == 0 || colors.empty())
        return layers;
    for (UnitArtDepth depth : {UnitArtDepth::Back, UnitArtDepth::Front}) {
        for (UnitColorRegion region : kUnitColorRegions) {
            const auto rgb = colors.get(region);
            if (!rgb || (baked_masks & unit_color_mask_bit(depth, region)) == 0)
                continue;
            layers.push_back({depth, region, *rgb,
                              std::string(depth == UnitArtDepth::Back ? "back_" : "front_") +
                                  unit_color_region_name(region)});
        }
    }
    return layers;
}

} // namespace helix
