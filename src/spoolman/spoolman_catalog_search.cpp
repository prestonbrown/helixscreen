// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spoolman_catalog_search.h"

#include "color_utils.h"
#include "core/text_io.h"
#include "filament_mapper.h"
#include "filament_variants.h"
#include "moonraker_error.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace helix {

namespace {

struct CachedAvailability {
    uint64_t connection_generation = 0;
    bool available = false;
};

std::optional<CachedAvailability> s_cached;

} // namespace

SpoolmanCatalogSearch::Availability
SpoolmanCatalogSearch::availability(uint64_t connection_generation) {
    if (!s_cached || s_cached->connection_generation != connection_generation) {
        return Availability::Unknown;
    }
    return s_cached->available ? Availability::Available : Availability::Unavailable;
}

void SpoolmanCatalogSearch::record_success(uint64_t connection_generation) {
    s_cached = CachedAvailability{connection_generation, true};
}

void SpoolmanCatalogSearch::record_error(uint64_t connection_generation,
                                         const MoonrakerError& err) {
    if (err.is_not_found()) {
        s_cached = CachedAvailability{connection_generation, false};
    }
}

void SpoolmanCatalogSearch::reset_cache() {
    s_cached.reset();
}

bool SpoolmanCatalogSearch::is_searchable(const std::string& query) {
    return text_io::trim(query).size() >= kMinQueryLength;
}

uint64_t SpoolmanCatalogSearch::begin() {
    return ++generation_;
}

namespace spoolman {

std::string normalize_color_hex(const std::string& hex) {
    std::string s = hex;
    if (!s.empty() && s[0] == '#') {
        s.erase(0, 1);
    }
    if (s.size() != 6 && s.size() != 8) {
        return "";
    }
    for (char& c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return "";
        }
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

namespace {

/// Whether a token of @p text carrying @p polymer also carries a '+'
/// ("ABS+", "ABS+HS", "PLA+").
bool states_plus_grade(const std::string& text, const std::string& polymer) {
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t end = std::min(text.find(' ', pos), text.size());
        const std::string_view token(text.data() + pos, end - pos);
        if (token.find('+') != std::string_view::npos &&
            text_io::contains_ignore_case(token, polymer)) {
            return true;
        }
        pos = end + 1;
    }
    return false;
}

} // namespace

bool is_filament_of_type(const FilamentInfo& filament, const std::string& precise_type) {
    const std::string type(text_io::trim(precise_type));
    const std::string recorded = filament::slicer_material(type);
    if (recorded.empty() || filament::slicer_material(filament.material) != recorded) {
        return false;
    }
    const std::string text = filament.material + " " + filament.filament_name;
    const std::string polymer = filament::extract_base_material(type);
    return filament::name_grades_match(text, type) &&
           filament_finish_of(text) == filament_finish_of(type) &&
           states_plus_grade(text, polymer) == states_plus_grade(type, polymer);
}

const FilamentInfo* find_matching_filament(const std::vector<FilamentInfo>& filaments,
                                           const std::string& precise_type,
                                           const std::string& color_hex) {
    const std::string needle = normalize_color_hex(color_hex);
    if (needle.empty()) {
        return nullptr;
    }
    for (const auto& f : filaments) {
        if (normalize_color_hex(f.color_hex) == needle && is_filament_of_type(f, precise_type)) {
            return &f;
        }
    }
    return nullptr;
}

std::string new_filament_name(const std::string& product, const std::string& color_name) {
    const std::string head(text_io::trim(product));
    const std::string tail(text_io::trim(color_name));
    if (head.empty() || tail.empty()) {
        return head.empty() ? tail : head;
    }
    return head + " " + tail;
}

int temperature_for_range(int min_temp, int max_temp) {
    if (min_temp > 0 && max_temp > 0) {
        return (min_temp + max_temp) / 2;
    }
    return std::max({min_temp, max_temp, 0});
}

const FilamentInfo* find_similar_filament(const std::vector<FilamentInfo>& filaments,
                                          const std::string& material,
                                          const std::string& color_hex) {
    const std::string needle = normalize_color_hex(color_hex);
    if (needle.empty() || material.empty()) {
        return nullptr;
    }
    // The first six digits are the colour; an alpha byte does not move it.
    const auto wanted = parse_hex_color(needle.substr(0, 6));
    if (!wanted) {
        return nullptr;
    }
    const FilamentInfo* best = nullptr;
    int best_distance = FilamentMapper::kColorMatchTolerance + 1;
    for (const auto& f : filaments) {
        const std::string hex = normalize_color_hex(f.color_hex);
        if (hex.empty() || filament::slicer_material(f.material) != material) {
            continue;
        }
        const auto rgb = parse_hex_color(hex.substr(0, 6));
        if (!rgb) {
            continue;
        }
        const int distance = FilamentMapper::color_distance(*wanted, *rgb);
        if (distance < best_distance) {
            best = &f;
            best_distance = distance;
        }
    }
    return best;
}

namespace {

std::string folded(const std::string& s) {
    return text_io::to_lower(std::string(text_io::trim(s)));
}

/// The normalized colours in a comma-separated list, sorted; empty when any
/// entry is not a colour.
std::vector<std::string> color_set(const std::string& colors) {
    std::vector<std::string> set;
    size_t pos = 0;
    while (pos <= colors.size()) {
        const size_t comma = colors.find(',', pos);
        const std::string part =
            colors.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const std::string hex = normalize_color_hex(std::string(text_io::trim(part)));
        if (hex.empty()) {
            return {};
        }
        set.push_back(hex);
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    std::sort(set.begin(), set.end());
    return set;
}

} // namespace

const FilamentInfo* find_catalog_filament(const std::vector<FilamentInfo>& filaments, int vendor_id,
                                          const std::string& name, const std::string& material,
                                          const std::string& colors) {
    const std::vector<std::string> wanted = color_set(colors);
    if (wanted.empty()) {
        return nullptr;
    }
    const std::string wanted_name = folded(name);
    const std::string wanted_material = folded(material);
    for (const auto& f : filaments) {
        const std::string& theirs = f.multi_color_hexes.empty() ? f.color_hex : f.multi_color_hexes;
        if (f.vendor_id == vendor_id && folded(f.filament_name) == wanted_name &&
            folded(f.material) == wanted_material && color_set(theirs) == wanted) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace spoolman
} // namespace helix
