// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "color_recents.h"

#include "color_utils.h"
#include "settings_manager.h"

#include <algorithm>
#include <string>

namespace helix {

namespace {

const char* key_of(ColorRecents::Kind kind) {
    return kind == ColorRecents::Kind::Led ? "led" : "filament";
}

void store(ColorRecents::Kind kind, const std::vector<uint32_t>& colors) {
    std::vector<std::string> hexes;
    hexes.reserve(colors.size());
    for (uint32_t c : colors) {
        hexes.push_back(color_to_hex_string(c));
    }
    SettingsManager::instance().set_color_recents(key_of(kind), hexes);
}

} // namespace

const std::vector<uint32_t>& ColorRecents::seed(Kind kind) {
    // Black, white, gray, red, orange, yellow, green, blue.
    static const std::vector<uint32_t> filament = {0x1A1A1A, 0xFFFFFF, 0x7F7F7F, 0xE53935,
                                                   0xFF9800, 0xFFEB3B, 0x43A047, 0x1E88E5};
    // Warm white (2700 K), white, red, cyan, purple.
    static const std::vector<uint32_t> led = {0xFFA757, 0xFFFFFF, 0xFF0000, 0x00FFFF, 0x8000FF};
    return kind == Kind::Led ? led : filament;
}

std::vector<uint32_t> ColorRecents::list(Kind kind) {
    const auto stored = SettingsManager::instance().get_color_recents(key_of(kind));
    if (!stored) {
        return seed(kind);
    }
    std::vector<uint32_t> out;
    for (const auto& hex : *stored) {
        const auto rgb = parse_hex_color(hex);
        if (rgb && std::find(out.begin(), out.end(), *rgb) == out.end()) {
            out.push_back(*rgb);
        }
        if (out.size() == kCap) {
            break;
        }
    }
    return out;
}

void ColorRecents::remember(Kind kind, uint32_t rgb) {
    rgb &= 0xFFFFFF;
    auto colors = list(kind);
    colors.erase(std::remove(colors.begin(), colors.end(), rgb), colors.end());
    colors.insert(colors.begin(), rgb);
    if (colors.size() > kCap) {
        colors.resize(kCap);
    }
    store(kind, colors);
}

void ColorRecents::remove(Kind kind, uint32_t rgb) {
    auto colors = list(kind);
    colors.erase(std::remove(colors.begin(), colors.end(), rgb & 0xFFFFFF), colors.end());
    store(kind, colors);
}

} // namespace helix
