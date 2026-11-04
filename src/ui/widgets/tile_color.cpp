// SPDX-License-Identifier: GPL-3.0-or-later
#include "tile_color.h"

#include "core/static_subject_registry.h"
#include "helix/xml/indexed_subject_pool.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"

#include <algorithm>
#include <iterator>
#include <memory>

#include "hv/json.hpp"

namespace helix::tile_color {

namespace {

constexpr uint32_t kPalette[] = {
    0xE53935, // Red
    0xFF5722, // Deep Orange
    0xFF9800, // Orange
    0xFFC107, // Amber
    0xFFEB3B, // Yellow
    0x8BC34A, // Lime
    0x43A047, // Green
    0x009688, // Teal
    0x00BCD4, // Cyan
    0x1E88E5, // Blue
    0x3F51B5, // Indigo
    0x7B1FA2, // Purple
    0xE91E63, // Pink
    0xFFFFFF, // White
    0x808080, // Gray
    0x000000, // no colour: the tile's theme colours
};
constexpr size_t kPaletteSize = std::size(kPalette);

/// Share of the user's colour left in the muted mix (255 = all of it).
constexpr lv_opa_t kMutedShare = 110;

/// Index of the ringed swatch, or -1.
lv_subject_t s_selected_subject{};
/// tile_color_grid's swatches bind tile_color_swatch_color_<i>.
std::unique_ptr<helix::xml::IndexedSubjectPool> s_color_pool;
bool s_registered = false;
SubjectManager s_subjects;

} // namespace

size_t palette_size() {
    return kPaletteSize;
}

uint32_t palette_at(int index) {
    if (index < 0 || static_cast<size_t>(index) >= kPaletteSize) {
        return 0;
    }
    return kPalette[index];
}

int palette_index(uint32_t rgb) {
    const auto* it = std::find(std::begin(kPalette), std::end(kPalette), rgb);
    return it == std::end(kPalette) ? -1 : static_cast<int>(it - std::begin(kPalette));
}

uint32_t color_from_config(const nlohmann::json& config) {
    if (!config.is_object() || !config.contains("color") || !config["color"].is_number_integer()) {
        return 0;
    }
    const int64_t v = config["color"].get<int64_t>();
    return (v >= 0 && v <= 0xFFFFFF) ? static_cast<uint32_t>(v) : 0;
}

Paint paint_for(uint32_t rgb, TileState state) {
    if (rgb == 0) {
        return Paint::Theme;
    }
    switch (state) {
    case TileState::Active:
        return Paint::Custom;
    case TileState::Inactive:
        return Paint::CustomMuted;
    case TileState::Unconfigured:
    case TileState::Locked:
        break;
    }
    return Paint::Theme;
}

lv_color_t muted(lv_color_t custom, lv_color_t muted_ref) {
    return lv_color_mix(custom, muted_ref, kMutedShare);
}

void register_subjects() {
    if (s_registered) {
        return;
    }
    lv_subject_init_int(&s_selected_subject, -1);
    s_color_pool = std::make_unique<helix::xml::IndexedSubjectPool>(
        "tile_color_swatch_color", helix::xml::IndexedSubjectPool::Type::Color);
    s_color_pool->ensure_size(kPaletteSize);
    s_subjects.publish("tile_color_selected", &s_selected_subject);
    // Registered once with the widget factories, so it outlives a printer switch.
    StaticSubjectRegistry::instance().register_deinit(
        "TileColorSubjects",
        []() {
            s_color_pool.reset();
            s_subjects.deinit_all();
            s_registered = false;
        },
        StaticSubjectRegistry::Lifetime::Process);
    s_registered = true;
}

void publish_palette(uint32_t rgb) {
    if (!s_registered) {
        return;
    }
    for (size_t i = 0; i < kPaletteSize; ++i) {
        const uint32_t swatch =
            kPalette[i] == 0 ? lv_color_to_u32(theme_manager_get_color("secondary")) & 0xFFFFFF
                             : kPalette[i];
        s_color_pool->set_color(i, swatch);
    }
    publish_selection(rgb);
}

void publish_selection(uint32_t rgb) {
    if (!s_registered) {
        return;
    }
    lv_subject_set_int(&s_selected_subject, palette_index(rgb));
}

} // namespace helix::tile_color
