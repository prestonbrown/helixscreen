// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The nav bar's inactive icon must never render at or above the active icon's
// size. Both sides are responsive ladders in ui_xml/navigation_bar.xml and
// ui_xml/globals.xml, so the relationship holds only by agreement between two
// separately edited tables. Retuning one without the other reads as correct in
// isolation and produces a nav bar with no size differentiation at all.

#include "theme_manager.h"

#include <string>
#include <unordered_map>

#include "../catch_amalgamated.hpp"

namespace {

const char* SUFFIXES[] = {"_micro", "_tiny", "_small", "_medium", "_large", "_xlarge", "_xxlarge"};

// Ordering of the icon size rungs. A higher index is a larger glyph.
int rung_index(const std::string& rung) {
    if (rung == "xs")
        return 0;
    if (rung == "sm")
        return 1;
    if (rung == "md")
        return 2;
    if (rung == "lg")
        return 3;
    if (rung == "xl")
        return 4;
    return -1;
}

std::string lookup(const std::unordered_map<std::string, std::string>& m, const char* key) {
    auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

} // namespace

TEST_CASE("nav inactive icon size stays below the active icon size", "[theme][navbar]") {
    for (const char* suffix : SUFFIXES) {
        auto strings = theme_manager_parse_all_xml_for_suffix("ui_xml", "string", suffix);

        const std::string active = lookup(strings, "icon_size");
        const std::string inactive = lookup(strings, "icon_size_nav_inactive");

        INFO("suffix=" << suffix << " icon_size=" << active
                       << " icon_size_nav_inactive=" << inactive);

        REQUIRE_FALSE(active.empty());
        REQUIRE_FALSE(inactive.empty());

        const int active_rung = rung_index(active);
        const int inactive_rung = rung_index(inactive);
        REQUIRE(active_rung >= 0);
        REQUIRE(inactive_rung >= 0);

        if (std::string(suffix) == "_micro") {
            // micro is the deliberate exception. One rung below md is sm, which
            // resolves to a 16px face, and a 16px outline glyph has strokes too
            // thin to read inside a 42px nav strip. It matches instead, and the
            // glyph and color carry the differentiation there.
            REQUIRE(inactive_rung == active_rung);
        } else {
            REQUIRE(inactive_rung < active_rung);
        }
    }
}
