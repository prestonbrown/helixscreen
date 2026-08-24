// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_display_name.h"

#include "operation_patterns.h"

#include <cctype>
#include <initializer_list>

namespace helix {

namespace {

/// Characters that bind into a word for boundary purposes.
///
/// Alphanumerics plus the glue real material names use: "PLA+", "PA6-CF",
/// "TPU_95A". Treating those as word characters is what stops material "PLA"
/// from being stripped out of "Ambrosia Pink PLA+" and brand "Poly" from being
/// stripped out of "Polymaker".
bool is_word_char(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '+' || c == '-' || c == '_' || c == '#';
}

char ascii_lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// Collapse every run of whitespace to one space and trim both ends.
std::string normalize_ws(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    bool space_pending = false;
    for (char ch : in) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            space_pending = !out.empty();
            continue;
        }
        if (space_pending) {
            out.push_back(' ');
            space_pending = false;
        }
        out.push_back(ch);
    }
    return out;
}

/// Case-insensitive search for `needle` in `haystack` delimited by word
/// boundaries on both sides. An empty needle never matches.
bool contains_word(const std::string& haystack, const std::string& needle) {
    if (needle.empty() || needle.size() > haystack.size()) {
        return false;
    }
    const size_t last = haystack.size() - needle.size();
    for (size_t i = 0; i <= last; ++i) {
        bool equal = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (ascii_lower(haystack[i + j]) != ascii_lower(needle[j])) {
                equal = false;
                break;
            }
        }
        if (!equal) {
            continue;
        }
        const bool left_boundary = (i == 0) || !is_word_char(haystack[i - 1]);
        const size_t end = i + needle.size();
        const bool right_boundary = (end == haystack.size()) || !is_word_char(haystack[end]);
        if (left_boundary && right_boundary) {
            return true;
        }
    }
    return false;
}

/// Does `name` already say what `material` says?
///
/// Materials are a small closed vocabulary, so containment really does imply
/// redundancy: "ePLA", "PLA+", "ABS+" and "HIPLA" all mean "this is that
/// material". Brands get no such treatment — they are arbitrary strings, and
/// "Sun" must never be stripped out of "Sunlu".
///
/// Matching is token-wise because Spoolman materials are often compound:
/// material "Silk PLA" against name "Silk Blue ePLA" has both of its words
/// present ("Silk" outright, "PLA" inside "ePLA"), so appending it would give
/// "eSUN Silk Blue ePLA Silk PLA" — redundant in a way no human would write.
/// A material with no overlap ("PLA" against "Ambrosia Pink") is still added,
/// which is what keeps the material on names that omit it.
bool material_is_redundant(const std::string& name, const std::string& material) {
    if (material.empty()) {
        return false;
    }
    size_t i = 0;
    bool saw_token = false;
    while (i < material.size()) {
        while (i < material.size() && std::isspace(static_cast<unsigned char>(material[i])) != 0) {
            ++i;
        }
        const size_t start = i;
        while (i < material.size() && std::isspace(static_cast<unsigned char>(material[i])) == 0) {
            ++i;
        }
        if (start == i) {
            continue;
        }
        saw_token = true;
        if (!contains_ci(name, material.substr(start, i - start))) {
            return false;
        }
    }
    return saw_token;
}

/// First candidate that is not blank, normalized. Blank is the unset sentinel
/// for every string field in the slot/override merge policy.
std::string first_non_blank(std::initializer_list<std::string_view> candidates) {
    for (std::string_view candidate : candidates) {
        std::string normalized = normalize_ws(candidate);
        if (!normalized.empty()) {
            return normalized;
        }
    }
    return {};
}

} // namespace

std::string compose_filament_label(std::string_view brand, std::string_view name,
                                   std::string_view material) {
    const std::string brand_part = normalize_ws(brand);
    const std::string name_part = normalize_ws(name);
    const std::string material_part = normalize_ws(material);

    // Brand keeps word-boundary matching (arbitrary strings — "Sun" must not
    // match inside "Sunlu"); material uses containment (closed vocabulary).
    const bool brand_redundant = contains_word(name_part, brand_part);
    const bool material_redundant = material_is_redundant(name_part, material_part);

    std::string label;
    const auto append = [&label](const std::string& part) {
        if (part.empty()) {
            return;
        }
        if (!label.empty()) {
            label.push_back(' ');
        }
        label += part;
    };

    if (!brand_redundant) {
        append(brand_part);
    }
    append(name_part);
    if (!material_redundant) {
        append(material_part);
    }
    return label;
}

FilamentLabelParts resolve_filament_label_parts(const SlotInfo& slot, const SpoolIdentity* identity,
                                                std::string_view color_fallback) {
    // A record carrying only ids or a color hex cannot name anything, so it is
    // indistinguishable from a cache miss here.
    const SpoolIdentity* id = (identity != nullptr && identity->valid()) ? identity : nullptr;

    const std::string_view id_vendor = id ? std::string_view(id->vendor) : std::string_view();
    const std::string_view id_name = id ? std::string_view(id->filament_name) : std::string_view();
    const std::string_view id_material = id ? std::string_view(id->material) : std::string_view();

    // Per-field precedence. The slot layer (user override already merged over
    // firmware by apply_overrides) wins field by field; the Spoolman identity
    // fills only the gaps. color_fallback is the algorithmic color name, which
    // names nothing about the filament and so sits below every real name.
    FilamentLabelParts parts;
    parts.brand = first_non_blank({slot.brand, id_vendor});
    parts.material = first_non_blank({slot.material, id_material});
    parts.name = first_non_blank({slot.spool_name, id_name, slot.color_name, color_fallback});
    return parts;
}

std::string resolve_filament_label(const SlotInfo& slot, const SpoolIdentity* identity,
                                   std::string_view color_fallback, std::string_view last_resort) {
    const FilamentLabelParts parts = resolve_filament_label_parts(slot, identity, color_fallback);

    std::string label = compose_filament_label(parts.brand, parts.name, parts.material);
    if (label.empty()) {
        label = normalize_ws(last_resort);
    }
    if (label.empty()) {
        label = "Filament";
    }
    return label;
}

} // namespace helix
