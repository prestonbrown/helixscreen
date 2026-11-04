// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spoolman_catalog_search.h"

#include "core/text_io.h"
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

const FilamentInfo* find_matching_filament(const std::vector<FilamentInfo>& filaments,
                                           const std::string& material,
                                           const std::string& color_hex) {
    const std::string needle = normalize_color_hex(color_hex);
    if (needle.empty()) {
        return nullptr;
    }
    for (const auto& f : filaments) {
        if (f.material == material && normalize_color_hex(f.color_hex) == needle) {
            return &f;
        }
    }
    return nullptr;
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
