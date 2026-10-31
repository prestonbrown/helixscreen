// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace helix {

// Orca ids are opaque tokens, never numeric Spoolman record ids.
inline bool is_orca_filament_id(std::string_view id) {
    bool has_letter = false;
    for (const char c : id) {
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!letter && !(c >= '0' && c <= '9') && c != '_' && c != '-')
            return false;
        has_letter = has_letter || letter;
    }
    return has_letter;
}

} // namespace helix
