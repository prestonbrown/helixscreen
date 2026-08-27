// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bed_mesh_profile_name.h"

#include <algorithm>
#include <cctype>

namespace helix {
namespace ui {
namespace bed_mesh {

namespace {

/// Strip leading and trailing whitespace. A field the user tabbed through and
/// left holding spaces is not a name, and Klipper would happily store it.
std::string_view trim(std::string_view s) {
    const auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

ProfileNameCheck check_profile_name(std::string_view raw,
                                    const std::vector<std::string>& existing) {
    const std::string_view trimmed = trim(raw);
    if (trimmed.empty()) {
        return {ProfileNameVerdict::Empty, std::string()};
    }

    std::string name(trimmed);
    const bool clashes = std::find(existing.begin(), existing.end(), name) != existing.end();
    return {clashes ? ProfileNameVerdict::Overwrite : ProfileNameVerdict::New, std::move(name)};
}

} // namespace bed_mesh
} // namespace ui
} // namespace helix
