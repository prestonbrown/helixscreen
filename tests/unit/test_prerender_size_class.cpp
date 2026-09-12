// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_prerender_size_class.cpp
 * @brief The compiled size-class ladder must agree with the manifest's
 *
 * assets/config/platforms.json carries the pre-render size-class ladder as data,
 * and scripts/platform_manifest.py evaluates it to decide which splash classes a
 * package contains. src/system/prerender_size_class.cpp is the compiled copy the
 * app and the splash binary use at runtime, and it derives its answer from
 * breakpoint_for() in ui_breakpoint.h.
 *
 * That is three statements of one rule - JSON, C++ selector, breakpoint ladder -
 * and they agree by convention until they silently do not. These tests compare
 * all three: the selector against the JSON across a resolution grid, and the
 * JSON's thresholds against the UI_BREAKPOINT_*_MAX macros they were copied from.
 */

#include "../../include/prerender_size_class.h"
#include "../../include/ui_breakpoint.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

json load_manifest() {
    std::filesystem::path path =
        std::filesystem::current_path() / "assets" / "config" / "platforms.json";
    INFO("manifest path: " << path.string());
    REQUIRE(std::filesystem::exists(path));
    std::ifstream f(path);
    REQUIRE(f.good());
    json j;
    f >> j;
    return j;
}

/// Evaluate the ladder from the manifest: first rule whose bounds all hold wins.
/// Mirrors _match/_first_match in scripts/platform_manifest.py.
template <typename T> T evaluate(const json& rules, int width, int height, const char* key) {
    const int narrow = std::min(width, height);
    for (const auto& rule : rules) {
        if (rule.contains("min_width") && width < rule["min_width"].get<int>())
            continue;
        if (rule.contains("max_width") && width > rule["max_width"].get<int>())
            continue;
        if (rule.contains("min_height") && height < rule["min_height"].get<int>())
            continue;
        if (rule.contains("max_height") && height > rule["max_height"].get<int>())
            continue;
        if (rule.contains("min_narrow") && narrow < rule["min_narrow"].get<int>())
            continue;
        if (rule.contains("max_narrow") && narrow > rule["max_narrow"].get<int>())
            continue;
        return rule[key].get<T>();
    }
    FAIL("no rule matched " << width << "x" << height << " - the ladder needs a catch-all");
    return T{};
}

/// Values that bracket every boundary the ladder names, plus a coarse sweep so a
/// rule reordering that misses the exact edges still shows up.
std::vector<int> probe_axis() {
    return {1,    100,  271,  272,  273,  319,  320,  321,  389,  390,  391,  439,  440,  441,
            459,  460,  461,  479,  480,  481,  499,  500,  501,  549,  550,  551,  599,  600,
            601,  699,  700,  701,  719,  720,  721,  799,  800,  801,  999,  1000, 1001, 1023,
            1024, 1025, 1099, 1100, 1101, 1279, 1280, 1281, 1439, 1920, 1921, 2560, 3840};
}

} // namespace

TEST_CASE("compiled splash ladder matches the manifest", "[assets][splash][manifest]") {
    const json manifest = load_manifest();
    const json& rules = manifest["size_classes"]["splash"];

    for (int w : probe_axis()) {
        for (int h : probe_axis()) {
            const std::string expected = evaluate<std::string>(rules, w, h, "class");
            INFO("resolution " << w << "x" << h);
            REQUIRE(std::string(get_splash_3d_size_name(w, h)) == expected);
            // The 2D logo uses the same ladder, so the two entry points must
            // never disagree about which class a panel is.
            REQUIRE(std::string(get_splash_size_name(w, h)) == expected);
        }
    }
}

TEST_CASE("manifest thresholds still match the breakpoint ladder", "[assets][splash][manifest]") {
    const json manifest = load_manifest();

    // The classes are UiBreakpoint tiers, so the manifest's max_narrow bounds are
    // the UI_BREAKPOINT_*_MAX macros. Changing a tier boundary in the header
    // without the manifest would leave packaging selecting the old one.
    const std::vector<std::pair<std::string, int>> expected = {
        {"micro", UI_BREAKPOINT_MICRO_MAX}, {"tiny", UI_BREAKPOINT_TINY_MAX},
        {"small", UI_BREAKPOINT_SMALL_MAX}, {"medium", UI_BREAKPOINT_MEDIUM_MAX},
        {"large", UI_BREAKPOINT_LARGE_MAX},
    };

    for (const auto& [name, bound] : expected) {
        bool found = false;
        for (const auto& rule : manifest["size_classes"]["splash"]) {
            if (rule.value("class", "") != name)
                continue;
            found = true;
            INFO("class " << name);
            REQUIRE(rule.contains("max_narrow"));
            REQUIRE(rule["max_narrow"].get<int>() == bound);
        }
        INFO("class " << name);
        REQUIRE(found);
    }
}

TEST_CASE("every class name is a name the layout system knows", "[assets][splash][manifest]") {
    const json manifest = load_manifest();

    for (const auto& rule : manifest["size_classes"]["splash"]) {
        const std::string cls = rule.value("class", "");
        if (cls == "ultrawide")
            continue; // An aspect, not a tier; deliberately outside the enum.
        INFO("class " << cls);
        REQUIRE(breakpoint_from_name(cls.c_str()) >= 0);
    }
}

TEST_CASE("the class a resolution gets round-trips to its own breakpoint tier",
          "[assets][splash][manifest]") {
    // The selector spells the tier names in its responsive_pick() call and
    // breakpoint_from_name() spells them in ui_breakpoint.h. Feeding one into
    // the other is what keeps those two lists the same list.
    struct Case {
        int w, h;
        UiBreakpoint expected;
    };
    const Case cases[] = {
        {480, 272, UiBreakpoint::Micro},  {480, 320, UiBreakpoint::Tiny},
        {480, 400, UiBreakpoint::Small},  {800, 480, UiBreakpoint::Medium},
        {1024, 600, UiBreakpoint::Large}, {1280, 720, UiBreakpoint::XLarge},
    };

    for (const Case& c : cases) {
        INFO("resolution " << c.w << "x" << c.h);
        const char* name = get_splash_3d_size_name(c.w, c.h);
        REQUIRE(breakpoint_from_name(name) == to_int(c.expected));
        // And the tier the layout system would independently pick.
        REQUIRE(breakpoint_for(std::min(c.w, c.h)) == c.expected);
    }

    SECTION("above xlarge the class clamps but is still a tier name") {
        const char* name = get_splash_3d_size_name(3840, 2160);
        REQUIRE(breakpoint_from_name(name) == to_int(UiBreakpoint::XLarge));
        REQUIRE(breakpoint_for(2160) == UiBreakpoint::XXLarge);
    }
}

TEST_CASE("compiled printer image ladder matches the manifest", "[assets][manifest]") {
    const json manifest = load_manifest();
    const json& rules = manifest["size_classes"]["printer_image"];

    for (int w : probe_axis()) {
        const int expected = evaluate<int>(rules, w, 0, "size");
        INFO("width " << w);
        REQUIRE(get_printer_image_size(w) == expected);
    }
}

TEST_CASE("composite heights match the manifest", "[assets][splash][manifest]") {
    const json manifest = load_manifest();
    const json& heights = manifest["size_classes"]["splash_composite_height"];

    REQUIRE_FALSE(heights.empty());
    for (auto it = heights.begin(); it != heights.end(); ++it) {
        INFO("class " << it.key());
        REQUIRE(get_splash_3d_target_height(it.key().c_str()) == it.value().get<int>());
    }

    SECTION("an unknown class reports 0 so callers fall back to runtime scaling") {
        REQUIRE(get_splash_3d_target_height("no-such-class") == 0);
    }
}

TEST_CASE("every class the ladder can return has a composite height",
          "[assets][splash][manifest]") {
    const json manifest = load_manifest();
    const json& heights = manifest["size_classes"]["splash_composite_height"];

    // A class with no height is the one case the overdraw guard cannot catch,
    // because it reads 0 as "unknown" and ships the canvas anyway.
    for (const auto& rule : manifest["size_classes"]["splash"]) {
        const std::string cls = rule.value("class", "");
        INFO("class " << cls);
        REQUIRE(heights.contains(cls));
        REQUIRE(get_splash_3d_target_height(cls.c_str()) > 0);
    }
}

TEST_CASE("a composited splash fits every panel the manifest describes",
          "[assets][splash][manifest]") {
    const json manifest = load_manifest();

    for (auto it = manifest["platforms"].begin(); it != manifest["platforms"].end(); ++it) {
        const json& panel = it.value()["panel"];
        if (panel.value("variable", false) || !panel.contains("width"))
            continue;

        int w = panel["width"].get<int>();
        int h = panel["height"].get<int>();
        const int rotate = panel.value("rotate", 0);
        if (rotate == 90 || rotate == 270)
            std::swap(w, h);

        const std::string cls = get_splash_3d_size_name(w, h);
        const int composite = get_splash_3d_target_height(cls.c_str());

        INFO("platform " << it.key() << " effective " << w << "x" << h << " class " << cls);
        REQUIRE(composite > 0);
        // Now that micro exists, no shipped panel should be overdrawn at all.
        // A class taller than the panel still degrades safely via the PNG
        // fallback, but it means a resolution has no canvas of its own.
        REQUIRE(composite <= h);
    }
}
