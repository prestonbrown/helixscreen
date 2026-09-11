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
 * Each case names the device rather than just the path, so a root going missing
 * fails with the printer it strands rather than with a string comparison.
 */

#include "../../include/helix_install_roots.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

bool searchable(const std::string& root) {
    return std::any_of(std::begin(helix::kInstallRoots), std::end(helix::kInstallRoots),
                       [&](const char* known) { return root == known; });
}

} // namespace

TEST_CASE("every shipped platform's install root is searchable", "[install-roots]") {
    struct Platform {
        const char* device;
        const char* root;
    };
    // Mirrors set_install_paths() in scripts/lib/installer/platform.sh and
    // HELIX_INSTALL_DIRS in scripts/lib/installer/common.sh.
    const Platform shipped[] = {
        {"Pi / K2 / AD5M Forge-X and KMod v00.06+", "/opt/helixscreen"},
        {"K1 and K1C", "/usr/data/helixscreen"},
        {"Snapmaker U1", "/userdata/helixscreen"},
        {"CC1 (COSMOS)", "/user-resource/helixscreen"},
        {"AD5M KMod v00.05 and earlier", "/root/printer_software/helixscreen"},
        {"AD5M ZMOD and AD5X", "/srv/helixscreen"},
        {"AD5X installs rooted at /data", "/data/helixscreen"},
    };

    for (const Platform& p : shipped) {
        INFO(p.device << " installs at " << p.root);
        REQUIRE(searchable(p.root));
    }
}

TEST_CASE("the searchable roots hold no duplicates", "[install-roots]") {
    std::vector<std::string> sorted(std::begin(helix::kInstallRoots),
                                    std::end(helix::kInstallRoots));
    std::sort(sorted.begin(), sorted.end());
    REQUIRE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    // Home-relative roots are a Pi fallback, not a platform's fixed root, and
    // must not be conflated with one.
    for (const char* home : helix::kHomeInstallRoots) {
        INFO("home root " << home);
        REQUIRE_FALSE(searchable(home));
    }
}
