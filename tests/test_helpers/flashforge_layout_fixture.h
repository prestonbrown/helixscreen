// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file flashforge_layout_fixture.h
 * @brief Fake-rootfs builders for the FlashForge mod-tree probe.
 *
 * make_flashforge_layout() builds, under a caller-supplied base dir, each filesystem
 * shape helix::flashforge_mod_tree() (and the launcher's
 * heap-diag gate) keys on. Every probe resolves under the returned root, so no
 * test ever creates /ZMOD or /usr/prog on the real build host.
 *
 * Shared by test_platform_info.cpp (the probe itself) and
 * test_log_collector.cpp (the mod_data log-path gating) so the two cannot
 * drift apart on what a layout means. The caller owns the base dir's
 * lifetime — pair with a temp-dir guard.
 */

#include <filesystem>
#include <fstream>
#include <string>

namespace helix::test {

namespace fs = std::filesystem;

//   zmod-marker : /ZMOD regular file (the Z-Mod indicator)
//   zmod-prog   : /usr/prog dir (FlashForge's own layout — present on any
//                 AD5X, modded or not; K1 has neither this nor the marker)
//   forgex      : /opt/config/mod/.shell/platform.sh and nothing else — a
//                 Forge-X chroot has no /ZMOD, no /usr/prog, no /usr/data
//   forgex-host : /usr/data/config/mod/.shell/platform.sh — the same mod tree
//                 seen from outside the chroot (the rig binds /usr/data at
//                 /opt, so exactly one spelling is reachable per side)
//   forgex-prog : forgex plus a /usr/prog dir — a Forge-X chroot built from a
//                 Z-Mod rootfs carries both, and Forge-X's own tree decides
//   plain       : empty root (Pi / dev box / K1: none of the markers)
//
// A non-empty `machine` also writes /usr/prog/app_startup.sh with `MACHINE=<machine>`,
// the line helix::flashforge_machine_id() reads, so a board test builds an AD5X,
// Creator 5 or Creator 5 Pro root in one call.
inline fs::path make_flashforge_layout(const fs::path& base, const std::string& layout,
                                       const std::string& machine = "") {
    fs::path root = base / layout;
    if (layout == "zmod-marker") {
        fs::create_directories(root);
        std::ofstream(root / "ZMOD") << "marker\n";
    } else if (layout == "zmod-prog") {
        fs::create_directories(root / "usr/prog");
    } else if (layout == "forgex") {
        fs::create_directories(root / "opt/config/mod/.shell");
        std::ofstream(root / "opt/config/mod/.shell/platform.sh") << "#!/bin/sh\n";
    } else if (layout == "forgex-prog") {
        fs::create_directories(root / "opt/config/mod/.shell");
        fs::create_directories(root / "usr/prog");
        std::ofstream(root / "opt/config/mod/.shell/platform.sh") << "#!/bin/sh\n";
    } else if (layout == "forgex-host") {
        fs::create_directories(root / "usr/data/config/mod/.shell");
        std::ofstream(root / "usr/data/config/mod/.shell/platform.sh") << "#!/bin/sh\n";
    } else {
        fs::create_directories(root);
    }
    if (!machine.empty()) {
        fs::create_directories(root / "usr/prog");
        std::ofstream(root / "usr/prog/app_startup.sh") << "MACHINE=" << machine << "\n";
    }
    return root;
}

} // namespace helix::test
