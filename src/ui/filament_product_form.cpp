// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_product_form.h"

#include "core/text_io.h"

#include <cctype>
#include <cstdlib>

namespace helix::ui {

namespace {

/// Lowercase @p s, collapse each run of non-alphanumeric characters to a single
/// '-', and trim leading/trailing hyphens. "Poly Maker / PLA" -> "poly-maker-pla".
std::string slugify(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool pending_sep = false;
    for (unsigned char c : s) {
        if (std::isalnum(c)) {
            if (pending_sep && !out.empty())
                out.push_back('-');
            pending_sep = false;
            out.push_back(static_cast<char>(std::tolower(c)));
        } else {
            pending_sep = true;
        }
    }
    return out;
}

bool has_value(const std::string& s) {
    return !helix::text_io::trim(s).empty();
}

} // namespace

std::string derive_product_id(const FilamentFormValues& v) {
    const std::string id(helix::text_io::trim(v.id));
    if (!id.empty())
        return id;
    // Fall back to a slug of brand + name.
    std::string combined(helix::text_io::trim(v.brand));
    const std::string name(helix::text_io::trim(v.name));
    if (!name.empty()) {
        if (!combined.empty())
            combined += ' ';
        combined += name;
    }
    return slugify(combined);
}

bool validate_product_form(const FilamentFormValues& v, std::string& error_out) {
    if (derive_product_id(v).empty()) {
        error_out = "Filament needs an ID (or a brand/name)";
        return false;
    }
    if (!has_value(v.type)) {
        error_out = "Filament type is required";
        return false;
    }
    if (has_value(v.nozzle_min) && has_value(v.nozzle_max)) {
        const int lo = std::atoi(std::string(helix::text_io::trim(v.nozzle_min)).c_str());
        const int hi = std::atoi(std::string(helix::text_io::trim(v.nozzle_max)).c_str());
        if (lo > hi) {
            error_out = "Min nozzle temp is higher than max";
            return false;
        }
    }
    return true;
}

nlohmann::json build_product_json(const FilamentFormValues& v) {
    nlohmann::json j = nlohmann::json::object();

    // Required identity, always present.
    j["id"] = derive_product_id(v);
    j["type"] = std::string(helix::text_io::trim(v.type));

    // Optional strings: only when the user entered something.
    if (has_value(v.brand))
        j["brand"] = std::string(helix::text_io::trim(v.brand));
    if (has_value(v.name))
        j["name"] = std::string(helix::text_io::trim(v.name));

    // Optional numerics. A blank field is OMITTED (never emitted as 0) so it
    // cannot clobber the value inherited from the material type or the shipped
    // catalog when this sparse object is merge-patched over an existing entry.
    // JSON keys intentionally differ from the EffectiveFilament struct names.
    if (has_value(v.nozzle_min))
        j["nozzle_min"] = std::atoi(std::string(helix::text_io::trim(v.nozzle_min)).c_str());
    if (has_value(v.nozzle_max))
        j["nozzle_max"] = std::atoi(std::string(helix::text_io::trim(v.nozzle_max)).c_str());
    if (has_value(v.nozzle))
        j["nozzle"] =
            std::atoi(std::string(helix::text_io::trim(v.nozzle)).c_str()); // recommended nozzle
    if (has_value(v.bed))
        j["bed"] = std::atoi(std::string(helix::text_io::trim(v.bed)).c_str()); // bed_temp
    if (has_value(v.density))
        j["density"] = static_cast<float>(
            std::atof(std::string(helix::text_io::trim(v.density)).c_str())); // density_g_cm3

    j["source"] = "user";
    return j;
}

} // namespace helix::ui
