// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_3mf_extract.h"

#include "core/gcode_filename.h"
#include "three_mf.h"

#include <algorithm>
#include <cctype>

namespace helix::firmware_3mf {

namespace {

/// One firmware's extract layout.
struct ExtractLayout {
    const char* root;         ///< Moonraker root the extract lands in
    const char* plate_prefix; ///< plate-numbered extracts: prefix, then the plate
    const char* plate_suffix; ///< ...and this suffix
};

// QIDI's Moonraker (Q2 and its siblings) extracts the printing plate into the
// hidden `.temp` root, as shadow_native_plate_<N>.gcode or as the archive's
// own stem. This table is the one place that layout is named.
constexpr ExtractLayout LAYOUT{".temp", "shadow_native_plate_", ".gcode"};

} // namespace

const char* extract_root() {
    return LAYOUT.root;
}

bool is_plate_extract(const std::string& name) {
    const std::string prefix = LAYOUT.plate_prefix;
    const std::string suffix = LAYOUT.plate_suffix;
    // At least one character between the prefix and suffix (the plate id).
    if (name.size() <= prefix.size() + suffix.size()) {
        return false;
    }
    if (name.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    return name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string extract_name(const std::string& print_filename) {
    return gcode::get_filename_basename(three_mf::extracted_gcode_name(print_filename, 0, false));
}

bool is_extract(const std::string& entry, const std::string& print_filename) {
    if (is_plate_extract(entry)) {
        return true;
    }
    const std::string name = extract_name(print_filename);
    return entry.size() == name.size() && std::equal(entry.begin(), entry.end(), name.begin(),
                                                     [](unsigned char a, unsigned char b) {
                                                         return std::tolower(a) == std::tolower(b);
                                                     });
}

} // namespace helix::firmware_3mf
