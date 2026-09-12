// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_install_roots.cpp
 * @brief Every platform's install root must be searchable
 *
 * `helix::kInstallRoots` is what a log tail, a crash-file read and a local
 * installer search use to find an install they did not start from. A platform
 * missing from it is a device whose logs and crash.txt cannot be recovered from
 * a debug bundle, and nothing about that failure is visible from the device -
 * the bundle uploads successfully with the field simply absent.
 *
 * `assets/config/platforms.json` is the list of platforms that ship, so it is
 * the thing to check against.
 */

#include "../../include/helix_install_roots.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

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

bool searchable(const std::string& root) {
    return std::any_of(std::begin(helix::kInstallRoots), std::end(helix::kInstallRoots),
                       [&](const char* known) { return root == known; });
}

bool is_state_root(const std::string& root) {
    return std::any_of(std::begin(helix::kStateRoots), std::end(helix::kStateRoots),
                       [&](const char* known) { return root == known; });
}

} // namespace

TEST_CASE("every platform's install root is searchable", "[install-roots][manifest]") {
    const json manifest = load_manifest();

    for (auto it = manifest["platforms"].begin(); it != manifest["platforms"].end(); ++it) {
        const json& storage = it.value()["storage"];
        const std::string root = storage.value("root", "");

        // A discovered root is resolved at runtime and has nothing fixed to list.
        if (root.empty() || root[0] != '/')
            continue;

        INFO("platform " << it.key() << " installs at " << root);
        REQUIRE(searchable(root));
    }
}

TEST_CASE("firmware-conditional install roots are searchable too", "[install-roots][manifest]") {
    const json manifest = load_manifest();

    for (auto it = manifest["platforms"].begin(); it != manifest["platforms"].end(); ++it) {
        const json& storage = it.value()["storage"];
        if (!storage.contains("root_by_firmware"))
            continue;

        for (auto fw = storage["root_by_firmware"].begin(); fw != storage["root_by_firmware"].end();
             ++fw) {
            // Values may describe a conditional ("X if that dir exists, else Y"),
            // so pull out every absolute path that names an install tree.
            const std::string value = fw.value().get<std::string>();
            size_t pos = 0;
            while ((pos = value.find('/', pos)) != std::string::npos) {
                size_t end = value.find_first_of(" ,", pos);
                std::string candidate = value.substr(pos, end - pos);
                pos = (end == std::string::npos) ? value.size() : end;
                if (candidate.size() > 12 &&
                    candidate.rfind("/helixscreen") == candidate.size() - 12) {
                    INFO("platform " << it.key() << " firmware " << fw.key() << " -> "
                                     << candidate);
                    REQUIRE(searchable(candidate));
                }
            }
        }
    }
}

TEST_CASE("the searchable roots hold no duplicates", "[install-roots]") {
    std::vector<std::string> roots(std::begin(helix::kInstallRoots),
                                   std::end(helix::kInstallRoots));
    std::vector<std::string> sorted = roots;
    std::sort(sorted.begin(), sorted.end());
    REQUIRE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    for (const char* home : helix::kHomeInstallRoots) {
        INFO("home root " << home);
        REQUIRE_FALSE(searchable(home));
    }
}

TEST_CASE("a superseded root stays searchable", "[install-roots][manifest]") {
    // A platform that has relocated leaves devices behind at the old path until
    // each one is migrated. Dropping it from the list is a device whose logs and
    // crash reports cannot be recovered from a debug bundle.
    const json manifest = load_manifest();

    for (auto it = manifest["platforms"].begin(); it != manifest["platforms"].end(); ++it) {
        const json& storage = it.value()["storage"];
        const std::string previous = storage.value("previous_root", "");
        if (previous.empty() || previous[0] != '/')
            continue;

        INFO("platform " << it.key() << " was installed at " << previous);
        REQUIRE(searchable(previous));
    }
}

TEST_CASE("every declared state root is one the app looks in", "[install-roots][manifest]") {
    const json manifest = load_manifest();

    for (auto it = manifest["platforms"].begin(); it != manifest["platforms"].end(); ++it) {
        const json& storage = it.value()["storage"];
        for (const char* key : {"state_root", "previous_state_root"}) {
            const std::string root = storage.value(key, "");
            if (root.empty() || root[0] != '/')
                continue;

            INFO("platform " << it.key() << " keeps " << key << " at " << root);
            REQUIRE(is_state_root(root));
        }
    }
}
