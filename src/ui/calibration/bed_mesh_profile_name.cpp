// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bed_mesh_profile_name.h"

#include "core/text_io.h"

#include <algorithm>
#include <cctype>

namespace helix {
namespace ui {
namespace bed_mesh {

ProfileNameCheck check_profile_name(std::string_view raw,
                                    const std::vector<std::string>& existing) {
    // A field the user tabbed through and left holding spaces is not a name.
    const std::string_view trimmed = helix::text_io::trim(raw);
    if (trimmed.empty()) {
        return {ProfileNameVerdict::Empty, std::string()};
    }

    std::string name(trimmed);
    // Klipper ends a command at ';' before reading its parameters, and a line break
    // starts another command, so no quoting carries either inside a name.
    if (name.find_first_of(";\r\n") != std::string::npos) {
        return {ProfileNameVerdict::Unusable, std::move(name)};
    }
    const bool clashes = std::find(existing.begin(), existing.end(), name) != existing.end();
    return {clashes ? ProfileNameVerdict::Overwrite : ProfileNameVerdict::New, std::move(name)};
}

} // namespace bed_mesh
} // namespace ui
} // namespace helix
