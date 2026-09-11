// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_prerender_size_class.cpp
 * @brief The compiled size-class ladder must agree with the manifest's
 *
 * assets/config/platforms.json carries the pre-render size-class ladder as data,
 * and scripts/platform_manifest.py evaluates it to decide which splash classes a
 * package contains. src/system/prerender_size_class.cpp is the compiled copy the
 * app and the splash binary use at runtime.
 *
 * Two implementations of one rule agree by convention until they silently do not,
 * so these tests evaluate the JSON rules directly and compare them against the
 * compiled selector across a resolution grid. Changing a threshold in one place
 * turns this red.
 */

#include "../../include/prerender_size_class.h"

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
    REQUIRE_FALSE(std::filesystem::exists(path) == false);
    std::ifstream f(path);
    REQUIRE(f.good());
    json j;
    f >> j;
    return j;
}

/// Evaluate one ladder from the manifest: first rule whose bounds all hold wins.
/// Mirrors _match/_first_match in scripts/platform_manifest.py.
template <typename T> T evaluate(const json& rules, int width, int height, const char* key) {
    for (const auto& rule : rules) {
        if (rule.contains("min_width") && width < rule["min_width"].get<int>())
            continue;
        if (rule.contains("max_width") && width > rule["max_width"].get<int>())
            continue;
        if (rule.contains("min_height") && height < rule["min_height"].get<int>())
            continue;
        if (rule.contains("max_height") && height > rule["max_height"].get<int>())
            continue;
        return rule[key].get<T>();
    }
    FAIL("no rule matched " << width << "x" << height << " - the ladder needs a catch-all");
    return T{};
}

/// Widths and heights that bracket every boundary the ladder names, plus a
/// coarse sweep so a rule reordering that misses the exact edges still shows up.
std::vector<int> probe_axis() {
    std::vector<int> v;
    for (int x : {1,    100,  271,  272,  273,  319,  320,  321,  379,  380,  381,  439,
                  440,  441,  479,  480,  481,  499,  500,  501,  599,  600,  601,  719,
                  720,  721,  799,  800,  801,  899,  900,  901,  1023, 1024, 1025, 1099,
                  1100, 1101, 1279, 1280, 1281, 1439, 1920, 1921, 2560, 3840})
        v.push_back(x);
    return v;
}

} // namespace

TEST_CASE("compiled 3D splash ladder matches the manifest", "[assets][splash][manifest]") {
    const json manifest = load_manifest();
    const json& rules = manifest["size_classes"]["splash_3d"];

    for (int w : probe_axis()) {
        for (int h : probe_axis()) {
            const std::string expected = evaluate<std::string>(rules, w, h, "class");
            const std::string actual = get_splash_3d_size_name(w, h);
            INFO("resolution " << w << "x" << h);
            REQUIRE(actual == expected);
        }
    }
}

TEST_CASE("compiled 2D splash ladder matches the manifest", "[assets][splash][manifest]") {
    const json manifest = load_manifest();
    const json& rules = manifest["size_classes"]["splash_2d"];

    for (int w : probe_axis()) {
        const std::string expected = evaluate<std::string>(rules, w, 0, "class");
        const std::string actual = get_splash_size_name(w);
        INFO("width " << w);
        REQUIRE(actual == expected);
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
    const json& heights = manifest["size_classes"]["splash_3d_composite_height"];

    REQUIRE_FALSE(heights.empty());
    for (auto it = heights.begin(); it != heights.end(); ++it) {
        INFO("class " << it.key());
        REQUIRE(get_splash_3d_target_height(it.key().c_str()) == it.value().get<int>());
    }

    SECTION("an unknown class reports 0 so callers fall back to runtime scaling") {
        REQUIRE(get_splash_3d_target_height("no-such-class") == 0);
    }
}

TEST_CASE("every manifest platform resolves to a class that has a render",
          "[assets][splash][manifest]") {
    const json manifest = load_manifest();
    const json& heights = manifest["size_classes"]["splash_3d_composite_height"];

    for (auto it = manifest["platforms"].begin(); it != manifest["platforms"].end(); ++it) {
        const json& panel = it.value()["panel"];
        if (panel.value("variable", false) || !panel.contains("width"))
            continue;

        int w = panel["width"].get<int>();
        int h = panel["height"].get<int>();
        const int rotate = panel.value("rotate", 0);
        if (rotate == 90 || rotate == 270)
            std::swap(w, h);

        INFO("platform " << it.key() << " effective " << w << "x" << h);
        const std::string cls = get_splash_3d_size_name(w, h);
        REQUIRE(heights.contains(cls));
        REQUIRE(get_printer_image_size(w) > 0);
    }
}

TEST_CASE("a composited splash never silently exceeds the panel it ships to",
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

        // A class taller than the panel is allowed only because
        // get_splash_3d_target_height lets the caller detect it and fall back to
        // a scaled PNG. What must never happen is a class whose height is
        // unknown, because then the overdraw ships as a clipped frame.
        INFO("platform " << it.key() << " effective " << w << "x" << h << " class " << cls);
        REQUIRE(composite > 0);
        if (composite > h) {
            WARN("platform " << it.key() << ": class " << cls << " composites at " << composite
                             << "px on a " << h << "px panel, so it renders via the PNG fallback");
        }
    }
}
