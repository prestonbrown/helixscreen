// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spool_assign.h"

#include "color_utils.h"
#include "core/text_io.h"
#include "filament_mapper.h"
#include "filament_variants.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace helix::spool_assign {

namespace {

bool brand_set(const std::string& brand) {
    return !brand.empty() && !helix::text_io::equals_ignore_case(brand, "Generic");
}

std::vector<std::string> words_of(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '+') {
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty()) {
        words.push_back(std::move(word));
    }
    return words;
}

/// Every word of the shorter text is a word of the longer: "White" and
/// "Polar White".
bool words_contained(const std::string& a, const std::string& b) {
    std::vector<std::string> wa = words_of(a);
    std::vector<std::string> wb = words_of(b);
    if (wa.empty() || wb.empty()) {
        return false;
    }
    if (wa.size() > wb.size()) {
        std::swap(wa, wb);
    }
    return std::all_of(wa.begin(), wa.end(), [&wb](const std::string& w) {
        return std::find(wb.begin(), wb.end(), w) != wb.end();
    });
}

enum class FieldMatch { None, Close, Exact };

FieldMatch match_text(const std::string& wanted, const std::string& have) {
    if (have.empty()) {
        return FieldMatch::None;
    }
    if (helix::text_io::equals_ignore_case(wanted, have)) {
        return FieldMatch::Exact;
    }
    return words_contained(wanted, have) ? FieldMatch::Close : FieldMatch::None;
}

FieldMatch match_material(const std::string& wanted, const std::string& have) {
    if (have.empty()) {
        return FieldMatch::None;
    }
    if (helix::text_io::equals_ignore_case(wanted, have)) {
        return FieldMatch::Exact;
    }
    return filament::materials_compatible(wanted, have) ? FieldMatch::Close : FieldMatch::None;
}

FieldMatch match_color(uint32_t wanted, const SpoolInfo& spool) {
    // A multi-color spool states its colors in multi_color_hexes alone; the
    // first is the one it shows first.
    std::string hex = spool.color_hex;
    if (hex.empty()) {
        hex = spool.multi_color_hexes.substr(0, spool.multi_color_hexes.find(','));
    }
    const std::optional<uint32_t> have = helix::parse_hex_color(hex);
    if (!have) {
        return FieldMatch::None;
    }
    const int distance = FilamentMapper::color_distance(wanted, *have);
    if (distance <= kExactColorDistance) {
        return FieldMatch::Exact;
    }
    return distance <= kCloseColorDistance ? FieldMatch::Close : FieldMatch::None;
}

} // namespace

bool SpoolQuery::empty() const {
    return material.empty() && !brand_set(brand) && product.empty() && !color.has_value();
}

SpoolMatch match_spool(const SpoolQuery& query, const SpoolInfo& spool) {
    SpoolMatch result;
    bool close = false;
    // Each set field must at least loosely match; one that only loosely
    // matches makes the whole spool close and is named in the note.
    auto take = [&close](FieldMatch m, bool& differs) {
        if (m == FieldMatch::None) {
            return false;
        }
        if (m == FieldMatch::Close) {
            differs = true;
            close = true;
        }
        return true;
    };
    if (!query.material.empty() &&
        !take(match_material(query.material, spool.material), result.differs.material)) {
        return {};
    }
    if (brand_set(query.brand) &&
        !take(match_text(query.brand, spool.vendor), result.differs.brand)) {
        return {};
    }
    if (!query.product.empty() &&
        !take(match_text(query.product, spool.filament_name), result.differs.product)) {
        return {};
    }
    if (query.color && !take(match_color(*query.color, spool), result.differs.color)) {
        return {};
    }
    result.kind = close ? MatchKind::Close : MatchKind::Exact;
    return result;
}

std::vector<RankedSpool> rank_spools(const SpoolQuery& query, const std::vector<SpoolInfo>& spools,
                                     size_t max) {
    std::vector<RankedSpool> exact;
    std::vector<RankedSpool> close;
    const bool everything = query.empty();
    for (size_t i = 0; i < spools.size(); ++i) {
        if (everything) {
            exact.push_back({i, {MatchKind::Exact, {}}});
            continue;
        }
        const SpoolMatch m = match_spool(query, spools[i]);
        if (m.kind == MatchKind::Exact) {
            exact.push_back({i, m});
        } else if (m.kind == MatchKind::Close) {
            close.push_back({i, m});
        }
    }
    exact.insert(exact.end(), close.begin(), close.end());
    if (exact.size() > max) {
        exact.resize(max);
    }
    return exact;
}

std::string close_note(const CloseFields& differs, const SpoolInfo& spool,
                       const std::string& note_format, const std::string& color_phrase) {
    if (!differs.any()) {
        return {};
    }
    std::vector<std::string> parts;
    if (differs.material) {
        parts.push_back(spool.material);
    }
    if (differs.brand) {
        parts.push_back(spool.vendor);
    }
    if (differs.product) {
        parts.push_back(spool.filament_name);
    }
    if (differs.color) {
        parts.push_back(color_phrase);
    }
    std::string listed;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            listed += ", ";
        }
        listed += parts[i];
    }
    return fmt::format(fmt::runtime(note_format), listed);
}

const std::vector<CommonSpool>& common_spools() {
    // The General palette's black, white and gray, so a pick rings its swatch.
    static const std::vector<CommonSpool> kCommon = [] {
        std::vector<CommonSpool> out;
        const char* const materials[] = {"PLA", "PETG", "ABS", "ASA"};
        const struct {
            const char* name;
            uint32_t rgb;
        } colors[] = {{"Black", 0x1A1A1A}, {"White", 0xFFFFFF}, {"Gray", 0x7F7F7F}};
        for (const char* material : materials) {
            for (const auto& color : colors) {
                out.push_back({material, color.name, color.rgb});
            }
        }
        return out;
    }();
    return kCommon;
}

std::string spool_tile_label(const std::string& vendor, const std::string& name) {
    // A product named after its maker ("Bambu ASA") says the vendor already.
    // So does one led by the maker's first word: "Bambu Lab" makes "Bambu ASA".
    const auto led_by = [&name](const std::string& word) {
        return !word.empty() && name.size() > word.size() && name[word.size()] == ' ' &&
               helix::text_io::equals_ignore_case(name.substr(0, word.size()), word);
    };
    if (vendor.empty() || led_by(vendor) || led_by(vendor.substr(0, vendor.find(' ')))) {
        return name;
    }
    return name.empty() ? vendor : vendor + " " + name;
}

std::vector<SpoolTile> spool_tiles(const std::vector<FavoriteTile>& favorites, size_t count) {
    std::vector<SpoolTile> tiles;
    for (const auto& fav : favorites) {
        if (tiles.size() >= count) {
            return tiles;
        }
        tiles.push_back({spool_tile_label(fav.brand, fav.name), 0, fav.product_id, 0});
    }
    // common_spools() is material-major with three colors each; the grid reads
    // a color at a time.
    const auto& common = common_spools();
    constexpr size_t kColors = 3;
    const size_t materials = common.size() / kColors;
    for (size_t c = 0; c < kColors; ++c) {
        for (size_t m = 0; m < materials && tiles.size() < count; ++m) {
            const size_t index = m * kColors + c;
            tiles.push_back({spool_tile_label("Generic", common[index].material),
                             common[index].rgb,
                             {},
                             index});
        }
    }
    return tiles;
}

bool show_common_spools(size_t spool_count) {
    return spool_count < kFewSpools;
}

bool product_names_material(const std::string& product, const std::string& material) {
    if (material.empty() || product.size() < material.size()) {
        return false;
    }
    const auto word_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    const std::string hay = helix::text_io::to_lower(product);
    const std::string needle = helix::text_io::to_lower(material);
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) {
        const size_t end = at + needle.size();
        const bool starts_word = at == 0 || !word_char(hay[at - 1]) || !word_char(needle.front());
        const bool ends_word =
            end == hay.size() || !word_char(hay[end]) || !word_char(needle.back());
        if (starts_word && ends_word) {
            return true;
        }
    }
    return false;
}

LaneScreen screen_for_lane(const SlotInfo& lane) {
    if (lane.status == SlotStatus::EMPTY) {
        return LaneScreen::Assign;
    }
    return (lane.spoolman_id > 0 || !lane.material.empty()) ? LaneScreen::Edit : LaneScreen::Assign;
}

BottomAction bottom_action(bool spoolman_connected, size_t listed) {
    if (!spoolman_connected) {
        return BottomAction::AssignLocal;
    }
    return listed > 0 ? BottomAction::AddNewSecondary : BottomAction::AddToSpoolmanAndUse;
}

int size_chip_for(float remaining_g, float total_g) {
    if (std::fabs(remaining_g - total_g) < 0.5f) {
        for (int i = 0; i < kPartialChip; ++i) {
            if (std::fabs(total_g - kSizeChipGrams[i]) < 0.5f) {
                return i;
            }
        }
    }
    return kPartialChip;
}

SpoolSize size_for_reading(float remaining_g, float total_g) {
    if (remaining_g < 0 && total_g <= 0) {
        return {};
    }
    const float remaining = remaining_g >= 0 ? remaining_g : total_g;
    const float total =
        total_g > 0 ? std::max(total_g, remaining) : std::max(remaining, kDefaultSpoolGrams);
    return {size_chip_for(remaining, total), remaining, total};
}

float step_remaining(float remaining_g, float delta_g, float total_g) {
    float next = std::max(0.0f, remaining_g + delta_g);
    if (total_g > 0) {
        next = std::min(next, total_g);
    }
    return next;
}

bool undo_still_applies(const SlotInfo& now, const SlotInfo& after) {
    return now.spoolman_id == after.spoolman_id && now.material == after.material &&
           now.brand == after.brand && now.color_rgb == after.color_rgb;
}

bool save_changed_lane(const SlotInfo& before, const SlotInfo& after) {
    constexpr float kWeightEpsilonG = 0.5f;
    return !undo_still_applies(after, before) || before.product_name != after.product_name ||
           std::fabs(before.remaining_weight_g - after.remaining_weight_g) > kWeightEpsilonG ||
           std::fabs(before.total_weight_g - after.total_weight_g) > kWeightEpsilonG;
}

} // namespace helix::spool_assign
